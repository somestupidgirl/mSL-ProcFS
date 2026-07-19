/*
 * Copyright (c) 2005-2021 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

/*
 * Forward-ported for libkern by Sunneva N. Mariu, 2022-2026.
 *
 * This file contains Modifications of Apple's Original Code from
 * xnu/bsd/kern/proc_info.c: only the routines a third-party kernel extension needs are
 * retained, and private KPIs that such a kext cannot link have been
 * re-implemented in terms of public ones. See README.md.
 */

#include <sys/file_internal.h>
#include <sys/kauth.h>
#include <sys/mount_internal.h>
#include <sys/proc_info.h>

/*
 * The running arm64 kernel builds struct lockf with IMPORTANCE_INHERITANCE, so
 * define it before <sys/lockf.h>. (The extra lf_boosted int lands in off_t's
 * alignment padding, so the fields we read are at the same offsets either way,
 * but we match the kernel's definition to be safe.)
 */
#ifndef IMPORTANCE_INHERITANCE
#define IMPORTANCE_INHERITANCE 1
#endif

#include <kern.h>

/*
 * =========== From bsd/kern/proc_info.c ===========
 */
int
proc_pidshortbsdinfo(proc_t p, struct proc_bsdshortinfo * pbsd_shortp, __unused int zombie)
{
    unsigned int proc_flag = p->p_flag;
    uint32_t proc_status = (uint32_t)p->p_stat;

    bzero(pbsd_shortp, sizeof(struct proc_bsdshortinfo));
    pbsd_shortp->pbsi_pid = (uint32_t)proc_pid(p);
    pbsd_shortp->pbsi_ppid = (uint32_t)proc_ppid(p);
    pbsd_shortp->pbsi_pgid = (uint32_t)proc_pgrpid(p);
    pbsd_shortp->pbsi_status = proc_status;

    bcopy(&p->p_comm, &pbsd_shortp->pbsi_comm[0], MAXCOMLEN);
    pbsd_shortp->pbsi_comm[MAXCOMLEN - 1] = '\0';

    kauth_cred_t my_cred = kauth_cred_proc_ref(p);
    pbsd_shortp->pbsi_uid = kauth_cred_getuid(my_cred);
    pbsd_shortp->pbsi_gid = kauth_cred_getgid(my_cred);
    pbsd_shortp->pbsi_ruid = kauth_cred_getruid(my_cred);
    pbsd_shortp->pbsi_rgid = kauth_cred_getrgid(my_cred);
    pbsd_shortp->pbsi_svuid = kauth_cred_getsvuid(my_cred);
    pbsd_shortp->pbsi_svgid = kauth_cred_getsvgid(my_cred);
    kauth_cred_unref(&my_cred);

    pbsd_shortp->pbsi_flags = 0;

    if ((proc_flag & P_SYSTEM) == P_SYSTEM) {
        pbsd_shortp->pbsi_flags |= PROC_FLAG_SYSTEM;
    }

    if ((proc_flag & P_LTRACED) == P_LTRACED) {
        pbsd_shortp->pbsi_flags |= PROC_FLAG_TRACED;
    }

    if (proc_exiting(p) != 0) {
        pbsd_shortp->pbsi_flags |= PROC_FLAG_INEXIT;
    }

    if ((proc_flag & P_LPPWAIT) == P_LPPWAIT) {
        pbsd_shortp->pbsi_flags |= PROC_FLAG_PPWAIT;
    }

    if (proc_is64bit(p) != 0) {
        pbsd_shortp->pbsi_flags |= PROC_FLAG_LP64;
    }

    if ((proc_flag & P_CONTROLT) == P_CONTROLT) {
        pbsd_shortp->pbsi_flags |= PROC_FLAG_CONTROLT;
    }

    if ((proc_flag & P_THCWD) == P_THCWD) {
        pbsd_shortp->pbsi_flags |= PROC_FLAG_THCWD;
    }

    /* proc_issetugid() is not exported for kext linkage; read the flag it checks
     * (P_SUGID: privileges changed since last exec) from p_flag directly. */
    if ((proc_flag & P_SUGID) != 0)  {
        pbsd_shortp->pbsi_flags |= PROC_FLAG_PSUGID;
    }

    if ((proc_flag & P_EXEC) == P_EXEC) {
        pbsd_shortp->pbsi_flags |= PROC_FLAG_EXEC;
    }

    if ((proc_flag & P_DELAYIDLESLEEP) == P_DELAYIDLESLEEP) {
        pbsd_shortp->pbsi_flags |= PROC_FLAG_DELAYIDLESLEEP;
    }

    switch (PROC_CONTROL_STATE(p)) {
    case P_PCTHROTTLE:
        pbsd_shortp->pbsi_flags |= PROC_FLAG_PC_THROTTLE;
        break;
    case P_PCSUSP:
        pbsd_shortp->pbsi_flags |= PROC_FLAG_PC_SUSP;
        break;
    case P_PCKILL:
        pbsd_shortp->pbsi_flags |= PROC_FLAG_PC_KILL;
        break;
    };

    switch (PROC_ACTION_STATE(p)) {
    case P_PCTHROTTLE:
        pbsd_shortp->pbsi_flags |= PROC_FLAG_PA_THROTTLE;
        break;
    case P_PCSUSP:
        pbsd_shortp->pbsi_flags |= PROC_FLAG_PA_SUSP;
        break;
    };

    return 0;
}

#pragma mark -
#pragma mark Forward-ported fd-table KPIs

/*
 * proc_fdlock()/proc_fdunlock() are private (in no third-party-linkable KPI),
 * so they are re-implemented here as the thin file-descriptor table mutex
 * wrappers they are in XNU. proc_fdlist() below relies on them.
 */
void
proc_fdlock(struct proc *p)
{
    lck_mtx_lock(&p->p_fd.fd_lock);
}

void
proc_fdunlock(struct proc *p)
{
    lck_mtx_unlock(&p->p_fd.fd_lock);
}

/*
 * Cheap sanity check on the filedesc layout before any lock is taken. fd_nfiles
 * and fd_afterlast precede every config-guarded field in struct filedesc, so
 * they sit at a fixed offset; if struct proc's CONFIG_* does not match the
 * running kernel and p_fd lands at the wrong offset, they read as implausible
 * values. Returning false makes a layout mismatch degrade to "no descriptors"
 * instead of panicking on an invalid mutex (fd_lock). Reading these ints is
 * safe even when the offset is off, because the address still lies within the
 * struct proc allocation; only locking a bogus mutex would panic.
 */
#define FD_NFILES_MAX  (1 << 20)
static boolean_t
fd_layout_ok(struct filedesc *fdp)
{
    int nfiles = fdp->fd_nfiles;
    int afterlast = fdp->fd_afterlast;
    return nfiles > 0 && nfiles <= FD_NFILES_MAX &&
           afterlast >= 0 && afterlast <= nfiles;
}

/*
 * proc_fdlist() - re-implementation of the com.apple.kpi.private KPI (which a
 * third-party kext cannot link). With buf == NULL it returns an upper bound on
 * the descriptor count (the table high-water mark); otherwise it fills up to
 * *count entries with the open descriptors and their types and updates *count
 * to the number written. Mirrors proc_fdlist_internal() in XNU; locking is
 * taken internally.
 */
int
proc_fdlist(proc_t p, struct proc_fdinfo *buf, size_t *count)
{
    if (p == NULL || count == NULL) {
        return EINVAL;
    }

    struct filedesc *fdp = &p->p_fd;

    if (!fd_layout_ok(fdp)) {
        *count = 0;
        return EINVAL;
    }

    if (buf == NULL) {
        proc_fdlock(p);
        *count = (size_t)fdp->fd_afterlast;
        proc_fdunlock(p);
        return 0;
    }

    size_t numfds = *count;
    size_t n = 0;

    proc_fdlock(p);
    for (int fd = 0; fd < fdp->fd_afterlast && n < numfds; fd++) {
        struct fileproc *fp = fdp->fd_ofiles[fd];
        if (fp == NULL || fp->fp_glob == NULL) {
            continue;
        }
        if (fdp->fd_ofileflags[fd] & UF_RESERVED) {
            continue;
        }
        file_type_t fdtype = FILEGLOB_DTYPE(fp->fp_glob);
        buf[n].proc_fd = fd;
        buf[n].proc_fdtype = (fdtype != DTYPE_ATALK) ? fdtype : PROX_FDTYPE_ATALK;
        n++;
    }
    proc_fdunlock(p);

    *count = n;
    return 0;
}
