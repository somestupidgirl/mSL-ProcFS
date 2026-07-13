/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_fpregs.c
 *
 * Process floating-point register node (/proc/<pid>/fpregs).
 *
 * Named after NetBSD's procfs_dofpregs() (sys/miscfs/procfs/procfs_machdep.c)
 * for cross-reference. NetBSD dumps a machine-dependent `struct fpreg`; macOS
 * has no such struct, so this node emits the platform's native Mach floating-
 * point / SIMD state - the direct analog:
 *
 *   arm64:  arm_neon_state64_t    (flavor ARM_NEON_STATE64)
 *   x86_64: x86_float_state64_t    (flavor x86_FLOAT_STATE64)
 *
 * As with regs, thread_get_state() is stripped from the arm64 kernelcache, so
 * the state is fetched from userspace by the procfsd daemon over the kernel-
 * control bridge (task_for_pid + task_threads + thread_get_state on the
 * representative thread; see procfs_regs.c and tools/procfsd.c). task_for_pid is
 * denied for Apple platform/hardened binaries (SIP/AMFI) - those return EPERM.
 *
 * A human-readable Linux-style dump is provided separately, guarded, in
 * procfs_linux.c for the planned native-vs-linux compatibility mode.
 */
#include <stdint.h>

#if defined(__arm64__) || defined(__aarch64__)
#include <mach/arm/thread_status.h>
#elif defined(__x86_64__)
#include <mach/i386/thread_status.h>
#endif
#include <mach/thread_status.h>

#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <sys/uio.h>

#include <fs/procfs/procfs.h>
#include <fs/procfs/procfs_ctl.h>

int
procfs_dofpregs(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    if (uio_rw(uio) != UIO_READ) {
        return EOPNOTSUPP;       /* the node is read-only */
    }

    /* Linux presentation mode renders the FP/SIMD registers as text. */
    if (procfs_linux_mode) {
        return procfs_dofpregs_linux(pnp, uio, ctx);
    }

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
    arm_neon_state64_t  state;
#elif defined(__x86_64__)
    x86_float_state64_t state;
#else
    return ENOTSUP;
#endif

#if defined(__arm64__) || defined(__aarch64__) || defined(__x86_64__)
    uint32_t got = 0;
    int rc = procfs_ctl_request(PROCFS_REQ_FPREGS, pnp->node_id.nodeid_pid, 0,
                                &state, sizeof(state), &got);
    if (rc != 0) {
        return (rc == ENOTCONN) ? ENOTSUP : rc;
    }
    return procfs_copy_data((const char *)&state, (int)got, uio);
#endif
}
