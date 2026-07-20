/*
 * Copyright (c) 2015 Kim Topley
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_status.c
 *
 * Data and size functions for the per-process and per-thread status nodes (pid, ppid, pgid, sid, tty, info, taskinfo).
 */
#include <libkern/libkern.h>
#include <sys/bsdtask_info.h>
#include <sys/file.h>
#include <sys/param.h>
#include <sys/filedesc.h>
#include <sys/kauth.h>
#include <sys/kpi_socket.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#include <sys/proc_internal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/vnode.h>
#include <netinet/in.h>

#include <fs/procfs/procfs.h>
#include <fs/procfs/procfs_ctl.h>


/*
 * Reads the data for the "pid" node. The data is the
 * owning process's pid.
 */
int
procfs_dopid(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int pid = pnp->node_id.nodeid_pid;
    int error = procfs_copy_data((const char *)&pid, sizeof(pid), uio);
    return error;
}

/*
 * Reads the data for the "ppid" node. The data is the
 * owning process's parent's pid.
 */
int
procfs_doppid(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p != PROC_NULL) {
        int ppid = proc_ppid(p);
        error = procfs_copy_data((const char *)&ppid, sizeof(ppid), uio);
        proc_rele(p);
    } else {
        error = ESRCH;
    }

    return error;
}

/*
 * Reads the data for the "pgid" node. The data is the
 * owning process's process group id.
 */
int
procfs_dopgid(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p != PROC_NULL) {
        pid_t pgrpid = proc_pgrpid(p);
        error = procfs_copy_data((const char *)&pgrpid, sizeof(pgrpid), uio);
        proc_rele(p);
    } else {
        error = ESRCH;
    }

    return error;
}

/*
 * Reads the data for the "sid" node. The data is the
 * owning process's session id.
 */
int
procfs_dosid(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx) {
    int error = 0;

    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p != PROC_NULL) {
        pid_t sid = proc_sessionid(p);
        error = procfs_copy_data((const char *)&sid, sizeof(sid), uio);
        proc_rele(p);
    } else {
        error = ESRCH;
    }

    return error;
}

/*
 * Reads the data for the "tty" node. The data is the
 * name of the owning process's controlling terminal.
 */
int
procfs_dotty(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    /*
     * The controlling terminal lives at p->p_pgrp->pg_session->s_ttyvp, reached
     * through the SMR-protected p_pgrp; the safe accessor, proc_gettty(), is
     * com.apple.kpi.private and unlinkable in-kernel (its SMR read section is too).
     * Rather than resolve it via libklookup, ask the procfsd daemon: it reads the
     * controlling-terminal device from proc_pidinfo(PROC_PIDTBSDINFO)'s e_tdev and
     * maps it to its /dev path (devname), returning the path string (empty when
     * the process has no controlling terminal). Without a connected daemon the
     * node reports ENOTSUP, as it did when libklookup was unavailable.
     */
    char path[MAXPATHLEN];
    uint32_t got = 0;

    int rc = procfs_ctl_request(PROCFS_REQ_TTY, pnp->node_id.nodeid_pid, 0,
        path, sizeof(path), &got);

    if (rc == ESRCH) {
        return ESRCH;
    }

    if (rc != 0) {
        /*
         * No daemon connected (ENOTCONN), timed out, or other failure.
         */
        return ENOTSUP;
    }

    /*
     * got == 0 means no controlling terminal: emit an empty node.
     */
    return procfs_copy_data(path, (int)got, uio);
}

/*
 * Reads basic info for a process. Populates an instance of the proc_bsdinfo
 * structure and copies it to the area described by a uio structure.
 */
int
procfs_dostatus(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    /*
     * Linux presentation mode renders status as text (Name:/State:/Pid:/...);
     * native mode emits the binary proc_bsdshortinfo below.
     */
    if (procfs_linux_mode) {
        return procfs_doprocstatus_linux(pnp, uio, ctx);
    }

    /*
     * Get the process id from the node id in the pfsnode and locate
     * the process.
     */
    int error = 0;

    /*
     * The BSD-centric process info comes from the procfsd daemon
     * (proc_pidinfo PROC_PIDT_SHORTBSDINFO). Assembling it in-kernel meant
     * reading struct proc's fields directly, whose offsets drift across kernel
     * point-releases; the daemon reaches the same data through a public call
     * that needs no task port, so it answers for every process.
     */
    struct proc_bsdshortinfo info;
    uint32_t got = 0;

    error = procfs_ctl_request(PROCFS_REQ_SHORTBSDINFO, pnp->node_id.nodeid_pid, 0,
                               &info, sizeof(info), &got);
    if (error != 0) {
        return error;
    }
    if (got != sizeof(info)) {
        return ESRCH;
    }

    return procfs_copy_data((const char *)&info, sizeof(info), uio);
}

/*
 * Reads basic info for the Mach task associated with a process. Populates 
 * an instance of a proc_taskinfo structure and copies it to tthe area described
 * by a uio structure.
 */
int
procfs_dotaskinfo(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    /*
     * Get the process id from the node id in the pfsnode and locate
     * the process.
     */
    int error = 0;

    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p != PROC_NULL) {
        struct proc_taskinfo info;

        /*
         * Preferred path: the procfsd daemon returns the exact proc_taskinfo
         * (all 18 fields) via libproc's proc_pidinfo(). Only when no daemon is
         * connected (or it doesn't answer in time) do we fall back to what the
         * kext can compute itself.
         */
        uint32_t got = 0;
        if (procfs_ctl_request(PROCFS_REQ_TASKINFO, pnp->node_id.nodeid_pid, 0,
                &info, sizeof(info), &got) == 0 && got == sizeof(info)) {
            error = procfs_copy_data((const char *)&info, sizeof(info), uio);
            proc_rele(p);
            return error;
        }

        /*
         * Fallback. On arm64 proc_pidtaskinfo() leaves everything zero
         * (fill_taskprocinfo is stripped), so fill the fields reachable without
         * it: the memory sizes from the VM-region walk, the thread count from
         * the uthread list, the default thread policy, and priority. The
         * remaining counters (CPU time, faults, syscalls, ...) live only in the
         * opaque task struct and stay zero. On x86_64, where proc_pidtaskinfo
         * already populated everything, these recompute the same values.
         */
        error = proc_pidtaskinfo(p, &info);
        if (error == 0) {
            uint64_t vsize = 0, rsize = 0;
            if (procfs_task_vm_sizes(p, &vsize, &rsize) == 0) {
                info.pti_virtual_size  = vsize;
                info.pti_resident_size = rsize;
            }

            int tcount = procfs_get_task_thread_count(p);
            if (tcount > 0) {
                info.pti_threadnum = tcount;
            }

            if (info.pti_policy == 0) {
                info.pti_policy = POLICY_TIMESHARE;
            }
            /*
             * The remaining proc_taskinfo fields (pti_priority, CPU time, faults,
             * syscalls, context switches, ...) live only in the opaque task
             * struct or in live per-CPU counters, with no resolvable accessor, so
             * this daemon-less fallback leaves them zero. The daemon path above
             * supplies the exact values when connected.
             */
            error = procfs_copy_data((const char *)&info, sizeof(info), uio);
        } else {
            error = ESRCH;
        }
        proc_rele(p);
    }

    return error;
}

/*
 * Reads basic info for a thread. Populates an instance of a proc_threadinfo 
 * structure and copies it to tthe area described by a uio structure.
 */
int
procfs_dothreadinfo(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    /*
     * Get the process id and thread from the node id in the pfsnode and locate
     * the process.
     */
    int error = 0;

    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p != PROC_NULL) {
        struct proc_threadinfo info;
        uint64_t threadid = pnp->node_id.nodeid_objectid;

        /*
         * Preferred: the daemon returns the exact per-thread info via
         * proc_pidinfo(PROC_PIDTHREADID64INFO), keyed on thread_id == our tid.
         * Fall back to the local (zeroed on arm64) proc_pidthreadinfo otherwise.
         */
        uint32_t got = 0;
        if (procfs_ctl_request(PROCFS_REQ_THREADINFO, pnp->node_id.nodeid_pid,
                threadid, &info, sizeof(info), &got) == 0 && got == sizeof(info)) {
            error = procfs_copy_data((const char *)&info, sizeof(info), uio);
        } else if (proc_pidthreadinfo(p, threadid, TRUE, &info) == 0) {
            error = procfs_copy_data((const char *)&info, sizeof(info), uio);
        } else {
            error = ESRCH;
        }
        proc_rele(p);
    }

    return error;
}

/*
 * Gets the value of the st_size field of a node's attributes. POSIX
 * allows us to use this value as we choose. This function calculates 
 * the appropriate size for a node by calling that node's psn_getsize()
 * function. For directories, the size reported by all non-directory
 * child nodes is aggregated to get the result.
 */
size_t
procfs_get_node_size_attr(pfsnode_t *pnp, kauth_cred_t creds)
{
    pfssnode_t *snode = pnp->node_structure_node;
    pfstype node_type = snode->psn_node_type;

    /*
     * In the special cases of "." and "..", we need to first move up
     * to the parent and grandparent structure node to get the correct result.
     */
    if (node_type == PFSdirthis) {
        snode = snode->psn_parent;
    } else if (node_type == PFSdirparent) {
        snode = snode->psn_parent;
        if (snode != NULL && snode->psn_node_type != PFSroot) {
            snode = snode->psn_parent;
        }
    }

    assert(snode != NULL);

    /*
     * For file types, get the size from the node itself. For
     * directory types, get the size by traversing child structure
     * nodes and adding in any implied children, such as process and
     * thread entries.
     */
    size_t size = 0;
    if (procfs_is_directory_type(node_type)) {
        /*
         * Directory
         */
        pfssnode_t *next_snode;
        TAILQ_FOREACH(next_snode, &snode->psn_children, psn_next) {
            procfs_node_size_fn node_size_fn = next_snode->psn_getsize_fn;
            size += node_size_fn == NULL ? 1 : node_size_fn(pnp, creds);
        }
    } else {
        /*
         * File or symlink
         */
        procfs_node_size_fn node_size_fn = snode->psn_getsize_fn;
        size = node_size_fn == NULL ? snode->psn_node_size : node_size_fn(pnp, creds);
    }
    
    return size;
}

/*
 * Gets the size for a node that represents a process.
 */
size_t
procfs_process_node_size(pfsnode_t *pnp, kauth_cred_t creds)
{
    /*
     * Nodes of this type contribute a size of 1 for each visible process.
     */
    size_t size = 0;
    int pid = pnp->node_id.nodeid_pid;

    proc_t p = proc_find(pid);
    if (p != PROC_NULL) {
        if (size == 0) {
            size += sizeof(procfs_get_process_count(creds));
        }
        proc_rele(p);
    }

    return size;
}

/*
 * Gets the size for a node that represents a thread.
 */
size_t
procfs_thread_node_size(pfsnode_t *pnp, __unused kauth_cred_t creds)
{
    /*
     * Nodes of this type contribute a size of 1 for each thread
     * in the owning process. Because of the way the file system is
     * structured, the pid of the owning process is available in the
     * node_id of the procfs node.
     */
    size_t size = 0;
    int pid = pnp->node_id.nodeid_pid;

    proc_t p = proc_find(pid);
    if (p != PROC_NULL) {
        int tcount = procfs_get_task_thread_count(p);
        size += (tcount > 0) ? tcount : 1;
        proc_rele(p);
    }
    return size;
}
