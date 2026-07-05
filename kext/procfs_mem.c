/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_mem.c
 *
 * Process virtual-memory node (/proc/<pid>/mem). The entry point is named after
 * NetBSD's procfs_mem.c (procfs_domem); FreeBSD's equivalent is procfs_doprocmem
 * in the same file. References:
 *   https://github.com/NetBSD/src/blob/trunk/sys/miscfs/procfs/procfs_mem.c
 *   https://github.com/freebsd/freebsd-src/blob/main/sys/fs/procfs/procfs_mem.c
 *
 * As with the Linux /proc/<pid>/mem file, the read offset is interpreted as a
 * virtual address in the target process: seeking to an address and reading
 * returns the bytes at that address. The overall shape matches both BSDs - a
 * credential/visibility check (NetBSD process_checkioperm(), FreeBSD
 * p_candebug()) followed by a memory-read loop driven by the uio.
 *
 * The read mechanism itself diverges. Both BSDs fault the target's pages in
 * through the VM system (NetBSD uvm_io(), FreeBSD proc_rwmem()); macOS exposes
 * no equivalent KPI a third-party kext may link. Rather than translate the
 * target's pages through its pmap in-kernel (which needs proc_task and the
 * unsupported pmap primitives), the read is delegated to the procfsd daemon: it
 * opens the target with task_for_pid() and reads with mach_vm_read_overwrite()
 * (PROCFS_REQ_MEMREAD), returning the resident prefix at the requested address.
 * A short or empty reply marks a hole, so - as with the BSD originals - only
 * resident memory is returned and a gap ends the read rather than faulting a
 * page back in. The cost is coverage: task_for_pid is denied for SIP/AMFI-
 * protected and hardened-runtime targets, which then report EPERM.
 */
#include <stdint.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <bsdcompat/sys/malloc.h>

#include <fs/procfs/procfs.h>
#include <fs/procfs/procfs_ctl.h>

/*
 * Reads the data for the "mem" node: bytes from the target process's address
 * space, with the uio offset interpreted as the virtual address (the Linux
 * /proc/<pid>/mem semantics). Named after NetBSD's procfs_domem().
 *
 * Each iteration asks the daemon for the resident bytes at the current address
 * (up to one payload). Returns EIO if the very first requested address is not
 * resident (an unmapped region or a paged-out address), and stops cleanly at the
 * first hole once some bytes have been returned - matching how reads of
 * /proc/<pid>/mem behave across unmapped gaps. ENOTSUP without a connected
 * daemon; EPERM for a SIP/AMFI-protected or hardened target.
 */
int
procfs_domem(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx)
{
    int error = 0;

    /* The node is read-only; writing another process's memory is not supported. */
    if (uio_rw(uio) != UIO_READ) {
        return EOPNOTSUPP;
    }

    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p == PROC_NULL) {
        return ESRCH;
    }

    /* Reading another process's memory is sensitive; gate on the same
     * credential check the rest of the filesystem uses. */
    error = procfs_check_can_access_process(vfs_context_ucred(ctx), p);
    if (error != 0) {
        proc_rele(p);
        return error;
    }

    /*
     * Hold the proc_find() reference across the whole read: that pins the pid so
     * it cannot be reused mid-read. The daemon opens the target by pid on each
     * chunk; if the process exits its memory simply stops being readable and the
     * read ends at that point.
     */
    pid_t pid = pnp->node_id.nodeid_pid;

    uint8_t *buf = malloc(PROCFS_CTL_MAXPAYLOAD, M_TEMP, M_WAITOK);
    if (buf == NULL) {
        proc_rele(p);
        return ENOMEM;
    }

    boolean_t any = FALSE;
    while (uio_resid(uio) > 0) {
        uint64_t va  = (uint64_t)uio_offset(uio);
        uint32_t got = 0;
        int rc = procfs_ctl_request(PROCFS_REQ_MEMREAD, pid, va,
                                    buf, PROCFS_CTL_MAXPAYLOAD, &got);
        if (rc != 0) {
            if (!any) {
                error = (rc == ENOTCONN || rc == ETIMEDOUT) ? ENOTSUP : rc;
            }
            break;                  /* no daemon / protected / gone */
        }
        if (got == 0) {
            if (!any) {
                error = EIO;        /* nothing resident at the start address */
            }
            break;                  /* otherwise return what we have */
        }

        user_ssize_t resid = uio_resid(uio);
        size_t move = ((user_ssize_t)got < resid) ? (size_t)got : (size_t)resid;
        error = uiomove((const char *)buf, (int)move, uio);
        if (error != 0) {
            break;
        }
        any = TRUE;

        if (got < PROCFS_CTL_MAXPAYLOAD) {
            break;                  /* resident prefix ended - stop at the hole */
        }
    }

    free(buf, M_TEMP);
    proc_rele(p);
    return error;
}
