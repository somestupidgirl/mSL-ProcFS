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
#include <sys/mount.h>
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/task.h>
#include <mach/thread_act.h>
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
