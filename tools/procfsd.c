/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfsd.c
 *
 * Userspace daemon for the procfs kext. It connects to the kext's PF_SYSTEM
 * kernel control and answers requests using libproc's proc_pidinfo() - the data
 * the kext cannot obtain on arm64 (the full proc_taskinfo, per-thread info).
 * Run as root via a LaunchDaemon; it reconnects automatically across kext
 * load/unload.
 *
 *   cc -O2 -Wall -o procfsd tools/procfsd.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <spawn.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/sys_domain.h>
#include <sys/kern_control.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/resource.h>
#include <sys/mount.h>
#include <dirent.h>
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/kext/KextManager.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/task.h>
#include <mach/thread_act.h>
#include <mach_debug/zone_info.h>
#if defined(__arm64__) || defined(__aarch64__)
#include <mach/arm/thread_status.h>
#elif defined(__x86_64__)
#include <mach/i386/thread_status.h>
#endif

#include "../include/fs/procfs/procfs_ctl.h"

extern char **environ;

/* Boot orchestration paths. */
#define PROCFS_BUNDLE_ID  "com.beako.filesystems.procfs"
#define PROCFS_KEXT_PATH  "/Library/Extensions/procfs.kext"  /* load by path at boot */
#define PROCFS_STAGER     "/usr/local/sbin/procfs_ksyms"   /* symbol-staging helper */
#define PROCFS_ARM_FLAG   "/var/db/procfs.enabled"         /* gate for auto-loading the kext */
#define PROCFS_LINUX_CONF "/var/db/procfs.linux"           /* persisted procfs.linux mode */

/* Run a program to completion (best-effort). */
static void
run_to_completion(const char *path, char *const argv[])
{
    pid_t pid;
    if (posix_spawn(&pid, path, NULL, NULL, argv, environ) == 0) {
        int status;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            /* retry */
        }
    }
}

/*
 * Boot bootstrap: stage the kernel symbols (so the kext's libklookup features
 * work), then - only if armed - load the kext. The arm flag is absent by
 * default so a kext panic during development cannot boot-loop the machine; the
 * operator creates PROCFS_ARM_FLAG to enable auto-load. Mounting is left to the
 * per-user login agent (the mount lives in the user's ~/proc).
 */
static void
procfsd_bootstrap(void)
{
    if (access(PROCFS_STAGER, X_OK) == 0) {
        fprintf(stderr, "procfsd: staging kernel symbols (%s)\n", PROCFS_STAGER);
        char *argv[] = { (char *)PROCFS_STAGER, NULL };
        run_to_completion(PROCFS_STAGER, argv);
    } else {
        fprintf(stderr, "procfsd: stager %s not found; skipping symbol staging\n", PROCFS_STAGER);
    }

    if (access(PROCFS_ARM_FLAG, F_OK) == 0) {
        fprintf(stderr, "procfsd: armed (%s present) - loading kext %s\n",
            PROCFS_ARM_FLAG, PROCFS_KEXT_PATH);
        /* Load by PATH, not by bundle id: `kmutil load -b <id>` frequently fails
         * at boot ("no such bundle" / not yet in the AuxKC), whereas loading the
         * installed bundle by path is what works interactively and here. */
        char *argv[] = { "/usr/bin/kmutil", "load", "-p", (char *)PROCFS_KEXT_PATH, NULL };
        run_to_completion("/usr/bin/kmutil", argv);
    } else {
        fprintf(stderr, "procfsd: not armed (%s absent); skipping kext auto-load. "
            "Create it to enable auto-load + login mount.\n", PROCFS_ARM_FLAG);
    }
}

#define PROCFS_MOUNTPOINT "/proc"

/*
 * Keep procfs mounted at /proc, as root, by calling mount(2) directly.
 *
 * We deliberately do NOT shell out to /sbin/mount: spawning a helper on every
 * retry forks a child per attempt, and when a mount keeps failing (e.g. the
 * kext is momentarily unloaded) that retry loop piles up processes until the
 * fork table is exhausted. A direct syscall has no such failure mode.
 *
 * The mount data is a page-sized, zeroed buffer rather than a bare 4-byte
 * pfsmount_args_t: a kext build can copyin() a larger struct than that, and the
 * over-read faults with EFAULT when the bytes past a small object are unmapped.
 * The kext interprets only the leading mnt_options (0 == defaults); the padding
 * keeps the copyin in mapped memory. (Same fix as mount_procfs.c - and why the
 * old "direct mount(2) returns EFAULT, so shell out" workaround is obsolete.)
 *
 * Gated on the arm flag; a no-op until /proc exists and while already mounted.
 */
static void
procfsd_try_mount(void)
{
    if (access(PROCFS_ARM_FLAG, F_OK) != 0) {
        return;
    }

    struct stat st;
    if (stat(PROCFS_MOUNTPOINT, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return;                 /* /proc not present yet (needs synthetic.conf + reboot) */
    }

    struct statfs sfs;
    if (statfs(PROCFS_MOUNTPOINT, &sfs) == 0 &&
        strcmp(sfs.f_fstypename, "procfs") == 0) {
        return;                 /* already mounted */
    }

    static unsigned char mount_data[4096];      /* zeroed (BSS); generous args padding */
    if (mount("procfs", PROCFS_MOUNTPOINT, 0, mount_data) == 0) {
        fprintf(stderr, "procfsd: mounted procfs at %s\n", PROCFS_MOUNTPOINT);
    } else {
        /* Usually benign: kext not loaded yet (ENOTSUP/ENODEV). Log and let the
         * next tick retry - no process is spawned either way. */
        fprintf(stderr, "procfsd: mount %s failed: %s\n",
            PROCFS_MOUNTPOINT, strerror(errno));
    }
}

static void *
procfsd_mount_thread(void *arg)
{
    (void)arg;
    for (;;) {
        procfsd_try_mount();
        sleep(3);
    }
    return NULL;
}

/* Connect to the kext's kernel control. Returns the socket fd or -1. */
static int
connect_ctl(void)
{
    int fd = socket(PF_SYSTEM, SOCK_DGRAM, SYSPROTO_CONTROL);
    if (fd < 0) {
        return -1;
    }

    struct ctl_info info;
    memset(&info, 0, sizeof(info));
    strlcpy(info.ctl_name, PROCFS_CTL_NAME, sizeof(info.ctl_name));
    if (ioctl(fd, CTLIOCGINFO, &info) < 0) {
        close(fd);
        return -1;      /* control not registered yet (kext not loaded) */
    }

    struct sockaddr_ctl addr;
    memset(&addr, 0, sizeof(addr));
    addr.sc_len      = sizeof(addr);
    addr.sc_family   = AF_SYSTEM;
    addr.ss_sysaddr  = AF_SYS_CONTROL;
    addr.sc_id       = info.ctl_id;
    addr.sc_unit     = 0;
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int
wait_connect(void)
{
    for (;;) {
        int fd = connect_ctl();
        if (fd >= 0) {
            return fd;
        }
        sleep(1);       /* wait for the kext to register the control */
    }
}

/*
 * ---- /proc/extensions: loaded kernel-extension listing --------------------
 *
 * The kext cannot enumerate loaded kexts (the private C++ OSKext class), so we
 * build a kextstat-style text listing from KextManagerCopyLoadedKextInfo() and
 * serve it to the kext in chunks. The blob is (re)built when a read starts at
 * offset 0 and cached for the remaining chunks of that read, so the listing is
 * self-consistent and IOKit is queried once per read.
 */
static char  *g_ext_blob = NULL;   /* macOS/kextstat format (/proc/extensions) */
static size_t g_ext_blob_len = 0;
static char  *g_mod_blob = NULL;   /* Linux format (/proc/modules) */
static size_t g_mod_blob_len = 0;
static char  *g_dev_blob = NULL;   /* Linux format (/proc/devices) */
static size_t g_dev_blob_len = 0;
static char  *g_alloc_blob = NULL; /* Linux format (/proc/allocinfo) */
static size_t g_alloc_blob_len = 0;
static char  *g_pci_blob = NULL;   /* Linux format (/proc/bus/pci/devices) */
static size_t g_pci_blob_len = 0;
static char  *g_fb_blob = NULL;    /* Linux format (/proc/fb) */
static size_t g_fb_blob_len = 0;

struct kextrow {
    long long tag, refs, addr, size, wired;
    char name[128];
    char vers[64];
};

static long long
cfnum_ll(CFDictionaryRef d, CFStringRef key)
{
    long long v = 0;
    CFNumberRef n = (CFNumberRef)CFDictionaryGetValue(d, key);
    if (n && CFGetTypeID(n) == CFNumberGetTypeID()) {
        CFNumberGetValue(n, kCFNumberLongLongType, &v);
    }
    return v;
}

static void
cfstr_copy(CFDictionaryRef d, CFStringRef key, char *out, size_t cap)
{
    out[0] = '\0';
    CFStringRef s = (CFStringRef)CFDictionaryGetValue(d, key);
    if (s && CFGetTypeID(s) == CFStringGetTypeID()) {
        CFStringGetCString(s, out, (CFIndex)cap, kCFStringEncodingUTF8);
    }
}

static int
kextrow_cmp(const void *a, const void *b)
{
    long long ta = ((const struct kextrow *)a)->tag;
    long long tb = ((const struct kextrow *)b)->tag;
    return (ta > tb) - (ta < tb);
}

/* Collect the loaded kexts into a sorted rows array (caller frees). Returns NULL
 * on failure; *out_n is the row count. */
static struct kextrow *
collect_kexts(CFIndex *out_n)
{
    *out_n = 0;
    CFDictionaryRef info = KextManagerCopyLoadedKextInfo(NULL, NULL);
    if (info == NULL) {
        return NULL;
    }
    CFIndex n = CFDictionaryGetCount(info);
    const void **vals = calloc((size_t)n, sizeof(*vals));
    struct kextrow *rows = calloc((size_t)n, sizeof(*rows));
    if (vals == NULL || rows == NULL) {
        free(vals); free(rows); CFRelease(info);
        return NULL;
    }
    CFDictionaryGetKeysAndValues(info, NULL, vals);

    CFIndex nrows = 0;
    for (CFIndex i = 0; i < n; i++) {
        CFDictionaryRef d = (CFDictionaryRef)vals[i];
        if (d == NULL || CFGetTypeID(d) != CFDictionaryGetTypeID()) {
            continue;
        }
        struct kextrow *r = &rows[nrows++];
        r->tag   = cfnum_ll(d, CFSTR("OSBundleLoadTag"));
        r->refs  = cfnum_ll(d, CFSTR("OSBundleRetainCount"));
        r->addr  = cfnum_ll(d, CFSTR("OSBundleLoadAddress"));
        r->size  = cfnum_ll(d, CFSTR("OSBundleLoadSize"));
        r->wired = cfnum_ll(d, CFSTR("OSBundleWiredSize"));
        cfstr_copy(d, CFSTR("CFBundleIdentifier"), r->name, sizeof(r->name));
        cfstr_copy(d, CFSTR("CFBundleVersion"), r->vers, sizeof(r->vers));
    }
    qsort(rows, (size_t)nrows, sizeof(*rows), kextrow_cmp);

    free(vals);
    CFRelease(info);
    *out_n = nrows;
    return rows;
}

/*
 * (Re)build a listing into *blobp. linux_fmt selects the Linux /proc/modules
 * format (name size refcount deps state address) versus the macOS kextstat-style
 * /proc/extensions format.
 */
static void
build_blob(int linux_fmt, char **blobp, size_t *lenp)
{
    free(*blobp);
    *blobp = NULL;
    *lenp = 0;

    CFIndex nrows = 0;
    struct kextrow *rows = collect_kexts(&nrows);
    if (rows == NULL) {
        return;
    }

    char  *buf = NULL;
    size_t sz  = 0;
    FILE  *f   = open_memstream(&buf, &sz);
    if (f != NULL) {
        if (!linux_fmt) {
            fprintf(f, "Index Refs Address            Size       Wired      "
                       "Name (Version)\n");
        }
        for (CFIndex i = 0; i < nrows; i++) {
            struct kextrow *r = &rows[i];
            if (linux_fmt) {
                /* Linux /proc/modules: name size refcount deps state address.
                 * macOS has no dependency string here, so deps is "-" and every
                 * loaded kext is "Live". */
                fprintf(f, "%s %lld %lld - Live 0x%llx\n",
                        r->name, r->size, r->refs, (unsigned long long)r->addr);
            } else {
                fprintf(f, "%5lld %4lld 0x%-16llx 0x%-8llx 0x%-8llx %s (%s)\n",
                        r->tag, r->refs, r->addr, r->size, r->wired,
                        r->name, r->vers);
            }
        }
        fclose(f);
        *blobp = buf;
        *lenp = sz;
    }
    free(rows);
}

/*
 * /proc/devices support. macOS has no named driver registry, so the char/block
 * major->name mapping is reconstructed from /dev: each device node's rdev gives
 * its type (char/block) and major number, and its name gives the driver family
 * (the name truncated at its first digit, so disk0/disk0s1 -> "disk"). One row
 * per major is kept, preferring the shortest family name.
 */
struct devrow {
    int  major;
    int  is_block;
    char name[64];
};

/* Truncate `in` at its first digit to get the driver-family name (disk0s1 ->
 * "disk", ttys003 -> "ttys", null -> "null"). Names starting with a digit keep
 * the whole name. */
static void
dev_family(const char *in, char *out, size_t cap)
{
    size_t i = 0;
    while (in[i] != '\0' && i + 1 < cap && !(in[i] >= '0' && in[i] <= '9')) {
        out[i] = in[i];
        i++;
    }
    out[i] = '\0';
    if (i == 0) {
        strlcpy(out, in, cap);      /* name began with a digit */
        return;
    }
    /* Trim a trailing instance separator left by the digit cut (aes_0 -> "aes",
     * apfs-raw-device.0 -> "apfs-raw-device"). */
    while (i > 1 && (out[i - 1] == '.' || out[i - 1] == '_' || out[i - 1] == '-')) {
        out[--i] = '\0';
    }
}

/* Insert (major,is_block) keeping one row per (type,major), preferring the
 * shortest family name. Linear scan - the device count is small. */
static void
dev_insert(struct devrow **rowsp, size_t *np, size_t *capp,
    int major_, int is_block, const char *fam)
{
    for (size_t i = 0; i < *np; i++) {
        struct devrow *r = &(*rowsp)[i];
        if (r->major == major_ && r->is_block == is_block) {
            if (strlen(fam) < strlen(r->name)) {
                strlcpy(r->name, fam, sizeof(r->name));
            }
            return;
        }
    }
    if (*np == *capp) {
        size_t ncap = (*capp == 0) ? 32 : *capp * 2;
        struct devrow *nr = realloc(*rowsp, ncap * sizeof(*nr));
        if (nr == NULL) {
            return;
        }
        *rowsp = nr;
        *capp = ncap;
    }
    struct devrow *r = &(*rowsp)[(*np)++];
    r->major = major_;
    r->is_block = is_block;
    strlcpy(r->name, fam, sizeof(r->name));
}

static int
devrow_cmp(const void *a, const void *b)
{
    int ma = ((const struct devrow *)a)->major;
    int mb = ((const struct devrow *)b)->major;
    return (ma > mb) - (ma < mb);
}

/* (Re)build the /proc/devices listing into *blobp. */
static void
build_devices_blob(char **blobp, size_t *lenp)
{
    free(*blobp);
    *blobp = NULL;
    *lenp = 0;

    DIR *dir = opendir("/dev");
    if (dir == NULL) {
        return;
    }

    struct devrow *rows = NULL;
    size_t nrows = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        char path[1100];
        snprintf(path, sizeof(path), "/dev/%s", de->d_name);
        struct stat st;
        if (lstat(path, &st) != 0) {
            continue;
        }
        int is_block;
        if (S_ISCHR(st.st_mode)) {
            is_block = 0;
        } else if (S_ISBLK(st.st_mode)) {
            is_block = 1;
        } else {
            continue;
        }
        char fam[64];
        dev_family(de->d_name, fam, sizeof(fam));
        dev_insert(&rows, &nrows, &cap, major(st.st_rdev), is_block, fam);
    }
    closedir(dir);

    qsort(rows, nrows, sizeof(*rows), devrow_cmp);

    char  *buf = NULL;
    size_t sz  = 0;
    FILE  *f   = open_memstream(&buf, &sz);
    if (f != NULL) {
        fprintf(f, "Character devices:\n");
        for (size_t i = 0; i < nrows; i++) {
            if (!rows[i].is_block) {
                fprintf(f, "%3d %s\n", rows[i].major, rows[i].name);
            }
        }
        fprintf(f, "\nBlock devices:\n");
        for (size_t i = 0; i < nrows; i++) {
            if (rows[i].is_block) {
                fprintf(f, "%3d %s\n", rows[i].major, rows[i].name);
            }
        }
        fclose(f);
        *blobp = buf;
        *lenp = sz;
    }
    free(rows);
}

/*
 * /proc/allocinfo support. Linux profiles allocations by code tag; macOS has no
 * such thing, so the nearest equivalent is the zone allocator (mach_zone_info,
 * the data behind zprint). Emit one row per zone - live bytes, live element
 * count, and the zone name in place of Linux's file:line func:name tag - sorted
 * by size descending. mach_zone_info needs the privileged host port (root).
 */
struct allocrow {
    uint64_t size;      /* live allocated bytes (count * elem_size) */
    uint64_t calls;     /* live element count */
    char     name[80];  /* zone name (ZONE_NAME_MAX_LEN) */
};

static int
allocrow_cmp(const void *a, const void *b)
{
    uint64_t sa = ((const struct allocrow *)a)->size;
    uint64_t sb = ((const struct allocrow *)b)->size;
    return (sa < sb) - (sa > sb);       /* descending */
}

static void
build_allocinfo_blob(char **blobp, size_t *lenp)
{
    free(*blobp);
    *blobp = NULL;
    *lenp = 0;

    host_priv_t hp = HOST_PRIV_NULL;
    if (host_get_host_priv_port(mach_host_self(), &hp) != KERN_SUCCESS ||
        hp == HOST_PRIV_NULL) {
        return;                         /* not privileged -> empty node */
    }

    mach_zone_name_t      *names = NULL;
    mach_msg_type_number_t namesCnt = 0;
    mach_zone_info_t      *info = NULL;
    mach_msg_type_number_t infoCnt = 0;
    if (mach_zone_info(hp, &names, &namesCnt, &info, &infoCnt) != KERN_SUCCESS) {
        return;
    }

    mach_msg_type_number_t n = (namesCnt < infoCnt) ? namesCnt : infoCnt;
    struct allocrow *rows = calloc(n, sizeof(*rows));
    if (rows != NULL) {
        for (mach_msg_type_number_t i = 0; i < n; i++) {
            rows[i].size  = info[i].mzi_count * info[i].mzi_elem_size;
            rows[i].calls = info[i].mzi_count;
            strlcpy(rows[i].name, names[i].mzn_name, sizeof(rows[i].name));
        }
        qsort(rows, n, sizeof(*rows), allocrow_cmp);

        char  *buf = NULL;
        size_t sz  = 0;
        FILE  *f   = open_memstream(&buf, &sz);
        if (f != NULL) {
            fprintf(f, "allocinfo - version: 1.0\n");
            fprintf(f, "#     <size>  <calls> <tag info>\n");
            for (mach_msg_type_number_t i = 0; i < n; i++) {
                fprintf(f, "%12llu %8llu zone:%s\n",
                    (unsigned long long)rows[i].size,
                    (unsigned long long)rows[i].calls, rows[i].name);
            }
            fclose(f);
            *blobp = buf;
            *lenp = sz;
        }
        free(rows);
    }

    vm_deallocate(mach_task_self(), (vm_address_t)names,
        namesCnt * sizeof(*names));
    vm_deallocate(mach_task_self(), (vm_address_t)info,
        infoCnt * sizeof(*info));
}

/*
 * /proc/bus/pci/devices support. macOS exposes PCI devices through the
 * IORegistry (IOPCIDevice), not a /proc table, so enumerate them and format the
 * Linux line for each. bus/dev/func come from the "pcidebug" property, the ids
 * from the little-endian "vendor-id"/"device-id" CFData, and the name from
 * "IOName". IRQ, base addresses and sizes are not read here and report 0.
 */
struct pcirow {
    uint32_t bus;       /* PCI bus number */
    uint32_t devfn;     /* (device << 3) | function */
    uint32_t vendor;
    uint32_t device;
    char     name[64];
};

/* Read a little-endian CFData registry property (vendor-id etc.) as a u32. */
static uint32_t
pci_cfdata_le32(io_registry_entry_t e, CFStringRef key)
{
    uint32_t v = 0;
    CFTypeRef p = IORegistryEntryCreateCFProperty(e, key, kCFAllocatorDefault, 0);
    if (p != NULL) {
        if (CFGetTypeID(p) == CFDataGetTypeID()) {
            CFDataRef d = (CFDataRef)p;
            CFIndex n = CFDataGetLength(d);
            const UInt8 *b = CFDataGetBytePtr(d);
            for (CFIndex i = 0; i < n && i < 4; i++) {
                v |= ((uint32_t)b[i]) << (8 * i);
            }
        }
        CFRelease(p);
    }
    return v;
}

/* Copy a CFString registry property into out (empty on failure). */
static void
pci_cfstr(io_registry_entry_t e, CFStringRef key, char *out, size_t cap)
{
    out[0] = '\0';
    CFTypeRef p = IORegistryEntryCreateCFProperty(e, key, kCFAllocatorDefault, 0);
    if (p != NULL) {
        if (CFGetTypeID(p) == CFStringGetTypeID()) {
            CFStringGetCString((CFStringRef)p, out, (CFIndex)cap, kCFStringEncodingUTF8);
        }
        CFRelease(p);
    }
}

static int
pcirow_cmp(const void *a, const void *b)
{
    const struct pcirow *ra = a, *rb = b;
    if (ra->bus != rb->bus) {
        return (ra->bus > rb->bus) - (ra->bus < rb->bus);
    }
    return (ra->devfn > rb->devfn) - (ra->devfn < rb->devfn);
}

static void
build_pci_blob(char **blobp, size_t *lenp)
{
    free(*blobp);
    *blobp = NULL;
    *lenp = 0;

    io_iterator_t it = MACH_PORT_NULL;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
            IOServiceMatching("IOPCIDevice"), &it) != KERN_SUCCESS) {
        return;
    }

    struct pcirow *rows = NULL;
    size_t nrows = 0, cap = 0;
    io_registry_entry_t e;
    while ((e = IOIteratorNext(it)) != MACH_PORT_NULL) {
        uint32_t bus = 0, dev = 0, func = 0;
        char dbg[64];
        pci_cfstr(e, CFSTR("pcidebug"), dbg, sizeof(dbg));   /* "bus:dev:func" */
        sscanf(dbg, "%u:%u:%u", &bus, &dev, &func);

        if (nrows == cap) {
            size_t ncap = (cap == 0) ? 32 : cap * 2;
            struct pcirow *nr = realloc(rows, ncap * sizeof(*nr));
            if (nr == NULL) {
                IOObjectRelease(e);
                break;
            }
            rows = nr;
            cap = ncap;
        }
        struct pcirow *r = &rows[nrows++];
        r->bus    = bus & 0xff;
        r->devfn  = ((dev & 0x1f) << 3) | (func & 0x07);
        r->vendor = pci_cfdata_le32(e, CFSTR("vendor-id")) & 0xffff;
        r->device = pci_cfdata_le32(e, CFSTR("device-id")) & 0xffff;
        pci_cfstr(e, CFSTR("IOName"), r->name, sizeof(r->name));

        IOObjectRelease(e);
    }
    IOObjectRelease(it);

    qsort(rows, nrows, sizeof(*rows), pcirow_cmp);

    char  *buf = NULL;
    size_t sz  = 0;
    FILE  *f   = open_memstream(&buf, &sz);
    if (f != NULL) {
        for (size_t i = 0; i < nrows; i++) {
            struct pcirow *r = &rows[i];
            fprintf(f, "%02x%02x\t%04x%04x\t%x",
                r->bus, r->devfn, r->vendor, r->device, 0 /* irq */);
            for (int j = 0; j < 7; j++) { fprintf(f, "\t%16llx", 0ULL); }  /* base addrs */
            for (int j = 0; j < 7; j++) { fprintf(f, "\t%16llx", 0ULL); }  /* sizes */
            fprintf(f, "\t%s\n", r->name);
        }
        fclose(f);
        *blobp = buf;
        *lenp = sz;
    }
    free(rows);
}

/*
 * /proc/fb support. macOS drives displays through IOKit framebuffers -
 * IOFramebuffer on Intel, IOMobileFramebuffer on Apple Silicon - so enumerate
 * both classes and emit one Linux "<index> <name>" line per device, using the
 * device's IORegistry name as the fix.id. Only one class matches on a given
 * machine, so a running index over both is contiguous.
 */
static void
build_fb_blob(char **blobp, size_t *lenp)
{
    free(*blobp);
    *blobp = NULL;
    *lenp = 0;

    char  *buf = NULL;
    size_t sz  = 0;
    FILE  *f   = open_memstream(&buf, &sz);
    if (f == NULL) {
        return;
    }

    static const char *classes[] = { "IOFramebuffer", "IOMobileFramebuffer" };
    int idx = 0;
    for (int c = 0; c < 2; c++) {
        io_iterator_t it = MACH_PORT_NULL;
        if (IOServiceGetMatchingServices(kIOMainPortDefault,
                IOServiceMatching(classes[c]), &it) != KERN_SUCCESS) {
            continue;
        }
        io_registry_entry_t e;
        while ((e = IOIteratorNext(it)) != MACH_PORT_NULL) {
            io_name_t name = "";
            if (IORegistryEntryGetName(e, name) != KERN_SUCCESS || name[0] == '\0') {
                strlcpy(name, classes[c], sizeof(name));
            }
            fprintf(f, "%d %s\n", idx++, name);
            IOObjectRelease(e);
        }
        IOObjectRelease(it);
    }

    fclose(f);
    *blobp = buf;
    *lenp = sz;
}

/*
 * Snapshot the system's power state for /proc/apm from IOKit power sources
 * (the data `pmset -g batt` reports). The kext turns these raw values into the
 * Linux APM line. Everything defaults to "unknown"; a machine with no power
 * source (a desktop) reports no battery and AC online.
 */
static void
fill_apm_info(struct procfs_apm_info *out)
{
    out->ac_online       = -1;
    out->battery_present = 0;
    out->charging        = 0;
    out->percentage      = -1;
    out->time_minutes    = -1;

    CFTypeRef blob = IOPSCopyPowerSourcesInfo();
    if (blob == NULL) {
        return;
    }
    CFArrayRef list = IOPSCopyPowerSourcesList(blob);
    if (list != NULL) {
        CFIndex n = CFArrayGetCount(list);
        if (n == 0) {
            out->ac_online = 1;         /* no power source -> desktop on AC */
        }
        for (CFIndex i = 0; i < n; i++) {
            CFDictionaryRef d = IOPSGetPowerSourceDescription(blob,
                CFArrayGetValueAtIndex(list, i));
            if (d == NULL) {
                continue;
            }
            out->battery_present = 1;

            CFStringRef state = CFDictionaryGetValue(d, CFSTR(kIOPSPowerSourceStateKey));
            if (state != NULL) {
                out->ac_online = CFEqual(state, CFSTR(kIOPSACPowerValue)) ? 1 : 0;
            }
            CFBooleanRef chg = CFDictionaryGetValue(d, CFSTR(kIOPSIsChargingKey));
            if (chg != NULL) {
                out->charging = CFBooleanGetValue(chg) ? 1 : 0;
            }
            int cur = 0, max = 0;
            CFNumberRef c = CFDictionaryGetValue(d, CFSTR(kIOPSCurrentCapacityKey));
            CFNumberRef m = CFDictionaryGetValue(d, CFSTR(kIOPSMaxCapacityKey));
            if (c != NULL) { CFNumberGetValue(c, kCFNumberIntType, &cur); }
            if (m != NULL) { CFNumberGetValue(m, kCFNumberIntType, &max); }
            if (max > 0) {
                out->percentage = (cur * 100) / max;
            }
            /* Time to empty when discharging, time to full when charging;
             * either is -1 while the estimate is still settling. */
            int t = -1;
            CFNumberRef te = CFDictionaryGetValue(d, out->charging ?
                CFSTR(kIOPSTimeToFullChargeKey) : CFSTR(kIOPSTimeToEmptyKey));
            if (te != NULL) { CFNumberGetValue(te, kCFNumberIntType, &t); }
            out->time_minutes = t;
            break;                      /* first (internal) battery */
        }
        CFRelease(list);
    }
    CFRelease(blob);
}

/* Copy the slice of `blob` at `off` into a response payload (chunked transfer). */
static void
blob_slice(const char *blob, size_t len, size_t off,
    void *payload, struct procfs_ctl_resp *resp)
{
    if (blob != NULL && off < len) {
        size_t avail = len - off;
        size_t n = (avail < PROCFS_CTL_MAXPAYLOAD) ? avail : PROCFS_CTL_MAXPAYLOAD;
        memcpy(payload, blob + off, n);
        resp->len = (uint32_t)n;
    } else {
        resp->len = 0;      /* past the end (or empty): final chunk */
    }
}

/*
 * Serve a PROCFS_REQ_REGS / PROCFS_REQ_FPREGS request. thread_get_state is
 * stripped from the arm64 kernelcache, so the kext cannot read register state;
 * we do it from userspace - get the target's task port, then read its
 * representative thread (threads[0]) machine state into the response payload.
 * task_for_pid is denied to root for Apple platform/hardened binaries (SIP/AMFI)
 * - those report EPERM, analogous to ptrace permissions on Linux.
 */
static void
procfsd_handle_regs(const struct procfs_ctl_req *req, struct procfs_ctl_resp *resp,
    void *payload)
{
    task_t task = TASK_NULL;
    if (task_for_pid(mach_task_self(), req->pid, &task) != KERN_SUCCESS) {
        resp->error = EPERM;
        return;
    }

    thread_act_array_t     threads = NULL;
    mach_msg_type_number_t tcount  = 0;
    if (task_threads(task, &threads, &tcount) != KERN_SUCCESS || tcount == 0) {
        resp->error = ESRCH;
        mach_port_deallocate(mach_task_self(), task);
        return;
    }

#if defined(__arm64__) || defined(__aarch64__)
    int flavor = (req->type == PROCFS_REQ_REGS) ? ARM_THREAD_STATE64 : ARM_NEON_STATE64;
    mach_msg_type_number_t cnt = (req->type == PROCFS_REQ_REGS)
        ? ARM_THREAD_STATE64_COUNT : ARM_NEON_STATE64_COUNT;
#elif defined(__x86_64__)
    int flavor = (req->type == PROCFS_REQ_REGS) ? x86_THREAD_STATE64 : x86_FLOAT_STATE64;
    mach_msg_type_number_t cnt = (req->type == PROCFS_REQ_REGS)
        ? x86_THREAD_STATE64_COUNT : x86_FLOAT_STATE64_COUNT;
#else
    int flavor = 0;
    mach_msg_type_number_t cnt = 0;
    resp->error = ENOTSUP;
#endif

#if defined(__arm64__) || defined(__aarch64__) || defined(__x86_64__)
    if ((size_t)cnt * sizeof(natural_t) <= PROCFS_CTL_MAXPAYLOAD) {
        mach_msg_type_number_t got = cnt;
        if (thread_get_state(threads[0], flavor, (thread_state_t)payload, &got) == KERN_SUCCESS) {
            resp->len = (uint32_t)((size_t)got * sizeof(natural_t));
        } else {
            resp->error = EIO;
        }
    } else {
        resp->error = EMSGSIZE;
    }
#endif

    for (mach_msg_type_number_t i = 0; i < tcount; i++) {
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), (vm_address_t)threads, tcount * sizeof(thread_act_t));
    mach_port_deallocate(mach_task_self(), task);
}

/*
 * Restore the persisted native/Linux presentation mode. The `procfs.linux`
 * sysctl lives in the kext and resets to its default (0, native) every time the
 * kext loads, so the GUI's choice is saved to PROCFS_LINUX_CONF and re-applied
 * here whenever the kext (re)appears - at boot and after any reload. Absent file
 * means "never set" and leaves the kext default untouched.
 */
static void
apply_persisted_linux_mode(void)
{
    FILE *f = fopen(PROCFS_LINUX_CONF, "r");
    if (f == NULL) {
        return;
    }
    int mode = 0;
    int got  = fscanf(f, "%d", &mode);
    fclose(f);
    if (got != 1) {
        return;
    }
    mode = (mode != 0) ? 1 : 0;

    /*
     * wait_connect() returns as soon as the kext registers its kernel control,
     * which the start routine does just before registering the procfs.linux
     * sysctl - so the oid may not exist for a few microseconds yet. Retry while
     * it is still absent (ENOENT), up to ~1s, rather than lose the restore to
     * that window.
     */
    for (int tries = 0; tries < 50; tries++) {
        if (sysctlbyname("procfs.linux", NULL, NULL, &mode, sizeof(mode)) == 0) {
            fprintf(stderr, "procfsd: restored procfs.linux=%d\n", mode);
            return;
        }
        if (errno != ENOENT) {
            break;              /* a real error, not "oid not ready yet" */
        }
        usleep(20000);          /* 20 ms */
    }
    fprintf(stderr, "procfsd: could not restore procfs.linux=%d: %s\n",
        mode, strerror(errno));
}

int
main(int argc, char **argv)
{
    /*
     * Defense in depth against a catastrophic mis-install. procfsd_bootstrap()
     * execs the symbol stager at PROCFS_STAGER (/usr/local/sbin/procfs_ksyms).
     * If a procfsd binary is ever installed there by mistake, exec'ing "the
     * stager" runs procfsd again, which bootstraps and execs "the stager"
     * again - unbounded recursion that fork-bombs the machine. Guard against it:
     * if we were invoked under the stager's name, we are NOT the stager, so
     * refuse to act as the daemon and exit instead of recursing.
     */
    const char *base = (argc > 0 && argv[0]) ? strrchr(argv[0], '/') : NULL;
    base = base ? base + 1 : (argc > 0 ? argv[0] : "");
    if (base && strcmp(base, "procfs_ksyms") == 0) {
        fprintf(stderr, "procfsd: invoked as the symbol stager (%s) but this is "
            "procfsd, not procfs_ksyms - refusing to run (mis-install?)\n",
            argv[0] ? argv[0] : "?");
        return 2;
    }

    procfsd_bootstrap();        /* stage symbols, gated kext load */

    /* Keep the console user's ~/proc mounted (root; gated by the arm flag). */
    pthread_t mt;
    if (pthread_create(&mt, NULL, procfsd_mount_thread, NULL) == 0) {
        pthread_detach(mt);
    }

    int fd = wait_connect();
    fprintf(stderr, "procfsd: connected to %s\n", PROCFS_CTL_NAME);
    apply_persisted_linux_mode();   /* restore saved native/Linux mode */

    for (;;) {
        uint8_t rbuf[sizeof(struct procfs_ctl_req)];
        ssize_t n = recv(fd, rbuf, sizeof(rbuf), 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);                  /* kext unloaded / socket error */
            fd = wait_connect();
            fprintf(stderr, "procfsd: reconnected\n");
            apply_persisted_linux_mode();   /* kext reloaded -> re-apply saved mode */
            continue;
        }
        if (n < (ssize_t)sizeof(struct procfs_ctl_req)) {
            continue;
        }

        struct procfs_ctl_req *req = (struct procfs_ctl_req *)rbuf;
        if (req->magic != PROCFS_CTL_MAGIC) {
            continue;
        }

        uint8_t sbuf[sizeof(struct procfs_ctl_resp) + PROCFS_CTL_MAXPAYLOAD];
        struct procfs_ctl_resp *resp = (struct procfs_ctl_resp *)sbuf;
        resp->magic = PROCFS_CTL_MAGIC;
        resp->seq   = req->seq;
        resp->error = 0;
        resp->len   = 0;
        void *payload = sbuf + sizeof(*resp);

        switch (req->type) {
        case PROCFS_REQ_TASKINFO: {
            struct proc_taskinfo ti;
            int r = proc_pidinfo(req->pid, PROC_PIDTASKINFO, 0, &ti, sizeof(ti));
            if (r == (int)sizeof(ti)) {
                memcpy(payload, &ti, sizeof(ti));
                resp->len = sizeof(ti);
            } else {
                resp->error = (r < 0) ? errno : ESRCH;
            }
            break;
        }
        case PROCFS_REQ_THREADINFO: {
            /* arg is the kext's tid (== thread_id). PROC_PIDTHREADID64INFO keys
             * on thread_id, so the tid is the handle directly - no mapping. */
            struct proc_threadinfo thi;
            int r = proc_pidinfo(req->pid, PROC_PIDTHREADID64INFO, req->arg, &thi, sizeof(thi));
            if (r == (int)sizeof(thi)) {
                memcpy(payload, &thi, sizeof(thi));
                resp->len = sizeof(thi);
            } else {
                resp->error = (r < 0) ? errno : ESRCH;
            }
            break;
        }
        case PROCFS_REQ_VMSTAT: {
            vm_statistics64_data_t vm;
            mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
            if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                    (host_info64_t)&vm, &cnt) == KERN_SUCCESS) {
                size_t n = (size_t)cnt * sizeof(integer_t);
                if (n > PROCFS_CTL_MAXPAYLOAD) {
                    n = PROCFS_CTL_MAXPAYLOAD;
                }
                memcpy(payload, &vm, n);
                resp->len = (uint32_t)n;
            } else {
                resp->error = EIO;
            }
            break;
        }
        case PROCFS_REQ_LOADAVG: {
            double la[3] = { 0, 0, 0 };
            (void)getloadavg(la, 3);
            uint32_t out[3];
            for (int i = 0; i < 3; i++) {
                out[i] = (uint32_t)(la[i] * 100.0 + 0.5);
            }
            memcpy(payload, out, sizeof(out));
            resp->len = sizeof(out);
            break;
        }
        case PROCFS_REQ_SYSCTL: {
            /* Userspace sysctlbyname serves every oid (no CTLFLAG_KERN gate),
             * returning the raw value bytes; the kext formats by the oid type. */
            req->name[sizeof(req->name) - 1] = '\0';
            size_t len = PROCFS_CTL_MAXPAYLOAD;
            if (sysctlbyname(req->name, payload, &len, NULL, 0) == 0) {
                resp->len = (uint32_t)len;
            } else {
                resp->error = errno;
            }
            break;
        }
        case PROCFS_REQ_FDLIST: {
            /* Open file descriptors of the process. proc_pidinfo writes
             * count*sizeof(proc_fdinfo) bytes and naturally caps at the buffer
             * size, so the list is truncated to what fits in one payload. */
            int r = proc_pidinfo(req->pid, PROC_PIDLISTFDS, 0, payload, PROCFS_CTL_MAXPAYLOAD);
            if (r >= 0) {
                resp->len = (uint32_t)r;
            } else {
                resp->error = errno;
            }
            break;
        }
        case PROCFS_REQ_FDINFO: {
            struct vnode_fdinfowithpath vi;
            int r = proc_pidfdinfo(req->pid, (int)req->arg, PROC_PIDFDVNODEPATHINFO,
                                   &vi, sizeof(vi));
            if (r == (int)sizeof(vi)) {
                memcpy(payload, &vi, sizeof(vi));
                resp->len = sizeof(vi);
            } else {
                resp->error = (r < 0) ? errno : EBADF;
            }
            break;
        }
        case PROCFS_REQ_FDSOCKET: {
            struct socket_fdinfo si;
            int r = proc_pidfdinfo(req->pid, (int)req->arg, PROC_PIDFDSOCKETINFO,
                                   &si, sizeof(si));
            if (r == (int)sizeof(si)) {
                memcpy(payload, &si, sizeof(si));
                resp->len = sizeof(si);
            } else {
                resp->error = (r < 0) ? errno : EBADF;
            }
            break;
        }
        case PROCFS_REQ_EXTENSIONS: {
            /* arg = byte offset. Rebuild the listing at the start of a read,
             * then return the slice at this offset (chunked). */
            size_t off = (size_t)req->arg;
            if (off == 0) {
                build_blob(0, &g_ext_blob, &g_ext_blob_len);
            }
            blob_slice(g_ext_blob, g_ext_blob_len, off, payload, resp);
            break;
        }
        case PROCFS_REQ_MODULES: {
            size_t off = (size_t)req->arg;
            if (off == 0) {
                build_blob(1, &g_mod_blob, &g_mod_blob_len);
            }
            blob_slice(g_mod_blob, g_mod_blob_len, off, payload, resp);
            break;
        }
        case PROCFS_REQ_DEVICES: {
            size_t off = (size_t)req->arg;
            if (off == 0) {
                build_devices_blob(&g_dev_blob, &g_dev_blob_len);
            }
            blob_slice(g_dev_blob, g_dev_blob_len, off, payload, resp);
            break;
        }
        case PROCFS_REQ_ALLOCINFO: {
            size_t off = (size_t)req->arg;
            if (off == 0) {
                build_allocinfo_blob(&g_alloc_blob, &g_alloc_blob_len);
            }
            blob_slice(g_alloc_blob, g_alloc_blob_len, off, payload, resp);
            break;
        }
        case PROCFS_REQ_APM: {
            struct procfs_apm_info ai;
            fill_apm_info(&ai);
            memcpy(payload, &ai, sizeof(ai));
            resp->len = sizeof(ai);
            break;
        }
        case PROCFS_REQ_PCIDEVICES: {
            size_t off = (size_t)req->arg;
            if (off == 0) {
                build_pci_blob(&g_pci_blob, &g_pci_blob_len);
            }
            blob_slice(g_pci_blob, g_pci_blob_len, off, payload, resp);
            break;
        }
        case PROCFS_REQ_FBDEVICES: {
            size_t off = (size_t)req->arg;
            if (off == 0) {
                build_fb_blob(&g_fb_blob, &g_fb_blob_len);
            }
            blob_slice(g_fb_blob, g_fb_blob_len, off, payload, resp);
            break;
        }
        case PROCFS_REQ_RUSAGE: {
            /* Actual disk I/O bytes for the process (Linux /proc/<pid>/io
             * read_bytes / write_bytes). macOS has no read()/write() char or
             * syscall counters, so only these two are reported. */
            rusage_info_current ri;
            if (proc_pid_rusage(req->pid, RUSAGE_INFO_CURRENT, (rusage_info_t *)&ri) == 0) {
                uint64_t io[2] = { ri.ri_diskio_bytesread, ri.ri_diskio_byteswritten };
                memcpy(payload, io, sizeof(io));
                resp->len = sizeof(io);
            } else {
                resp->error = errno ? errno : ESRCH;
            }
            break;
        }
        case PROCFS_REQ_REGS:
        case PROCFS_REQ_FPREGS:
            /* thread_get_state is stripped from the kernelcache, so the kext
             * cannot read register state itself. We do it from userspace: get
             * the target's task port and read its representative thread's state.
             * task_for_pid is denied for Apple platform/hardened binaries even
             * to root (SIP/AMFI) - those report EPERM, like ptrace on Linux. */
            procfsd_handle_regs(req, resp, payload);
            break;
        default:
            resp->error = EINVAL;
            break;
        }

        (void)send(fd, sbuf, sizeof(*resp) + resp->len, 0);
    }
    return 0;
}
