/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_regs.c
 *
 * Process general-register node (/proc/<pid>/regs).
 *
 * Named after NetBSD's procfs_doregs() (sys/miscfs/procfs/procfs_machdep.c) for
 * cross-reference. NetBSD dumps a machine-dependent `struct reg`; macOS has no
 * such struct, so this node emits the platform's native Mach thread state - the
 * direct analog:
 *
 *   arm64:  arm_thread_state64_t   (flavor ARM_THREAD_STATE64)
 *   x86_64: x86_thread_state64_t   (flavor x86_THREAD_STATE64)
 *
 * thread_get_state() is stripped from the arm64 kernelcache (it is neither a
 * bindable KPI symbol nor present in the symtab for libklookup), so the kext
 * cannot read register state itself. Instead the procfsd daemon reads it from
 * userspace - via task_for_pid() + task_threads() + thread_get_state() on the
 * representative thread - and returns it over the kernel-control bridge (see
 * procfs_ctl.c, tools/procfsd.c). The state is for the process's representative
 * thread, matching NetBSD's per-process regs node. On arm64e the saved pc/lr are
 * PAC-signed and emitted raw, leaving stripping to the consumer.
 *
 * task_for_pid is denied to root for Apple platform/hardened binaries (SIP/AMFI)
 * - reads of those return EPERM, analogous to ptrace permissions on Linux.
 *
 * A human-readable Linux-style register dump is provided separately, guarded,
 * in procfs_linux.c for the planned native-vs-linux compatibility mode.
 */
#include <stdint.h>

#include <mach/thread_status.h>
#if defined(__arm64__) || defined(__aarch64__)
#include <mach/arm/thread_status.h>
#elif defined(__x86_64__)
#include <mach/i386/thread_status.h>
#endif

#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <sys/uio.h>

#include <fs/procfs/procfs.h>
#include <fs/procfs/procfs_ctl.h>

int
procfs_doregs(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    if (uio_rw(uio) != UIO_READ) {
        /*
         * The node is read-only.
         */
        return EOPNOTSUPP;
    }

    /*
     * Linux presentation mode renders the registers as text (procfs_linux.c).
     */
    if (procfs_linux_mode) {
        return procfs_doregs_linux(pnp, uio, ctx);
    }

    /*
     * Validate the pid and reject processes with no user thread state.
     */
    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p == PROC_NULL) {
        return ESRCH;
    }
    int sysproc = (p->p_stat == SZOMB) || (p->p_flag & P_SYSTEM) != 0;
    proc_rele(p);
    if (sysproc) {
        return ESRCH;
    }

#if defined(__arm64__) || defined(__aarch64__)
    arm_thread_state64_t state;
#elif defined(__x86_64__)
    x86_thread_state64_t state;
#else
    return ENOTSUP;
#endif

#if defined(__arm64__) || defined(__aarch64__) || defined(__x86_64__)
    uint32_t got = 0;
    int rc = procfs_ctl_request(PROCFS_REQ_REGS, pnp->node_id.nodeid_pid, 0,
                                &state, sizeof(state), &got);
    if (rc != 0) {
        /*
         * ENOTCONN: the daemon is not running. Otherwise the daemon's errno
         * (EPERM for a SIP/AMFI-protected target, ESRCH/EIO otherwise).
         */
        return (rc == ENOTCONN) ? ENOTSUP : rc;
    }
    return procfs_copy_data((const char *)&state, (int)got, uio);
#endif
}
