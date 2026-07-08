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
#include <limits.h>
#include <ctype.h>
#include <sys/mman.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/fat.h>
#include <mach/machine.h>
#include <libkern/OSByteOrder.h>
#include <libproc.h>
#include "kallsyms_extra.h"     /* generated: non-exported names for /proc/kallsyms */
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/kext/KextManager.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/mach_vm.h>
#include <mach/vm_region.h>
#include <mach/processor_info.h>
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
#define PROCFS_ARM_FLAG   "/var/db/procfs.enabled"         /* gate for auto-loading the kext */
#define PROCFS_LINUX_CONF "/var/db/procfs.linux"           /* persisted procfs.linux mode */
#define PROCFS_LINUX_VER_CONF "/var/db/procfs.linux_version" /* persisted spoof-version idx */

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
 * Boot bootstrap: only if armed, load the kext. The arm flag is absent by
 * default so a kext panic during development cannot boot-loop the machine; the
 * operator creates PROCFS_ARM_FLAG to enable auto-load. Mounting is left to the
 * per-user login agent (the mount lives in the user's ~/proc).
 */
static void
procfsd_bootstrap(void)
{
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
static char  *g_misc_blob = NULL;  /* Linux format (/proc/misc) */
static size_t g_misc_blob_len = 0;
static char  *g_alloc_blob = NULL; /* Linux format (/proc/allocinfo) */
static size_t g_alloc_blob_len = 0;
static char  *g_vmalloc_blob = NULL; /* Linux format (/proc/vmallocinfo) */
static size_t g_vmalloc_blob_len = 0;
static char  *g_pci_blob = NULL;   /* Linux format (/proc/bus/pci/devices) */
static size_t g_pci_blob_len = 0;
static char  *g_scsi_blob = NULL;  /* Linux format (/proc/scsi/scsi) */
static size_t g_scsi_blob_len = 0;
static char  *g_shm_blob = NULL;   /* Linux format (/proc/sysvipc/shm) */
static size_t g_shm_blob_len = 0;
static char  *g_sem_blob = NULL;   /* Linux format (/proc/sysvipc/sem) */
static size_t g_sem_blob_len = 0;
static char  *g_msg_blob = NULL;   /* Linux format (/proc/sysvipc/msg) */
static size_t g_msg_blob_len = 0;
static char  *g_slab_blob = NULL;  /* Linux format (/proc/slabinfo) */
static size_t g_slab_blob_len = 0;
static char  *g_kmsg_blob = NULL;  /* kernel message buffer snapshot (/proc/kmsg) */
static size_t g_kmsg_blob_len = 0;
static char  *g_lastkmsg_blob = NULL; /* newest kernel panic report (/proc/last_kmsg) */
static size_t g_lastkmsg_blob_len = 0;
static char  *g_ksyms_blob = NULL; /* kernel symbol table, nm-style (/proc/ksyms) */
static size_t g_ksyms_blob_len = 0;
static char  *g_kallsyms_blob = NULL; /* ksyms + non-exported names (/proc/kallsyms) */
static size_t g_kallsyms_blob_len = 0;
static char  *g_fb_blob = NULL;    /* Linux format (/proc/fb) */
static size_t g_fb_blob_len = 0;
static char  *g_nfs_blob = NULL;   /* NFS exports (/proc/fs/nfs/exports) */
static size_t g_nfs_blob_len = 0;
static char  *g_intr_blob = NULL;  /* Linux format (/proc/interrupts) */
static size_t g_intr_blob_len = 0;
static char  *g_tty_blob = NULL;   /* Linux format (/proc/tty/drivers) */
static size_t g_tty_blob_len = 0;

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
 * /proc/misc support. Linux's /proc/misc lists the miscellaneous character-
 * device drivers registered under the shared misc major (10), one "<minor>
 * <name>" line each. macOS has no misc-device framework, but it does have plenty
 * of miscellaneous single-purpose character devices (autofs, bpf, dtrace,
 * fsevents, klog, oslog, pf, auditpipe, ...); like /proc/devices we recover them
 * from /dev. We list one row per driver family (the device name truncated at its
 * first digit), excluding the families that belong to other subsystems - block/
 * disk, tty/pty, and the standard mem/std streams (which Linux keeps under their
 * own majors, not misc). The minor is the family's lowest device minor.
 */
struct miscrow {
    int  minor;
    char name[64];
};

/* True for device families that are NOT miscellaneous (handled by tty, disk, or
 * mem majors on Linux, so excluded from /proc/misc). */
static int
misc_excluded(const char *fam)
{
    static const char *const prefixes[] = {
        "tty", "pty", "cu", "disk", "rdisk", NULL
    };
    static const char *const exact[] = {
        "null", "zero", "mem", "kmem", "random", "urandom", "console", "ptmx",
        "stdin", "stdout", "stderr", "fd", NULL
    };
    for (int i = 0; prefixes[i] != NULL; i++) {
        if (strncmp(fam, prefixes[i], strlen(prefixes[i])) == 0) {
            return 1;
        }
    }
    for (int i = 0; exact[i] != NULL; i++) {
        if (strcmp(fam, exact[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static int
miscrow_cmp(const void *a, const void *b)
{
    const struct miscrow *ra = a, *rb = b;
    if (ra->minor != rb->minor) {
        return (ra->minor > rb->minor) - (ra->minor < rb->minor);
    }
    return strcmp(ra->name, rb->name);
}

static void
build_misc_blob(char **blobp, size_t *lenp)
{
    free(*blobp);
    *blobp = NULL;
    *lenp = 0;

    DIR *dir = opendir("/dev");
    if (dir == NULL) {
        return;
    }

    struct miscrow *rows = NULL;
    size_t nrows = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        char path[1100];
        snprintf(path, sizeof(path), "/dev/%s", de->d_name);
        struct stat st;
        if (lstat(path, &st) != 0 || !S_ISCHR(st.st_mode)) {
            continue;                   /* misc devices are character devices */
        }

        char fam[64];
        dev_family(de->d_name, fam, sizeof(fam));
        if (fam[0] == '\0' || misc_excluded(fam)) {
            continue;
        }

        int dmin = minor(st.st_rdev);
        /* One row per family, keeping the lowest minor. */
        size_t j;
        for (j = 0; j < nrows; j++) {
            if (strcmp(rows[j].name, fam) == 0) {
                if (dmin < rows[j].minor) {
                    rows[j].minor = dmin;
                }
                break;
            }
        }
        if (j == nrows) {
            if (nrows == cap) {
                size_t ncap = (cap == 0) ? 32 : cap * 2;
                struct miscrow *nr = realloc(rows, ncap * sizeof(*nr));
                if (nr == NULL) {
                    break;
                }
                rows = nr;
                cap = ncap;
            }
            rows[nrows].minor = dmin;
            strlcpy(rows[nrows].name, fam, sizeof(rows[nrows].name));
            nrows++;
        }
    }
    closedir(dir);

    qsort(rows, nrows, sizeof(*rows), miscrow_cmp);

    char  *buf = NULL;
    size_t sz  = 0;
    FILE  *f   = open_memstream(&buf, &sz);
    if (f != NULL) {
        for (size_t i = 0; i < nrows; i++) {
            fprintf(f, "%3d %s\n", rows[i].minor, rows[i].name);
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
 * /proc/slabinfo support. Linux reports the SLAB/SLUB allocator's per-cache
 * statistics; macOS has no slab allocator, but its zone allocator (zalloc) is
 * the direct analog - each zone is a cache of fixed-size elements, exactly like
 * a slab cache. mach_zone_info() (the data behind zprint) supplies each zone's
 * live-object count, current/allocation sizes and element size, which map onto
 * the slabinfo columns as follows:
 *
 *   active_objs   = mzi_count                       (elements in use)
 *   num_objs      = mzi_cur_size / mzi_elem_size    (elements backed by memory)
 *   objsize       = mzi_elem_size
 *   objperslab    = mzi_alloc_size / mzi_elem_size  (elements per alloc chunk)
 *   pagesperslab  = mzi_alloc_size / page_size
 *   active/num_slabs = mzi_cur_size / mzi_alloc_size
 *
 * The SLUB tunables (limit/batchcount/sharedfactor) and sharedavail have no zone
 * equivalent and are reported as 0. mach_zone_info needs the privileged host
 * port (root); without it the node is empty. Version line matches Linux's 2.1.
 */
static void
build_slabinfo_blob(char **blobp, size_t *lenp)
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

    uint64_t pagesize = (uint64_t)getpagesize();
    mach_msg_type_number_t n = (namesCnt < infoCnt) ? namesCnt : infoCnt;

    char  *buf = NULL;
    size_t sz  = 0;
    FILE  *f   = open_memstream(&buf, &sz);
    if (f != NULL) {
        fprintf(f, "slabinfo - version: 2.1\n");
        fprintf(f, "# name            <active_objs> <num_objs> <objsize>"
            " <objperslab> <pagesperslab>"
            " : tunables <limit> <batchcount> <sharedfactor>"
            " : slabdata <active_slabs> <num_slabs> <sharedavail>\n");
        for (mach_msg_type_number_t i = 0; i < n; i++) {
            uint64_t elem  = info[i].mzi_elem_size;
            uint64_t alloc = info[i].mzi_alloc_size;
            uint64_t cur   = info[i].mzi_cur_size;
            uint64_t active_objs   = info[i].mzi_count;
            uint64_t num_objs      = elem  ? cur   / elem  : 0;
            uint64_t objperslab    = elem  ? alloc / elem  : 0;
            uint64_t pagesperslab  = pagesize ? alloc / pagesize : 0;
            uint64_t num_slabs     = alloc ? cur   / alloc : 0;

            /* Linux left-justifies the name to a 17-char field. */
            fprintf(f, "%-17s %6llu %6llu %6llu %4llu %4llu"
                " : tunables %4d %4d %4d"
                " : slabdata %6llu %6llu %6d\n",
                names[i].mzn_name,
                (unsigned long long)active_objs,
                (unsigned long long)num_objs,
                (unsigned long long)elem,
                (unsigned long long)objperslab,
                (unsigned long long)pagesperslab,
                0, 0, 0,
                (unsigned long long)num_slabs,
                (unsigned long long)num_slabs,
                0);
        }
        fclose(f);
        *blobp = buf;
        *lenp = sz;
    }

    vm_deallocate(mach_task_self(), (vm_address_t)names,
        namesCnt * sizeof(*names));
    vm_deallocate(mach_task_self(), (vm_address_t)info,
        infoCnt * sizeof(*info));
}

/*
 * /proc/kmsg support. Linux's /proc/kmsg is the kernel log ring buffer (the data
 * behind dmesg). macOS keeps the same classic kernel printf buffer; its size is
 * kern.msgbuf and its contents are read with proc_kmsgbuf() - the root-only
 * libproc call dmesg(1) uses (an unprivileged reader gets EPERM, which is why
 * this goes through the daemon). We take a snapshot: read the whole buffer once
 * and hand it back as text. This is not Linux's blocking/consuming interface -
 * repeated reads return the current buffer, like `dmesg` rather than a drain.
 */
static void
build_kmsg_blob(char **blobp, size_t *lenp)
{
    free(*blobp);
    *blobp = NULL;
    *lenp = 0;

    int    bufsize = 0;
    size_t osize   = sizeof(bufsize);
    if (sysctlbyname("kern.msgbuf", &bufsize, &osize, NULL, 0) != 0 ||
        bufsize <= 0) {
        return;
    }

    char *buf = malloc((size_t)bufsize);
    if (buf == NULL) {
        return;
    }

    /* proc_kmsgbuf() fills buf with the message buffer and returns the byte
     * count (0 or negative on failure, e.g. without root). */
    int n = proc_kmsgbuf(buf, (uint32_t)bufsize);
    if (n > 0) {
        *blobp = buf;
        *lenp  = (size_t)((n <= bufsize) ? n : bufsize);
    } else {
        free(buf);
    }
}

/*
 * /proc/last_kmsg support. Linux/Android's /proc/last_kmsg is the kernel log
 * from before the last reboot, preserved in a persistent RAM console. macOS has
 * no such per-reboot RAM console, but it does persist one cross-boot kernel log:
 * the kernel panic report. So we return the newest panic report from
 * /Library/Logs/DiagnosticReports (the "panic-full-*.panic" files, world-
 * readable), which is exactly what last_kmsg is used for - diagnosing the prior
 * crash. Empty when the machine has no panic report (a clean history), matching
 * a Linux box with no preserved previous-boot log.
 */
#define PROCFS_PANIC_DIR "/Library/Logs/DiagnosticReports"

static void
build_last_kmsg_blob(char **blobp, size_t *lenp)
{
    free(*blobp);
    *blobp = NULL;
    *lenp = 0;

    DIR *d = opendir(PROCFS_PANIC_DIR);
    if (d == NULL) {
        return;
    }

    char   newest[PATH_MAX] = { 0 };
    time_t newest_mtime = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;               /* skip ., .., and .contents.panic index */
        }
        size_t nl = strlen(de->d_name);
        if (nl < 6 || strcmp(de->d_name + nl - 6, ".panic") != 0) {
            continue;               /* kernel panics are the *.panic reports */
        }

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", PROCFS_PANIC_DIR, de->d_name);
        struct stat st;
        if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        if (st.st_mtime >= newest_mtime) {
            newest_mtime = st.st_mtime;
            strlcpy(newest, path, sizeof(newest));
        }
    }
    closedir(d);

    if (newest[0] == '\0') {
        return;                     /* no panic report -> empty node */
    }

    struct stat st;
    if (stat(newest, &st) != 0 || st.st_size <= 0) {
        return;
    }
    char *buf = malloc((size_t)st.st_size);
    if (buf == NULL) {
        return;
    }
    FILE *f = fopen(newest, "rb");
    if (f == NULL) {
        free(buf);
        return;
    }
    size_t got = fread(buf, 1, (size_t)st.st_size, f);
    fclose(f);

    if (got == 0) {
        free(buf);
        return;
    }
    *blobp = buf;
    *lenp  = got;
}

/*
 * /proc/ksyms support. Linux's /proc/ksyms (and the modern /proc/kallsyms) list
 * the kernel symbol table as "address type name" lines. macOS does not expose
 * the running kernel's symbols at runtime (KASLR/SIP), but it ships the exact
 * on-disk kernel image whose symbol table we can read: on Apple Silicon each SoC
 * has its own thin arm64e Mach-O at /System/Library/Kernels/kernel.release.<soc>
 * (the running one is named in kern.version, e.g. RELEASE_ARM64_T8103 -> t8103);
 * on Intel it is the thin x86_64 /System/Library/Kernels/kernel. We parse that
 * image's LC_SYMTAB and emit nm-style lines. These are the static (unslid) link
 * addresses - KASLR still holds - and are already public in the world-readable
 * kernel image. The kext zeroes the address column for non-root readers, mirror-
 * ing Linux's kptr_restrict. Only symbols whose Mach-O cputype matches the
 * daemon's (== the running kernel's) architecture are emitted.
 */
#if defined(__arm64__)
#define PROCFS_NATIVE_CPUTYPE   CPU_TYPE_ARM64
#elif defined(__x86_64__)
#define PROCFS_NATIVE_CPUTYPE   CPU_TYPE_X86_64
#else
#define PROCFS_NATIVE_CPUTYPE   0
#endif

/* Path of the on-disk image for the running kernel, arch/SoC-specific. */
static void
ksyms_kernel_path(char *out, size_t outsz)
{
    out[0] = '\0';

    /* The running kernel names its SoC in kern.version ("...ARM64_T8103"). The
     * matching image is kernel.release.<soc> (lowercased). */
    char   ver[512];
    size_t vs = sizeof(ver);
    if (sysctlbyname("kern.version", ver, &vs, NULL, 0) == 0) {
        char *m = strstr(ver, "ARM64_");
        if (m != NULL) {
            m += 6;
            char soc[32];
            int  j = 0;
            while (*m != '\0' && isalnum((unsigned char)*m) &&
                   j < (int)sizeof(soc) - 1) {
                soc[j++] = (char)tolower((unsigned char)*m++);
            }
            soc[j] = '\0';
            if (j > 0) {
                snprintf(out, outsz,
                    "/System/Library/Kernels/kernel.release.%s", soc);
                struct stat st;
                if (stat(out, &st) == 0) {
                    return;
                }
                out[0] = '\0';
            }
        }
    }

    /* Fallback: the plain kernel image (native on Intel; on Apple Silicon it is
     * the x86_64 build, which ksyms_emit_macho() rejects on arch mismatch). */
    snprintf(out, outsz, "/System/Library/Kernels/kernel");
}

/* Emit nm-style "address type name" lines for a thin Mach-O in [base, base+size). */
static void
ksyms_emit_macho(const uint8_t *base, size_t size, FILE *out)
{
    if (size < sizeof(struct mach_header_64)) {
        return;
    }
    const struct mach_header_64 *mh = (const void *)base;
    if (mh->magic != MH_MAGIC_64 ||
        mh->cputype != PROCFS_NATIVE_CPUTYPE) {
        return;                         /* wrong format or wrong architecture */
    }

    /* First pass: map 1-based section index -> nm type letter, find LC_SYMTAB. */
    char section_type[256];
    memset(section_type, 't', sizeof(section_type));
    int nsect = 0;
    const struct symtab_command *st = NULL;

    const uint8_t *p   = base + sizeof(*mh);
    const uint8_t *end = base + size;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (p + sizeof(struct load_command) > end) {
            break;
        }
        const struct load_command *lc = (const void *)p;
        if (lc->cmdsize < sizeof(*lc) || p + lc->cmdsize > end) {
            break;
        }
        if (lc->cmd == LC_SYMTAB) {
            st = (const void *)p;
        } else if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg = (const void *)p;
            const struct section_64 *sec = (const void *)(sg + 1);
            for (uint32_t s = 0; s < sg->nsects; s++) {
                nsect++;
                if (nsect < 256) {
                    char t = 't';
                    if (strncmp(sec[s].segname, "__DATA", 6) == 0) {
                        t = (strstr(sec[s].sectname, "bss") != NULL ||
                             strstr(sec[s].sectname, "common") != NULL)
                            ? 'b' : 'd';
                    }
                    section_type[nsect] = t;
                }
            }
        }
        p += lc->cmdsize;
    }

    if (st == NULL || st->nsyms == 0 ||
        (size_t)st->symoff + (size_t)st->nsyms * sizeof(struct nlist_64) > size ||
        (size_t)st->stroff + st->strsize > size) {
        return;
    }

    const struct nlist_64 *syms = (const void *)(base + st->symoff);
    const char            *strs = (const char *)(base + st->stroff);

    for (uint32_t i = 0; i < st->nsyms; i++) {
        const struct nlist_64 *n = &syms[i];
        if ((n->n_type & N_STAB) != 0) {
            continue;                   /* debug symbol */
        }
        uint32_t strx = n->n_un.n_strx;
        if (strx == 0 || strx >= st->strsize) {
            continue;
        }
        const char *name = strs + strx;
        if (name[0] == '\0') {
            continue;
        }

        char    type;
        uint8_t ntype = n->n_type & N_TYPE;
        if (ntype == N_UNDF || ntype == N_PBUD) {
            continue;                   /* undefined - not listed */
        } else if (ntype == N_ABS) {
            type = 'a';
        } else if (ntype == N_INDR) {
            type = 'i';
        } else if (ntype == N_SECT) {
            type = section_type[n->n_sect];   /* n_sect is uint8_t, 0..255 */
        } else {
            type = '?';
        }
        if ((n->n_type & N_EXT) != 0) {
            type = (char)toupper((unsigned char)type);
        }

        fprintf(out, "%016llx %c %s\n",
            (unsigned long long)n->n_value, type, name);
    }
}

/* Emit the running kernel image's symbol table (nm-style) to out. Shared by
 * /proc/ksyms and /proc/kallsyms. */
static void
ksyms_emit_kernel(FILE *out)
{
    char path[PATH_MAX];
    ksyms_kernel_path(path, sizeof(path));

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return;
    }
    uint8_t *base = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        return;
    }

    uint32_t magic = *(const uint32_t *)base;
    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        /* Universal image: pick the slice matching the running arch. */
        const struct fat_header *fh = (const void *)base;
        uint32_t nfat = OSSwapBigToHostInt32(fh->nfat_arch);
        const struct fat_arch *fa = (const void *)(base + sizeof(*fh));
        for (uint32_t i = 0; i < nfat; i++) {
            cpu_type_t ct = (cpu_type_t)OSSwapBigToHostInt32(fa[i].cputype);
            if (ct == PROCFS_NATIVE_CPUTYPE) {
                uint32_t off = OSSwapBigToHostInt32(fa[i].offset);
                uint32_t asz = OSSwapBigToHostInt32(fa[i].size);
                if ((size_t)off + asz <= (size_t)st.st_size) {
                    ksyms_emit_macho(base + off, asz, out);
                }
                break;
            }
        }
    } else if (magic == MH_MAGIC_64) {
        ksyms_emit_macho(base, (size_t)st.st_size, out);
    }

    munmap(base, (size_t)st.st_size);
}

static void
build_ksyms_blob(char **blobp, size_t *lenp)
{
    free(*blobp);
    *blobp = NULL;
    *lenp = 0;

    char  *buf = NULL;
    size_t sz  = 0;
    FILE  *out = open_memstream(&buf, &sz);
    if (out != NULL) {
        ksyms_emit_kernel(out);
        fclose(out);
    }

    if (buf != NULL && sz > 0) {
        *blobp = buf;
        *lenp  = sz;
    } else {
        free(buf);
    }
}

/*
 * /proc/kallsyms - the modern, fuller symbol table: the exported kernel symbols
 * (with their real addresses, as in /proc/ksyms) plus the non-exported symbol
 * names recovered from the XNU source (kallsyms_extra.h). macOS ships the arm64
 * kernel stripped of its local symbols, so those names have no address in the
 * running image and are emitted with address 0 - which is also what the kext's
 * kptr_restrict shows a non-root reader for every symbol. Format matches Linux
 * ("address type name [module]"); macOS exposes no loadable-module symbol
 * addresses, so no line carries a [module] tag.
 */
static void
build_kallsyms_blob(char **blobp, size_t *lenp)
{
    free(*blobp);
    *blobp = NULL;
    *lenp = 0;

    char  *buf = NULL;
    size_t sz  = 0;
    FILE  *out = open_memstream(&buf, &sz);
    if (out != NULL) {
        ksyms_emit_kernel(out);
        for (unsigned i = 0; i < procfs_kallsyms_extra_count; i++) {
            fprintf(out, "0000000000000000 t %s\n", procfs_kallsyms_extra[i]);
        }
        fclose(out);
    }

    if (buf != NULL && sz > 0) {
        *blobp = buf;
        *lenp  = sz;
    } else {
        free(buf);
    }
}

/*
 * Kernel symbol lookup for /proc/<pid>/wchan. The kext reads a blocked thread's
 * continuation, un-slides it (vm_kernel_unslide_or_perm_external) to the link
 * address, and asks us to name it. We keep a value-sorted table of the kernel
 * image's __TEXT symbols (the same unslid addresses) and return the nearest
 * preceding one, so a continuation (a function entry) resolves to its function.
 * Built lazily from the same image /proc/ksyms parses.
 */
struct ksym_ent {
    uint64_t val;
    char     name[96];
};
static struct ksym_ent *g_ksym_tab = NULL;
static size_t           g_ksym_n   = 0;

static int
ksym_ent_cmp(const void *a, const void *b)
{
    uint64_t va = ((const struct ksym_ent *)a)->val;
    uint64_t vb = ((const struct ksym_ent *)b)->val;
    return (va > vb) - (va < vb);
}

/* Collect __TEXT symbols (value + name) from a thin Mach-O into a growable array. */
static void
ksym_collect_macho(const uint8_t *base, size_t size,
    struct ksym_ent **tab, size_t *n, size_t *cap)
{
    if (size < sizeof(struct mach_header_64)) {
        return;
    }
    const struct mach_header_64 *mh = (const void *)base;
    if (mh->magic != MH_MAGIC_64 || mh->cputype != PROCFS_NATIVE_CPUTYPE) {
        return;
    }

    char is_text[256];
    memset(is_text, 0, sizeof(is_text));
    int nsect = 0;
    const struct symtab_command *st = NULL;
    const uint8_t *p = base + sizeof(*mh), *end = base + size;
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (p + sizeof(struct load_command) > end) {
            break;
        }
        const struct load_command *lc = (const void *)p;
        if (lc->cmdsize < sizeof(*lc) || p + lc->cmdsize > end) {
            break;
        }
        if (lc->cmd == LC_SYMTAB) {
            st = (const void *)p;
        } else if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg = (const void *)p;
            const struct section_64 *sec = (const void *)(sg + 1);
            for (uint32_t s = 0; s < sg->nsects; s++) {
                nsect++;
                if (nsect < 256) {
                    is_text[nsect] = (strncmp(sec[s].segname, "__TEXT", 6) == 0);
                }
            }
        }
        p += lc->cmdsize;
    }
    if (st == NULL || st->nsyms == 0 ||
        (size_t)st->symoff + (size_t)st->nsyms * sizeof(struct nlist_64) > size ||
        (size_t)st->stroff + st->strsize > size) {
        return;
    }
    const struct nlist_64 *syms = (const void *)(base + st->symoff);
    const char            *strs = (const char *)(base + st->stroff);
    for (uint32_t i = 0; i < st->nsyms; i++) {
        const struct nlist_64 *nn = &syms[i];
        if ((nn->n_type & N_STAB) != 0 || (nn->n_type & N_TYPE) != N_SECT) {
            continue;
        }
        if (!is_text[nn->n_sect]) {
            continue;                   /* text symbols only */
        }
        uint32_t strx = nn->n_un.n_strx;
        if (strx == 0 || strx >= st->strsize) {
            continue;
        }
        const char *name = strs + strx;
        if (name[0] == '\0') {
            continue;
        }
        if (*n == *cap) {
            size_t nc = (*cap == 0) ? 4096 : *cap * 2;
            struct ksym_ent *nt = realloc(*tab, nc * sizeof(**tab));
            if (nt == NULL) {
                return;
            }
            *tab = nt;
            *cap = nc;
        }
        (*tab)[*n].val = nn->n_value;
        strlcpy((*tab)[*n].name, name, sizeof((*tab)[*n].name));
        (*n)++;
    }
}

/*
 * Symbol source for /proc/<pid>/wchan. The stripped running-kernel image exports
 * only ~6900 symbols (no local functions), and thread continuations are almost
 * always local (wait1continue, ipc_mqueue_receive_continue, ...). So we prefer
 * the matching Kernel Debug Kit's dSYM, which carries the full symbol table at
 * the same (release) layout the running kernel uses: /Library/Developer/KDKs/
 * *<build>*.kdk/System/Library/Kernels/kernel.release.<soc>.dSYM/.../DWARF/
 * kernel.release.<soc>. Falls back to the stripped image (KPI symbols only) when
 * no matching KDK is installed.
 */
static void
ksyms_wchan_source_path(char *out, size_t cap)
{
    out[0] = '\0';

    /* SoC from kern.version (ARM64_T8103 -> t8103), running build from
     * kern.osversion (e.g. 25F84). */
    char   soc[32] = { 0 };
    char   ver[512];
    size_t vs = sizeof(ver);
    if (sysctlbyname("kern.version", ver, &vs, NULL, 0) == 0) {
        char *m = strstr(ver, "ARM64_");
        if (m != NULL) {
            m += 6;
            int j = 0;
            while (*m != '\0' && isalnum((unsigned char)*m) &&
                   j < (int)sizeof(soc) - 1) {
                soc[j++] = (char)tolower((unsigned char)*m++);
            }
            soc[j] = '\0';
        }
    }
    char   build[64] = { 0 };
    size_t bs = sizeof(build);
    (void)sysctlbyname("kern.osversion", build, &bs, NULL, 0);

    if (soc[0] != '\0' && build[0] != '\0') {
        DIR *d = opendir("/Library/Developer/KDKs");
        if (d != NULL) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                if (strstr(de->d_name, build) == NULL) {
                    continue;           /* KDK for a different build */
                }
                char p[PATH_MAX];
                snprintf(p, sizeof(p),
                    "/Library/Developer/KDKs/%s/System/Library/Kernels/"
                    "kernel.release.%s.dSYM/Contents/Resources/DWARF/"
                    "kernel.release.%s", de->d_name, soc, soc);
                struct stat st;
                if (stat(p, &st) == 0) {
                    strlcpy(out, p, cap);
                    break;
                }
            }
            closedir(d);
        }
    }

    if (out[0] == '\0') {
        ksyms_kernel_path(out, cap);    /* fallback: stripped kernel image */
    }
}

static void
ksym_table_build(void)
{
    if (g_ksym_tab != NULL) {
        return;                         /* built once */
    }
    char path[PATH_MAX];
    ksyms_wchan_source_path(path, sizeof(path));
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return;
    }
    uint8_t *b = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (b == MAP_FAILED) {
        return;
    }

    struct ksym_ent *tab = NULL;
    size_t n = 0, cap = 0;
    uint32_t magic = *(const uint32_t *)b;
    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        const struct fat_header *fh = (const void *)b;
        uint32_t nf = OSSwapBigToHostInt32(fh->nfat_arch);
        const struct fat_arch *fa = (const void *)(b + sizeof(*fh));
        for (uint32_t i = 0; i < nf; i++) {
            if ((cpu_type_t)OSSwapBigToHostInt32(fa[i].cputype) == PROCFS_NATIVE_CPUTYPE) {
                uint32_t off = OSSwapBigToHostInt32(fa[i].offset);
                uint32_t sz  = OSSwapBigToHostInt32(fa[i].size);
                if ((size_t)off + sz <= (size_t)st.st_size) {
                    ksym_collect_macho(b + off, sz, &tab, &n, &cap);
                }
                break;
            }
        }
    } else if (magic == MH_MAGIC_64) {
        ksym_collect_macho(b, (size_t)st.st_size, &tab, &n, &cap);
    }
    munmap(b, (size_t)st.st_size);

    if (tab != NULL && n > 0) {
        qsort(tab, n, sizeof(*tab), ksym_ent_cmp);
        g_ksym_tab = tab;
        g_ksym_n = n;
    } else {
        free(tab);
    }
}

/* Name the nearest preceding kernel text symbol for an unslid address (empty if
 * none, or if the address is implausibly far past the last symbol). */
static void
ksym_lookup(uint64_t addr, char *out, size_t cap)
{
    out[0] = '\0';
    ksym_table_build();
    if (g_ksym_n == 0) {
        return;
    }
    size_t lo = 0, hi = g_ksym_n;       /* find first entry with val > addr */
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (g_ksym_tab[mid].val <= addr) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0) {
        return;                         /* addr precedes all symbols */
    }
    const struct ksym_ent *e = &g_ksym_tab[lo - 1];
    if (addr - e->val > 0x100000ULL) {
        return;                         /* >1MB past a symbol -> not a real match */
    }
    strlcpy(out, e->name, cap);
}

/* Address (in the wchan symbol source) of a named symbol, or 0. The kext uses a
 * reference symbol (proc_pid) to compute the running kernel's slide relative to
 * this symbol source. */
static uint64_t
ksym_addr_of(const char *name)
{
    ksym_table_build();
    for (size_t i = 0; i < g_ksym_n; i++) {
        if (strcmp(g_ksym_tab[i].name, name) == 0) {
            return g_ksym_tab[i].val;
        }
    }
    return 0;
}

/*
 * /proc/vmallocinfo support. Linux lists the kernel's vmalloc'd (virtually-
 * contiguous, non-zone) areas: "<addr-range> <size> <caller> <flags>". macOS has
 * no vmalloc; the nearest analog is XNU's kernel VM allocations tagged by site,
 * which mach_memory_info() reports (the data behind `zprint -v`). Emit one line
 * per site with a nonzero size, sorted by size descending. macOS does not expose
 * per-site kernel virtual addresses to userspace, so the address range is
 * 0x0-0x0 and the size + site name (or VM tag) carry the real information.
 * mach_memory_info needs the privileged host port (root).
 */
struct vmallocrow {
    uint64_t size;
    uint64_t mapped;
    char     name[MACH_MEMORY_INFO_NAME_MAX_LEN];
};

static int
vmallocrow_cmp(const void *a, const void *b)
{
    uint64_t sa = ((const struct vmallocrow *)a)->size;
    uint64_t sb = ((const struct vmallocrow *)b)->size;
    return (sa < sb) - (sa > sb);       /* descending */
}

static void
build_vmallocinfo_blob(char **blobp, size_t *lenp)
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
    mach_memory_info_t    *meminfo = NULL;
    mach_msg_type_number_t meminfoCnt = 0;
    if (mach_memory_info(hp, &names, &namesCnt, &info, &infoCnt,
            &meminfo, &meminfoCnt) != KERN_SUCCESS) {
        return;
    }

    struct vmallocrow     *rows  = calloc(meminfoCnt, sizeof(*rows));
    mach_msg_type_number_t nrows = 0;
    if (rows != NULL) {
        for (mach_msg_type_number_t i = 0; i < meminfoCnt; i++) {
            if (meminfo[i].size == 0) {
                continue;
            }
            rows[nrows].size   = meminfo[i].size;
            rows[nrows].mapped = meminfo[i].mapped;
            if (meminfo[i].name[0] != '\0') {
                strlcpy(rows[nrows].name, meminfo[i].name, sizeof(rows[nrows].name));
            } else {
                snprintf(rows[nrows].name, sizeof(rows[nrows].name),
                    "tag-%u", (unsigned)meminfo[i].tag);
            }
            nrows++;
        }
        qsort(rows, nrows, sizeof(*rows), vmallocrow_cmp);

        char  *buf = NULL;
        size_t sz  = 0;
        FILE  *f   = open_memstream(&buf, &sz);
        if (f != NULL) {
            for (mach_msg_type_number_t i = 0; i < nrows; i++) {
                fprintf(f, "0x0000000000000000-0x0000000000000000 %10llu %s vmalloc",
                    (unsigned long long)rows[i].size, rows[i].name);
                if (rows[i].mapped > 0) {
                    fprintf(f, " pages=%llu",
                        (unsigned long long)(rows[i].mapped / vm_page_size));
                }
                fprintf(f, "\n");
            }
            fclose(f);
            *blobp = buf;
            *lenp = sz;
        }
        free(rows);
    }

    vm_deallocate(mach_task_self(), (vm_address_t)names, namesCnt * sizeof(*names));
    vm_deallocate(mach_task_self(), (vm_address_t)info, infoCnt * sizeof(*info));
    vm_deallocate(mach_task_self(), (vm_address_t)meminfo,
        meminfoCnt * sizeof(*meminfo));
}

/*
 * /proc/bus/pci/devices support. macOS exposes PCI devices through the
 * IORegistry (IOPCIDevice), not a /proc table, so enumerate them and format the
 * Linux line for each. bus/dev/func come from the "pcidebug" property, the ids
 * from the little-endian "vendor-id"/"device-id" CFData, the name from "IOName",
 * and the base addresses/sizes (BAR0..5 + expansion ROM, with region flags) from
 * the "assigned-addresses" property (see pci_read_bars). IRQ reports 0: macOS
 * routes PCI interrupts (MSI/GIC) with no legacy per-device IRQ line to report.
 */
struct pcirow {
    uint32_t bus;       /* PCI bus number */
    uint32_t devfn;     /* (device << 3) | function */
    uint32_t vendor;
    uint32_t device;
    uint64_t base[7];   /* BAR0..5 + expansion ROM, low bits = region flags */
    uint64_t size[7];   /* matching region sizes */
    char     name[64];
};

/* Little-endian 32-bit load from a byte pointer. */
static uint32_t
pci_le32(const UInt8 *b)
{
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

/*
 * Fill base[]/size[] (BAR0..5 + expansion ROM) from an IOPCIDevice's IOKit
 * "assigned-addresses" property - an array of OpenFirmware PCI address tuples
 * (five 32-bit cells each: phys.hi with the BAR register in its low byte and the
 * space/prefetch flags up top, then the 64-bit address and 64-bit size, low cell
 * first). The base value carries the Linux/PCI BAR low-bit flags (bit0 = I/O
 * space, bit2 = 64-bit memory, bit3 = prefetchable) as /proc/bus/pci expects.
 */
static void
pci_read_bars(io_registry_entry_t e, uint64_t base[7], uint64_t size[7])
{
    for (int i = 0; i < 7; i++) {
        base[i] = 0;
        size[i] = 0;
    }
    CFTypeRef p = IORegistryEntryCreateCFProperty(e, CFSTR("assigned-addresses"),
        kCFAllocatorDefault, 0);
    if (p == NULL) {
        return;
    }
    if (CFGetTypeID(p) == CFDataGetTypeID()) {
        CFDataRef d = (CFDataRef)p;
        CFIndex n = CFDataGetLength(d);
        const UInt8 *b = CFDataGetBytePtr(d);
        for (CFIndex off = 0; off + 20 <= n; off += 20) {
            uint32_t c0 = pci_le32(b + off);
            uint64_t addr = (uint64_t)pci_le32(b + off + 4) |
                            ((uint64_t)pci_le32(b + off + 8) << 32);
            uint64_t sz   = (uint64_t)pci_le32(b + off + 12) |
                            ((uint64_t)pci_le32(b + off + 16) << 32);
            uint32_t reg      = c0 & 0xff;
            uint32_t space    = (c0 >> 24) & 0x3;   /* 1=I/O 2=mem32 3=mem64 */
            uint32_t prefetch = (c0 >> 30) & 0x1;

            int idx = -1;
            if (reg >= 0x10 && reg <= 0x24 && ((reg - 0x10) & 0x3) == 0) {
                idx = (int)((reg - 0x10) / 4);      /* BAR0..5 */
            } else if (reg == 0x30) {
                idx = 6;                            /* expansion ROM */
            }
            if (idx < 0) {
                continue;
            }
            uint64_t flags = 0;
            if (space == 1) {
                flags |= 0x1;                       /* PCI_BASE_ADDRESS_SPACE_IO */
            } else {
                if (space == 3) {
                    flags |= 0x4;                   /* MEM_TYPE_64 */
                }
                if (prefetch) {
                    flags |= 0x8;                   /* MEM_PREFETCH */
                }
            }
            base[idx] = addr | flags;
            size[idx] = sz;
        }
    }
    CFRelease(p);
}

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
        pci_read_bars(e, r->base, r->size);
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
            for (int j = 0; j < 7; j++) {
                fprintf(f, "\t%16llx", (unsigned long long)r->base[j]);
            }
            for (int j = 0; j < 7; j++) {
                fprintf(f, "\t%16llx", (unsigned long long)r->size[j]);
            }
            fprintf(f, "\t%s\n", r->name);
        }
        fclose(f);
        *blobp = buf;
        *lenp = sz;
    }
    free(rows);
}

/* Linux /proc/scsi/scsi peripheral-device-type name for a SCSI PDT code. */
static const char *
scsi_type_name(int pdt)
{
    switch (pdt) {
    case 0x00: return "Direct-Access";
    case 0x01: return "Sequential-Access";
    case 0x02: return "Printer";
    case 0x03: return "Processor";
    case 0x04: return "WORM";
    case 0x05: return "CD-ROM";
    case 0x06: return "Scanner";
    case 0x07: return "Optical Device";
    case 0x08: return "Medium Changer";
    case 0x0e: return "RBC Device";
    default:   return "Unknown";
    }
}

/*
 * /proc/scsi/scsi support. Linux lists attached SCSI logical units. macOS
 * represents SCSI-protocol peripherals (USB/external/Thunderbolt storage that
 * goes through the SCSI Architecture Model) as IOSCSIPeripheralDeviceType* nubs
 * carrying the SCSI INQUIRY properties; enumerate those and emit the Linux
 * format. Internal NVMe is not SCSI and is intentionally excluded. The
 * "Attached devices:" header is always emitted, so with no SCSI devices the
 * output matches a SCSI-less Linux host. Channel/Id/Lun are 00 (macOS does not
 * expose the SCSI address the same way).
 */
static void
build_scsi_blob(char **blobp, size_t *lenp)
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
    fprintf(f, "Attached devices:\n");

    static const char *const classes[] = {
        "IOSCSIPeripheralDeviceType00",  /* direct-access (disk)   */
        "IOSCSIPeripheralDeviceType05",  /* CD/DVD                 */
        "IOSCSIPeripheralDeviceType07",  /* optical memory         */
        "IOSCSIPeripheralDeviceType0E",  /* simplified block (RBC) */
    };
    int host = 0;
    for (size_t c = 0; c < sizeof(classes) / sizeof(classes[0]); c++) {
        io_iterator_t it = MACH_PORT_NULL;
        if (IOServiceGetMatchingServices(kIOMainPortDefault,
                IOServiceMatching(classes[c]), &it) != KERN_SUCCESS) {
            continue;
        }
        io_registry_entry_t e;
        while ((e = IOIteratorNext(it)) != MACH_PORT_NULL) {
            char vendor[64] = "", model[64] = "", rev[64] = "";
            pci_cfstr(e, CFSTR("Vendor Identification"),  vendor, sizeof(vendor));
            pci_cfstr(e, CFSTR("Product Identification"), model,  sizeof(model));
            pci_cfstr(e, CFSTR("Product Revision Level"), rev,    sizeof(rev));

            int pdt = 0;
            CFTypeRef p = IORegistryEntryCreateCFProperty(e,
                CFSTR("Peripheral Device Type"), kCFAllocatorDefault, 0);
            if (p != NULL) {
                if (CFGetTypeID(p) == CFNumberGetTypeID()) {
                    CFNumberGetValue((CFNumberRef)p, kCFNumberIntType, &pdt);
                }
                CFRelease(p);
            }

            fprintf(f, "Host: scsi%d Channel: 00 Id: 00 Lun: 00\n", host++);
            fprintf(f, "  Vendor: %-8s Model: %-16s Rev: %-4s\n", vendor, model, rev);
            fprintf(f, "  Type:   %-33s ANSI  SCSI revision: 05\n",
                scsi_type_name(pdt));

            IOObjectRelease(e);
        }
        IOObjectRelease(it);
    }

    fclose(f);
    *blobp = buf;
    *lenp = sz;
}

/*
 * /proc/sysvipc/{shm,sem,msg} support. macOS implements System V IPC but has no
 * Linux SHM_STAT/MSG_STAT/SEM_STAT enumeration; instead it exposes the same
 * kern.sysv.ipcs.{shm,sem,msg} sysctl that ipcs(1) uses. Drive it with an
 * IPCS_command (from <sys/ipcs.h>, not in the userspace SDK, so declared here -
 * ABI verified against ipcs on macOS 26): set the ITER op and a zeroed cursor,
 * pass the command as both old and new value, and the kernel fills one *id_ds
 * per call and advances the cursor, returning ENOENT at the end. The object id
 * is IXSEQ_TO_IPCID = (_seq << 16) | index, index = cursor-1. Emits the Linux
 * header plus one line per object (the header always, so an empty subsystem
 * matches a Linux host with no such objects). rss/swap have no macOS source (0).
 */
#define PROCFS_IPCS_MAGIC     1
#define PROCFS_IPCS_SHM_ITER  0x00000002
#define PROCFS_IPCS_SEM_ITER  0x00000020
#define PROCFS_IPCS_MSG_ITER  0x00000200

typedef struct {
    int   ipcs_magic;
    int   ipcs_op;
    int   ipcs_cursor;
    int   ipcs_datalen;
    void *ipcs_data;
} procfs_ipcs_command;

#define PROCFS_IPCS_GUARD 200000   /* bound the iteration defensively */

static void
build_sysvipc_shm_blob(char **blobp, size_t *lenp)
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
    fprintf(f, "       key      shmid perms       size  cpid  lpid nattch"
               "   uid   gid  cuid  cgid      atime      dtime      ctime"
               "        rss       swap\n");

    procfs_ipcs_command ic;
    struct shmid_ds     ds;
    memset(&ic, 0, sizeof(ic));
    ic.ipcs_magic = PROCFS_IPCS_MAGIC;
    ic.ipcs_op = PROCFS_IPCS_SHM_ITER;
    ic.ipcs_datalen = sizeof(ds);
    ic.ipcs_data = &ds;
    size_t len = sizeof(ic);
    int guard = 0;
    while (sysctlbyname("kern.sysv.ipcs.shm", &ic, &len, &ic, sizeof(ic)) == 0 &&
           guard++ < PROCFS_IPCS_GUARD) {
        int id = (ds.shm_perm._seq << 16) | ((ic.ipcs_cursor - 1) & 0xffff);
        fprintf(f, "%10d %10d %5o %10lu %5d %5d %6d %5u %5u %5u %5u %10ld %10ld %10ld %10lu %10lu\n",
            ds.shm_perm._key, id, ds.shm_perm.mode & 0777,
            (unsigned long)ds.shm_segsz, ds.shm_cpid, ds.shm_lpid,
            (int)ds.shm_nattch, ds.shm_perm.uid, ds.shm_perm.gid,
            ds.shm_perm.cuid, ds.shm_perm.cgid, (long)ds.shm_atime,
            (long)ds.shm_dtime, (long)ds.shm_ctime, 0UL, 0UL);
        ic.ipcs_op = PROCFS_IPCS_SHM_ITER;
        ic.ipcs_datalen = sizeof(ds);
        ic.ipcs_data = &ds;
        len = sizeof(ic);
    }

    fclose(f);
    *blobp = buf;
    *lenp = sz;
}

static void
build_sysvipc_sem_blob(char **blobp, size_t *lenp)
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
    fprintf(f, "       key      semid perms      nsems   uid   gid  cuid  cgid"
               "      otime      ctime\n");

    procfs_ipcs_command ic;
    struct semid_ds     ds;
    memset(&ic, 0, sizeof(ic));
    ic.ipcs_magic = PROCFS_IPCS_MAGIC;
    ic.ipcs_op = PROCFS_IPCS_SEM_ITER;
    ic.ipcs_datalen = sizeof(ds);
    ic.ipcs_data = &ds;
    size_t len = sizeof(ic);
    int guard = 0;
    while (sysctlbyname("kern.sysv.ipcs.sem", &ic, &len, &ic, sizeof(ic)) == 0 &&
           guard++ < PROCFS_IPCS_GUARD) {
        int id = (ds.sem_perm._seq << 16) | ((ic.ipcs_cursor - 1) & 0xffff);
        fprintf(f, "%10d %10d %5o %10u %5u %5u %5u %5u %10ld %10ld\n",
            ds.sem_perm._key, id, ds.sem_perm.mode & 0777,
            (unsigned)ds.sem_nsems, ds.sem_perm.uid, ds.sem_perm.gid,
            ds.sem_perm.cuid, ds.sem_perm.cgid, (long)ds.sem_otime,
            (long)ds.sem_ctime);
        ic.ipcs_op = PROCFS_IPCS_SEM_ITER;
        ic.ipcs_datalen = sizeof(ds);
        ic.ipcs_data = &ds;
        len = sizeof(ic);
    }

    fclose(f);
    *blobp = buf;
    *lenp = sz;
}

static void
build_sysvipc_msg_blob(char **blobp, size_t *lenp)
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
    fprintf(f, "       key      msqid perms      cbytes       qnum lspid lrpid"
               "   uid   gid  cuid  cgid      stime      rtime      ctime\n");

    procfs_ipcs_command ic;
    struct msqid_ds     ds;
    memset(&ic, 0, sizeof(ic));
    ic.ipcs_magic = PROCFS_IPCS_MAGIC;
    ic.ipcs_op = PROCFS_IPCS_MSG_ITER;
    ic.ipcs_datalen = sizeof(ds);
    ic.ipcs_data = &ds;
    size_t len = sizeof(ic);
    int guard = 0;
    while (sysctlbyname("kern.sysv.ipcs.msg", &ic, &len, &ic, sizeof(ic)) == 0 &&
           guard++ < PROCFS_IPCS_GUARD) {
        int id = (ds.msg_perm._seq << 16) | ((ic.ipcs_cursor - 1) & 0xffff);
        fprintf(f, "%10d %10d %5o %10lu %10lu %5d %5d %5u %5u %5u %5u %10ld %10ld %10ld\n",
            ds.msg_perm._key, id, ds.msg_perm.mode & 0777,
            (unsigned long)ds.msg_cbytes, (unsigned long)ds.msg_qnum,
            ds.msg_lspid, ds.msg_lrpid, ds.msg_perm.uid, ds.msg_perm.gid,
            ds.msg_perm.cuid, ds.msg_perm.cgid, (long)ds.msg_stime,
            (long)ds.msg_rtime, (long)ds.msg_ctime);
        ic.ipcs_op = PROCFS_IPCS_MSG_ITER;
        ic.ipcs_datalen = sizeof(ds);
        ic.ipcs_data = &ds;
        len = sizeof(ic);
    }

    fclose(f);
    *blobp = buf;
    *lenp = sz;
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
 * /proc/interrupts support. macOS exposes no per-CPU interrupt counts (only
 * private IOReporting), so the count columns are 0; the IRQ topology is real,
 * though - each device in the IORegistry carries IOInterruptSpecifiers (the IRQ
 * numbers) and IOInterruptControllers. Walk the registry, collect one row per
 * (irq, controller, device), sort by IRQ, and format the Linux table.
 */
struct irqrow {
    long irq;
    char ctrl[80];
    char dev[80];
};

static int
irqrow_cmp(const void *a, const void *b)
{
    long ia = ((const struct irqrow *)a)->irq;
    long ib = ((const struct irqrow *)b)->irq;
    return (ia > ib) - (ia < ib);
}

static void
build_interrupts_blob(char **blobp, size_t *lenp)
{
    free(*blobp);
    *blobp = NULL;
    *lenp = 0;

    int    ncpu = 1;
    size_t nlen = sizeof(ncpu);
    if (sysctlbyname("hw.logicalcpu", &ncpu, &nlen, NULL, 0) != 0 || ncpu < 1) {
        ncpu = 1;
    }
    if (ncpu > 64) {
        ncpu = 64;
    }

    struct irqrow *rows = NULL;
    size_t nrows = 0, cap = 0;

    io_registry_entry_t root = IORegistryGetRootEntry(kIOMainPortDefault);
    io_iterator_t it = MACH_PORT_NULL;
    if (root != MACH_PORT_NULL &&
        IORegistryEntryCreateIterator(root, kIOServicePlane,
            kIORegistryIterateRecursively, &it) == KERN_SUCCESS) {
        io_registry_entry_t e;
        while ((e = IOIteratorNext(it)) != MACH_PORT_NULL) {
            CFArrayRef specs = IORegistryEntryCreateCFProperty(e,
                CFSTR("IOInterruptSpecifiers"), kCFAllocatorDefault, 0);
            if (specs != NULL && CFGetTypeID(specs) == CFArrayGetTypeID()) {
                CFArrayRef ctrls = IORegistryEntryCreateCFProperty(e,
                    CFSTR("IOInterruptControllers"), kCFAllocatorDefault, 0);
                io_name_t name = "";
                IORegistryEntryGetName(e, name);

                CFIndex n = CFArrayGetCount(specs);
                for (CFIndex i = 0; i < n; i++) {
                    CFTypeRef d = CFArrayGetValueAtIndex(specs, i);
                    long irq = -1;
                    if (d != NULL && CFGetTypeID(d) == CFDataGetTypeID() &&
                        CFDataGetLength((CFDataRef)d) >= 4) {
                        const UInt8 *b = CFDataGetBytePtr((CFDataRef)d);
                        irq = (long)(uint32_t)(b[0] | (b[1] << 8) |
                            (b[2] << 16) | ((uint32_t)b[3] << 24));
                    }
                    if (irq < 0) {
                        continue;
                    }
                    if (nrows == cap) {
                        size_t nc = (cap == 0) ? 128 : cap * 2;
                        struct irqrow *nr = realloc(rows, nc * sizeof(*nr));
                        if (nr == NULL) {
                            break;
                        }
                        rows = nr;
                        cap = nc;
                    }
                    struct irqrow *r = &rows[nrows++];
                    r->irq = irq;
                    r->ctrl[0] = '\0';
                    if (ctrls != NULL && CFGetTypeID(ctrls) == CFArrayGetTypeID() &&
                        i < CFArrayGetCount(ctrls)) {
                        CFTypeRef cs = CFArrayGetValueAtIndex(ctrls, i);
                        if (cs != NULL && CFGetTypeID(cs) == CFStringGetTypeID()) {
                            CFStringGetCString((CFStringRef)cs, r->ctrl,
                                sizeof(r->ctrl), kCFStringEncodingUTF8);
                        }
                    }
                    strlcpy(r->dev, name, sizeof(r->dev));
                }
                if (ctrls != NULL) {
                    CFRelease(ctrls);
                }
            }
            if (specs != NULL) {
                CFRelease(specs);
            }
            IOObjectRelease(e);
        }
        IOObjectRelease(it);
    }
    if (root != MACH_PORT_NULL) {
        IOObjectRelease(root);
    }

    qsort(rows, nrows, sizeof(*rows), irqrow_cmp);

    char  *buf = NULL;
    size_t sz  = 0;
    FILE  *f   = open_memstream(&buf, &sz);
    if (f != NULL) {
        fprintf(f, "     ");                        /* pad over the IRQ column */
        for (int c = 0; c < ncpu; c++) {
            fprintf(f, "CPU%-8d", c);
        }
        fprintf(f, "\n");
        for (size_t i = 0; i < nrows; i++) {
            fprintf(f, "%4ld:", rows[i].irq);
            for (int c = 0; c < ncpu; c++) {
                fprintf(f, " %10u", 0u);            /* per-CPU count unavailable */
            }
            fprintf(f, "  %s  %s\n", rows[i].ctrl, rows[i].dev);
        }
        fclose(f);
        *blobp = buf;
        *lenp = sz;
    }
    free(rows);
}

/*
 * /proc/tty/drivers support. Linux lists registered tty_driver structs; macOS
 * has no such registry, so derive the table from the tty devices in /dev,
 * grouped by major. Each major becomes one Linux-style line: driver name,
 * /dev prefix, major, minor range and a type.
 */
struct ttyrow {
    int      major;
    uint32_t min, max;      /* minor range seen for this major */
    char     name[64];      /* driver / dev family name */
    char     type[16];      /* system / console / serial / pty:master|slave */
};

static int
tty_is_name(const char *n)
{
    return strncmp(n, "tty", 3) == 0 || strncmp(n, "cu.", 3) == 0 ||
           strncmp(n, "pty", 3) == 0 || strncmp(n, "ptm", 3) == 0 ||
           strcmp(n, "console") == 0;
}

/* Family name: the device name up to its first '.' or digit (ptyp0 -> "ptyp",
 * cu.wlan-debug -> "cu", ttys003 -> "ttys", console -> "console"). */
static void
tty_family(const char *in, char *out, size_t cap)
{
    size_t i = 0;
    for (; in[i] != '\0' && i + 1 < cap; i++) {
        char c = in[i];
        if (c == '.' || (c >= '0' && c <= '9')) {
            break;
        }
        out[i] = c;
    }
    out[i] = '\0';
    if (i == 0) {
        strlcpy(out, in, cap);
    }
}

static const char *
tty_type(const char *n)
{
    if (strcmp(n, "console") == 0)                                return "console";
    if (strncmp(n, "cu.", 3) == 0 || strncmp(n, "tty.", 4) == 0)  return "serial";
    if (strncmp(n, "ptm", 3) == 0 || strncmp(n, "pty", 3) == 0)   return "pty:master";
    if (strcmp(n, "tty") == 0)                                    return "system";
    if (strncmp(n, "tty", 3) == 0)                                return "pty:slave";
    return "system";
}

static int
ttyrow_cmp(const void *a, const void *b)
{
    int ma = ((const struct ttyrow *)a)->major;
    int mb = ((const struct ttyrow *)b)->major;
    return (ma > mb) - (ma < mb);
}

static void
build_ttydrivers_blob(char **blobp, size_t *lenp)
{
    free(*blobp);
    *blobp = NULL;
    *lenp = 0;

    DIR *dir = opendir("/dev");
    if (dir == NULL) {
        return;
    }

    struct ttyrow *rows = NULL;
    size_t nrows = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.' || !tty_is_name(de->d_name)) {
            continue;
        }
        char path[1100];
        snprintf(path, sizeof(path), "/dev/%s", de->d_name);
        struct stat st;
        if (lstat(path, &st) != 0 || !S_ISCHR(st.st_mode)) {
            continue;
        }
        int      maj = major(st.st_rdev);
        uint32_t min = (uint32_t)minor(st.st_rdev);

        struct ttyrow *r = NULL;
        for (size_t i = 0; i < nrows; i++) {
            if (rows[i].major == maj) { r = &rows[i]; break; }
        }
        if (r != NULL) {
            if (min < r->min) { r->min = min; }
            if (min > r->max) { r->max = min; }
            continue;
        }
        if (nrows == cap) {
            size_t nc = (cap == 0) ? 32 : cap * 2;
            struct ttyrow *nr = realloc(rows, nc * sizeof(*nr));
            if (nr == NULL) { break; }
            rows = nr; cap = nc;
        }
        r = &rows[nrows++];
        r->major = maj;
        r->min = r->max = min;
        tty_family(de->d_name, r->name, sizeof(r->name));
        strlcpy(r->type, tty_type(de->d_name), sizeof(r->type));
    }
    closedir(dir);

    qsort(rows, nrows, sizeof(*rows), ttyrow_cmp);

    char  *buf = NULL;
    size_t sz  = 0;
    FILE  *f   = open_memstream(&buf, &sz);
    if (f != NULL) {
        for (size_t i = 0; i < nrows; i++) {
            char devp[80], range[32];
            snprintf(devp, sizeof(devp), "/dev/%s", rows[i].name);
            if (rows[i].min == rows[i].max) {
                snprintf(range, sizeof(range), "%u", rows[i].min);
            } else {
                snprintf(range, sizeof(range), "%u-%u", rows[i].min, rows[i].max);
            }
            fprintf(f, "%-20s %-11s %3d %s %s\n",
                rows[i].name, devp, rows[i].major, range, rows[i].type);
        }
        fclose(f);
        *blobp = buf;
        *lenp = sz;
    }
    free(rows);
}

/*
 * /proc/fs/nfs/exports support. macOS keeps the NFS export configuration in
 * /etc/exports, which nfsd registers with the kernel, so serve that file's
 * contents. A machine with no NFS server configured has no /etc/exports, which
 * leaves the node empty.
 */
static void
build_nfsexports_blob(char **blobp, size_t *lenp)
{
    free(*blobp);
    *blobp = NULL;
    *lenp = 0;

    FILE *in = fopen("/etc/exports", "r");
    if (in == NULL) {
        return;                     /* no NFS exports configured -> empty */
    }

    char  *buf = NULL;
    size_t sz  = 0;
    FILE  *f   = open_memstream(&buf, &sz);
    if (f != NULL) {
        char line[1024];
        while (fgets(line, sizeof(line), in) != NULL) {
            fputs(line, f);
        }
        fclose(f);
        *blobp = buf;
        *lenp = sz;
    }
    fclose(in);
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
 * Serve a PROCFS_REQ_MAPS request: enumerate the target task's VM regions at or
 * above req->arg (the resume address; 0 on the first request) and return as many
 * struct procfs_map_region records as fit in one payload. Each region is probed
 * twice - VM_REGION_BASIC_INFO_64 for protections/offset/wired, then
 * VM_REGION_EXTENDED_INFO at the same address for the page counts and share mode.
 * The kext re-requests with arg = the last region's end until an empty reply.
 * task_for_pid is denied for SIP/AMFI-protected & hardened targets (EPERM),
 * which is the coverage limit of moving this off the in-kernel VM walk.
 */
static void
procfsd_handle_maps(const struct procfs_ctl_req *req, struct procfs_ctl_resp *resp,
    void *payload)
{
    task_t task = TASK_NULL;
    if (task_for_pid(mach_task_self(), req->pid, &task) != KERN_SUCCESS) {
        resp->error = EPERM;
        return;
    }

    struct procfs_map_region *out = (struct procfs_map_region *)payload;
    uint32_t maxfit = PROCFS_CTL_MAXPAYLOAD / (uint32_t)sizeof(*out);
    uint32_t n = 0;
    mach_vm_address_t addr = (mach_vm_address_t)req->arg;

    while (n < maxfit) {
        mach_vm_size_t                 size = 0;
        vm_region_basic_info_data_64_t bi;
        mach_msg_type_number_t         bc = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t                    bobj = MACH_PORT_NULL;
        if (mach_vm_region(task, &addr, &size, VM_REGION_BASIC_INFO_64,
                (vm_region_info_t)&bi, &bc, &bobj) != KERN_SUCCESS) {
            break;      /* no more regions at or above addr */
        }

        /* Extended info for the same region (found at or above addr == its base). */
        mach_vm_address_t              eaddr = addr;
        mach_vm_size_t                 esize = 0;
        vm_region_extended_info_data_t ei;
        mach_msg_type_number_t         ec = VM_REGION_EXTENDED_INFO_COUNT;
        mach_port_t                    eobj = MACH_PORT_NULL;
        memset(&ei, 0, sizeof(ei));
        (void)mach_vm_region(task, &eaddr, &esize, VM_REGION_EXTENDED_INFO,
                (vm_region_info_t)&ei, &ec, &eobj);

        out[n].start          = addr;
        out[n].size           = size;
        out[n].offset         = bi.offset;
        out[n].prot           = (uint32_t)bi.protection;
        out[n].max_prot       = (uint32_t)bi.max_protection;
        out[n].user_wired     = bi.user_wired_count;
        out[n].share_mode     = ei.share_mode;
        out[n].resident_pages = ei.pages_resident;
        out[n].dirty_pages    = ei.pages_dirtied;
        out[n].swapped_pages  = ei.pages_swapped_out;
        out[n].external_pager = ei.external_pager;
        out[n].shared         = (uint32_t)bi.shared;
        n++;

        mach_vm_address_t next = addr + size;
        if (next <= addr) {
            break;      /* wrapped at the top of the address space */
        }
        addr = next;
    }

    resp->len = n * (uint32_t)sizeof(*out);
    mach_port_deallocate(mach_task_self(), task);
}

/*
 * Serve a PROCFS_REQ_MEMREAD request: read up to MAXPAYLOAD bytes from the target
 * task starting at the virtual address req->arg, into the response payload. Reads
 * proceed page by page and stop at the first unreadable (non-resident/unmapped)
 * page, so the reply is the resident prefix - a short or empty reply signals a
 * hole, which the kext turns into the Linux /proc/<pid>/mem "stop at the gap"
 * behaviour. task_for_pid is denied for SIP/AMFI-protected & hardened targets
 * (EPERM).
 */
static void
procfsd_handle_memread(const struct procfs_ctl_req *req, struct procfs_ctl_resp *resp,
    void *payload)
{
    task_t task = TASK_NULL;
    if (task_for_pid(mach_task_self(), req->pid, &task) != KERN_SUCCESS) {
        resp->error = EPERM;
        return;
    }

    uint8_t          *out  = (uint8_t *)payload;
    mach_vm_address_t base  = (mach_vm_address_t)req->arg;
    mach_vm_size_t    total = 0;

    while (total < PROCFS_CTL_MAXPAYLOAD) {
        mach_vm_address_t cur   = base + total;
        mach_vm_size_t    pgoff = cur & (mach_vm_size_t)(vm_page_size - 1);
        mach_vm_size_t    chunk = (mach_vm_size_t)vm_page_size - pgoff;
        if (chunk > PROCFS_CTL_MAXPAYLOAD - total) {
            chunk = PROCFS_CTL_MAXPAYLOAD - total;
        }
        mach_vm_size_t got = 0;
        if (mach_vm_read_overwrite(task, cur, chunk,
                (mach_vm_address_t)(uintptr_t)(out + total), &got) != KERN_SUCCESS ||
                got == 0) {
            break;      /* non-resident/unmapped page - stop at the hole */
        }
        total += got;
        if (got < chunk) {
            break;
        }
    }

    resp->len = (uint32_t)total;    /* 0 => hole at the start address */
    mach_port_deallocate(mach_task_self(), task);
}

/*
 * Serve a PROCFS_REQ_PROCARGS request: fetch the target's flattened argument
 * region with the KERN_PROCARGS2 sysctl (argc, exec path, argv, env, apple[])
 * and return the slice [arg, arg+MAXPAYLOAD). The kext re-requests at successive
 * offsets until a short reply, then splits the sections. This is a root sysctl,
 * not task_for_pid, so it works for protected/hardened targets too; it fails
 * (EINVAL) for zombies and some system processes, which the kext already handles
 * by falling back to the parenthesised command name.
 */
static void
procfsd_handle_procargs(const struct procfs_ctl_req *req, struct procfs_ctl_resp *resp,
    void *payload)
{
    static size_t argmax = 0;
    if (argmax == 0) {
        int    am = 0;
        size_t s  = sizeof(am);
        int    m[2] = { CTL_KERN, KERN_ARGMAX };
        argmax = (sysctl(m, 2, &am, &s, NULL, 0) == 0 && am > 0)
                 ? (size_t)am : (1u << 20);     /* 1 MB fallback */
    }

    char *buf = malloc(argmax);
    if (buf == NULL) {
        resp->error = ENOMEM;
        return;
    }

    int    mib[3] = { CTL_KERN, KERN_PROCARGS2, req->pid };
    size_t sz     = argmax;
    if (sysctl(mib, 3, buf, &sz, NULL, 0) != 0) {
        resp->error = (errno != 0) ? errno : EINVAL;
        free(buf);
        return;
    }

    size_t off = (size_t)req->arg;
    if (off < sz) {
        size_t avail = sz - off;
        size_t n = (avail < PROCFS_CTL_MAXPAYLOAD) ? avail : PROCFS_CTL_MAXPAYLOAD;
        memcpy(payload, buf + off, n);
        resp->len = (uint32_t)n;
    } else {
        resp->len = 0;      /* at/past the end of the region */
    }
    free(buf);
}

/*
 * Restore one persisted integer setting: read `path` and write the value to the
 * `oid` sysctl. wait_connect() returns as soon as the kext registers its kernel
 * control, which the start routine does just before registering these oids - so
 * an oid may not exist for a few microseconds yet; retry while it is still
 * absent (ENOENT), up to ~1s. When clamp01 is set the value is forced to 0/1.
 */
static void
apply_persisted_int(const char *path, const char *oid, int clamp01)
{
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        return;                 /* never set -> leave the kext default */
    }
    int val = 0;
    int got = fscanf(f, "%d", &val);
    fclose(f);
    if (got != 1) {
        return;
    }
    if (clamp01) {
        val = (val != 0) ? 1 : 0;
    }

    for (int tries = 0; tries < 50; tries++) {
        if (sysctlbyname(oid, NULL, NULL, &val, sizeof(val)) == 0) {
            fprintf(stderr, "procfsd: restored %s=%d\n", oid, val);
            return;
        }
        if (errno != ENOENT) {
            break;              /* a real error, not "oid not ready yet" */
        }
        usleep(20000);          /* 20 ms */
    }
    fprintf(stderr, "procfsd: could not restore %s=%d: %s\n",
        oid, val, strerror(errno));
}

/*
 * Restore the persisted procfs state - the native/Linux presentation mode and
 * the spoofed Linux kernel version. Both sysctls live in the kext and reset to
 * their defaults every time it loads, so the GUI's choices are saved to
 * /var/db/procfs.linux[_version] and re-applied whenever the kext (re)appears.
 */
static void
apply_persisted_linux_mode(void)
{
    apply_persisted_int(PROCFS_LINUX_CONF,     "procfs.linux",         1);
    apply_persisted_int(PROCFS_LINUX_VER_CONF, "procfs.linux_version", 0);
}

int
main(__unused int argc, __unused char **argv)
{
    procfsd_bootstrap();        /* gated kext auto-load */

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
        case PROCFS_REQ_TTY: {
            /* Controlling terminal for /proc/<pid>/tty. proc_pidinfo's BSD info
             * carries e_tdev (the controlling tty device, NODEV when none);
             * map it to its /dev path via devname_r. Empty reply = no tty. */
            struct proc_bsdinfo bi;
            int r = proc_pidinfo(req->pid, PROC_PIDTBSDINFO, 0, &bi, sizeof(bi));
            if (r == (int)sizeof(bi)) {
                if ((bi.pbi_flags & PROC_FLAG_CONTROLT) &&
                    bi.e_tdev != (uint32_t)-1) {
                    char dbuf[64];
                    char *nm = devname_r((dev_t)bi.e_tdev, S_IFCHR,
                                         dbuf, (int)sizeof(dbuf));
                    if (nm != NULL && nm[0] != '\0' && nm[0] != '?') {
                        int n = snprintf((char *)payload, PROCFS_CTL_MAXPAYLOAD,
                                         "/dev/%s", nm);
                        if (n > 0) {
                            resp->len = (n > (int)PROCFS_CTL_MAXPAYLOAD)
                                        ? PROCFS_CTL_MAXPAYLOAD : (uint32_t)n;
                        }
                    }
                }
                /* else: no controlling terminal -> resp->len stays 0 */
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
        case PROCFS_REQ_MISC: {
            size_t off = (size_t)req->arg;
            if (off == 0) {
                build_misc_blob(&g_misc_blob, &g_misc_blob_len);
            }
            blob_slice(g_misc_blob, g_misc_blob_len, off, payload, resp);
            break;
        }
        case PROCFS_REQ_KSYM_LOOKUP: {
            /* req->arg = a kernel text address in the wchan symbol source's
             * layout; reply = the symbol name (empty -> kext reports "0"). */
            char nm[128];
            ksym_lookup(req->arg, nm, sizeof(nm));
            size_t n = strlen(nm);
            if (n > 0) {
                if (n > PROCFS_CTL_MAXPAYLOAD) {
                    n = PROCFS_CTL_MAXPAYLOAD;
                }
                memcpy(payload, nm, n);
                resp->len = (uint32_t)n;
            }
            break;
        }
        case PROCFS_REQ_KSYM_REF: {
            /* Address of _proc_pid in the wchan symbol source, so the kext can
             * derive the running kernel's slide. */
            uint64_t a = ksym_addr_of("_proc_pid");
            memcpy(payload, &a, sizeof(a));
            resp->len = sizeof(a);
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
        case PROCFS_REQ_VMALLOCINFO: {
            size_t off = (size_t)req->arg;
            if (off == 0) {
                build_vmallocinfo_blob(&g_vmalloc_blob, &g_vmalloc_blob_len);
            }
            blob_slice(g_vmalloc_blob, g_vmalloc_blob_len, off, payload, resp);
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
        case PROCFS_REQ_SCSI: {
            size_t off = (size_t)req->arg;
            if (off == 0) {
                build_scsi_blob(&g_scsi_blob, &g_scsi_blob_len);
            }
            blob_slice(g_scsi_blob, g_scsi_blob_len, off, payload, resp);
            break;
        }
        case PROCFS_REQ_SYSVIPC_SHM: {
            size_t off = (size_t)req->arg;
            if (off == 0) {
                build_sysvipc_shm_blob(&g_shm_blob, &g_shm_blob_len);
            }
            blob_slice(g_shm_blob, g_shm_blob_len, off, payload, resp);
            break;
        }
        case PROCFS_REQ_SYSVIPC_SEM: {
            size_t off = (size_t)req->arg;
            if (off == 0) {
                build_sysvipc_sem_blob(&g_sem_blob, &g_sem_blob_len);
            }
            blob_slice(g_sem_blob, g_sem_blob_len, off, payload, resp);
            break;
        }
        case PROCFS_REQ_SYSVIPC_MSG: {
            size_t off = (size_t)req->arg;
            if (off == 0) {
                build_sysvipc_msg_blob(&g_msg_blob, &g_msg_blob_len);
            }
            blob_slice(g_msg_blob, g_msg_blob_len, off, payload, resp);
            break;
        }
        case PROCFS_REQ_SLABINFO: {
            size_t off = (size_t)req->arg;
            if (off == 0) {
                build_slabinfo_blob(&g_slab_blob, &g_slab_blob_len);
            }
            blob_slice(g_slab_blob, g_slab_blob_len, off, payload, resp);
            break;
        }
        case PROCFS_REQ_KMSG: {
            size_t off = (size_t)req->arg;
            if (off == 0) {
                build_kmsg_blob(&g_kmsg_blob, &g_kmsg_blob_len);
            }
            blob_slice(g_kmsg_blob, g_kmsg_blob_len, off, payload, resp);
            break;
        }
        case PROCFS_REQ_LAST_KMSG: {
            size_t off = (size_t)req->arg;
            if (off == 0) {
                build_last_kmsg_blob(&g_lastkmsg_blob, &g_lastkmsg_blob_len);
            }
            blob_slice(g_lastkmsg_blob, g_lastkmsg_blob_len, off, payload, resp);
            break;
        }
        case PROCFS_REQ_KSYMS: {
            size_t off = (size_t)req->arg;
            if (off == 0) {
                build_ksyms_blob(&g_ksyms_blob, &g_ksyms_blob_len);
            }
            blob_slice(g_ksyms_blob, g_ksyms_blob_len, off, payload, resp);
            break;
        }
        case PROCFS_REQ_KALLSYMS: {
            size_t off = (size_t)req->arg;
            if (off == 0) {
                build_kallsyms_blob(&g_kallsyms_blob, &g_kallsyms_blob_len);
            }
            blob_slice(g_kallsyms_blob, g_kallsyms_blob_len, off, payload, resp);
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
        case PROCFS_REQ_NFSEXPORTS: {
            size_t off = (size_t)req->arg;
            if (off == 0) {
                build_nfsexports_blob(&g_nfs_blob, &g_nfs_blob_len);
            }
            blob_slice(g_nfs_blob, g_nfs_blob_len, off, payload, resp);
            break;
        }
        case PROCFS_REQ_INTERRUPTS: {
            size_t off = (size_t)req->arg;
            if (off == 0) {
                build_interrupts_blob(&g_intr_blob, &g_intr_blob_len);
            }
            blob_slice(g_intr_blob, g_intr_blob_len, off, payload, resp);
            break;
        }
        case PROCFS_REQ_TTYDRIVERS: {
            size_t off = (size_t)req->arg;
            if (off == 0) {
                build_ttydrivers_blob(&g_tty_blob, &g_tty_blob_len);
            }
            blob_slice(g_tty_blob, g_tty_blob_len, off, payload, resp);
            break;
        }
        case PROCFS_REQ_CPUCLUSTERS: {
            /* Per-logical-CPU cluster type ('E'/'P') from the device tree, for
             * the /proc/cpuinfo part number (Icestorm E vs Firestorm P, etc.). */
            char *out = (char *)payload;
            uint32_t maxn = PROCFS_CTL_MAXPAYLOAD;
            memset(out, '?', maxn < 256 ? maxn : 256);
            uint32_t maxid = 0;
            io_registry_entry_t cpus =
                IORegistryEntryFromPath(kIOMainPortDefault, "IODeviceTree:/cpus");
            if (cpus != MACH_PORT_NULL) {
                io_iterator_t it = MACH_PORT_NULL;
                if (IORegistryEntryGetChildIterator(cpus, kIODeviceTreePlane, &it)
                        == KERN_SUCCESS) {
                    io_registry_entry_t e;
                    while ((e = IOIteratorNext(it)) != MACH_PORT_NULL) {
                        int id = -1;
                        CFTypeRef p = IORegistryEntryCreateCFProperty(e,
                            CFSTR("logical-cpu-id"), kCFAllocatorDefault, 0);
                        if (p != NULL) {
                            if (CFGetTypeID(p) == CFNumberGetTypeID()) {
                                CFNumberGetValue(p, kCFNumberIntType, &id);
                            } else if (CFGetTypeID(p) == CFDataGetTypeID() &&
                                       CFDataGetLength(p) >= 1) {
                                id = *(const uint8_t *)CFDataGetBytePtr(p);
                            }
                            CFRelease(p);
                        }
                        char ct = '?';
                        p = IORegistryEntryCreateCFProperty(e, CFSTR("cluster-type"),
                            kCFAllocatorDefault, 0);
                        if (p != NULL) {
                            if (CFGetTypeID(p) == CFDataGetTypeID() &&
                                CFDataGetLength(p) >= 1) {
                                ct = *(const char *)CFDataGetBytePtr(p);
                            } else if (CFGetTypeID(p) == CFStringGetTypeID()) {
                                char b[8] = "";
                                CFStringGetCString(p, b, sizeof(b), kCFStringEncodingUTF8);
                                ct = b[0];
                            }
                            CFRelease(p);
                        }
                        if (id >= 0 && (uint32_t)id < maxn) {
                            out[id] = ct;
                            if ((uint32_t)id + 1 > maxid) { maxid = id + 1; }
                        }
                        IOObjectRelease(e);
                    }
                    IOObjectRelease(it);
                }
                IOObjectRelease(cpus);
            }
            resp->len = maxid;
            break;
        }
        case PROCFS_REQ_CPULOAD: {
            /* Per-CPU user/nice/system/idle ticks for /proc/stat's cpu/cpuN
             * lines. PROCESSOR_CPU_LOAD_INFO returns processor_cpu_load_info
             * records (cpu_ticks[] indexed by CPU_STATE_*), already in USER_HZ. */
            natural_t              nc = 0;
            processor_info_array_t pinfo = NULL;
            mach_msg_type_number_t pcnt = 0;
            if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO,
                    &nc, &pinfo, &pcnt) == KERN_SUCCESS && nc > 0) {
                struct procfs_cpu_load *out = (struct procfs_cpu_load *)payload;
                natural_t maxfit = PROCFS_CTL_MAXPAYLOAD / sizeof(*out);
                natural_t n = (nc < maxfit) ? nc : maxfit;
                processor_cpu_load_info_t li = (processor_cpu_load_info_t)pinfo;
                for (natural_t i = 0; i < n; i++) {
                    out[i].user = li[i].cpu_ticks[CPU_STATE_USER];
                    out[i].nice = li[i].cpu_ticks[CPU_STATE_NICE];
                    out[i].sys  = li[i].cpu_ticks[CPU_STATE_SYSTEM];
                    out[i].idle = li[i].cpu_ticks[CPU_STATE_IDLE];
                }
                resp->len = (uint32_t)(n * sizeof(*out));
                vm_deallocate(mach_task_self(), (vm_address_t)pinfo,
                    pcnt * sizeof(natural_t));
            } else {
                resp->error = EIO;
            }
            break;
        }
        case PROCFS_REQ_CPUSTAT: {
            /* Per-CPU interrupt-event counters, the XNU-side softirq data. The
             * PROCESSOR_CPU_STAT flavor (32-bit) is the one populated on Apple
             * Silicon; each CPU's slice is irq_ex_cnt, ipi_cnt, timer_cnt, ... */
#ifndef PROCESSOR_CPU_STAT
#define PROCESSOR_CPU_STAT 4
#endif
            natural_t              nc = 0;
            processor_info_array_t pinfo = NULL;
            mach_msg_type_number_t pcnt = 0;
            if (host_processor_info(mach_host_self(), PROCESSOR_CPU_STAT,
                    &nc, &pinfo, &pcnt) == KERN_SUCCESS && nc > 0) {
                struct procfs_cpu_stat *out = (struct procfs_cpu_stat *)payload;
                natural_t maxfit = PROCFS_CTL_MAXPAYLOAD / sizeof(*out);
                natural_t n = (nc < maxfit) ? nc : maxfit;
                mach_msg_type_number_t stride = pcnt / nc;   /* natural_t per CPU */
                for (natural_t i = 0; i < n; i++) {
                    const uint32_t *s = (const uint32_t *)pinfo + (size_t)i * stride;
                    out[i].hwirq = s[0];        /* irq_ex_cnt */
                    out[i].ipi   = s[1];        /* ipi_cnt    */
                    out[i].timer = s[2];        /* timer_cnt  */
                }
                resp->len = (uint32_t)(n * sizeof(*out));
                vm_deallocate(mach_task_self(), (vm_address_t)pinfo,
                    pcnt * sizeof(natural_t));
            } else {
                resp->error = EIO;
            }
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
        case PROCFS_REQ_MAPS:
            procfsd_handle_maps(req, resp, payload);
            break;
        case PROCFS_REQ_MEMREAD:
            procfsd_handle_memread(req, resp, payload);
            break;
        case PROCFS_REQ_PROCARGS:
            procfsd_handle_procargs(req, resp, payload);
            break;
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
