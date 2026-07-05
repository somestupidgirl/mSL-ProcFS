/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 */
#include <os/log.h>
#include <os/refcnt.h>
#include <sys/fcntl.h>
#include <sys/guarded.h>
#include <sys/file.h>
#include <sys/file_internal.h>
#include <sys/filedesc.h>
#include <sys/kauth.h>
#include <sys/mount_internal.h>
#include <sys/param.h>
#include <sys/kpi_socket.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#include <sys/proc_internal.h>
#include <sys/queue.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/syslimits.h>
#include <sys/types.h>
#include <sys/user.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <mach/mach_types.h>
#include <ptrauth.h>

#include "symbols.h"

/*
 * =========== From bsd/kern/bsd_init.c ===========
 */
proc_t XNU_PTRAUTH_SIGNED_PTR("initproc") initproc;

/*
 * =========== From bsd/conf/param.c ===========
 */
#if !defined(__x86_64__)
#define NPROC 1000          /* Account for DEFAULT_TOTAL_CORPSES_ALLOWED by making this slightly lower than we can. */
#define NPROC_PER_UID 950
#else
#define NPROC (20 + 32 * 32)
#define NPROC_PER_UID (NPROC/2)
#endif

/* NOTE: maxproc and hard_maxproc values are subject to device specific scaling in bsd_scale_setup */
#define HNPROC 2500     /* based on thread_max */
int     maxproc = NPROC;
int     maxprocperuid = NPROC_PER_UID;

#if !defined(__x86_64__)
int hard_maxproc = NPROC;       /* hardcoded limit -- for ARM the number of processes is limited by the ASID space */
#else
int hard_maxproc = HNPROC;      /* hardcoded limit */
#endif

int nprocs = 0; /* XXX */

/*
 * =========== From bsd/kern/kern_proc.c ===========
 */

/*
 * The pidlist_* routines support the functions in this file that
 * walk lists of processes applying filters and callouts to the
 * elements of the list.
 *
 * A prior implementation used a single linear array, which can be
 * tricky to allocate on large systems. This implementation creates
 * an SLIST of modestly sized arrays of PIDS_PER_ENTRY elements.
 *
 * The array should be sized large enough to keep the overhead of
 * walking the list low, but small enough that blocking allocations of
 * pidlist_entry_t structures always succeed.
 */

#define PIDS_PER_ENTRY 1021

typedef struct pidlist_entry {
    SLIST_ENTRY(pidlist_entry) pe_link;
    u_int pe_nused;
    pid_t pe_pid[PIDS_PER_ENTRY];
} pidlist_entry_t;

typedef struct {
    SLIST_HEAD(, pidlist_entry) pl_head;
    struct pidlist_entry *pl_active;
    u_int pl_nalloc;
} pidlist_t;

static __inline__ pidlist_t *
pidlist_init(pidlist_t *pl)
{
    SLIST_INIT(&pl->pl_head);
    pl->pl_active = NULL;
    pl->pl_nalloc = 0;
    return pl;
}

static u_int
pidlist_alloc(pidlist_t *pl, u_int needed)
{
    while (pl->pl_nalloc < needed) {
        pidlist_entry_t *pe = kalloc_type(pidlist_entry_t,
            Z_WAITOK | Z_ZERO | Z_NOFAIL);
        SLIST_INSERT_HEAD(&pl->pl_head, pe, pe_link);
        pl->pl_nalloc += (sizeof(pe->pe_pid) / sizeof(pe->pe_pid[0]));
    }
    return pl->pl_nalloc;
}

static void
pidlist_free(pidlist_t *pl)
{
    pidlist_entry_t *pe;
    while (NULL != (pe = SLIST_FIRST(&pl->pl_head))) {
        SLIST_FIRST(&pl->pl_head) = SLIST_NEXT(pe, pe_link);
        kfree_type(pidlist_entry_t, pe);
    }
    pl->pl_nalloc = 0;
}

static __inline__ void
pidlist_set_active(pidlist_t *pl)
{
    pl->pl_active = SLIST_FIRST(&pl->pl_head);
    assert(pl->pl_active);
}

static void
pidlist_add_pid(pidlist_t *pl, pid_t pid)
{
    pidlist_entry_t *pe = pl->pl_active;
    if (pe->pe_nused >= sizeof(pe->pe_pid) / sizeof(pe->pe_pid[0])) {
        if (NULL == (pe = SLIST_NEXT(pe, pe_link))) {
            panic("pidlist allocation exhausted");
        }
        pl->pl_active = pe;
    }
    pe->pe_pid[pe->pe_nused++] = pid;
}

static __inline__ u_int
pidlist_nalloc(const pidlist_t *pl)
{
    return pl->pl_nalloc;
}

struct proclist allproc = LIST_HEAD_INITIALIZER(allproc);
struct proclist zombproc = LIST_HEAD_INITIALIZER(zombproc);

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

    if (_proc_issetugid != NULL && proc_issetugid(p) != 0)  {
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

/*
 * proc_pidtaskinfo / proc_pidthreadinfo: the private fillers (fill_taskprocinfo,
 * fill_taskthreadinfo) are stripped from the arm64 kernel and were never
 * resolvable, so these return a zeroed struct - the procfsd daemon supplies the
 * real proc_taskinfo / proc_threadinfo (PROCFS_REQ_TASKINFO / THREADINFO), and
 * the callers in procfs_status.c recompute what they can without the daemon.
 */
int
proc_pidtaskinfo(__unused proc_t p, struct proc_taskinfo * ptinfo)
{
    bzero(ptinfo, sizeof(struct proc_taskinfo));
    return 0;
}

int
proc_pidthreadinfo(__unused proc_t p, __unused uint64_t arg, __unused bool thuniqueid,
    struct proc_threadinfo *pthinfo)
{
    bzero(pthinfo, sizeof(struct proc_threadinfo));
    return 0;
}

/*
 * copy stat64 structure into vinfo_stat structure.
 */
static void
munge_vinfo_stat(struct stat64 *sbp, struct vinfo_stat *vsbp)
{
    bzero(vsbp, sizeof(struct vinfo_stat));

    vsbp->vst_dev = sbp->st_dev;
    vsbp->vst_mode = sbp->st_mode;
    vsbp->vst_nlink = sbp->st_nlink;
    vsbp->vst_ino = sbp->st_ino;
    vsbp->vst_uid = sbp->st_uid;
    vsbp->vst_gid = sbp->st_gid;
    vsbp->vst_atime = sbp->st_atimespec.tv_sec;
    vsbp->vst_atimensec = sbp->st_atimespec.tv_nsec;
    vsbp->vst_mtime = sbp->st_mtimespec.tv_sec;
    vsbp->vst_mtimensec = sbp->st_mtimespec.tv_nsec;
    vsbp->vst_ctime = sbp->st_ctimespec.tv_sec;
    vsbp->vst_ctimensec = sbp->st_ctimespec.tv_nsec;
    vsbp->vst_birthtime = sbp->st_birthtimespec.tv_sec;
    vsbp->vst_birthtimensec = sbp->st_birthtimespec.tv_nsec;
    vsbp->vst_size = sbp->st_size;
    vsbp->vst_blocks = sbp->st_blocks;
    vsbp->vst_blksize = sbp->st_blksize;
    vsbp->vst_flags = sbp->st_flags;
    vsbp->vst_gen = sbp->st_gen;
    vsbp->vst_rdev = sbp->st_rdev;
    vsbp->vst_qspare[0] = sbp->st_qspare[0];
    vsbp->vst_qspare[1] = sbp->st_qspare[1];
}

int
fill_vnodeinfo(vnode_t vp, struct vnode_info *vinfo, __unused boolean_t check_fsgetpath)
{
    vfs_context_t context;
    struct stat64 sb;
    int error = 0;

    struct vnode_attr *vap;

    bzero(&sb, sizeof(struct stat64));
    context = vfs_context_create((vfs_context_t)0);

    if (!error) {
        if (_vn_stat != NULL) {
            error = vn_stat(vp, &sb, NULL, 1, 0, context);
            munge_vinfo_stat(&sb, &vinfo->vi_stat);
        }
    }

    (void)vfs_context_rele(context);

    if (error != 0) {
        goto out;
    }

    if (_dead_mountp != NULL && vnode_mount(vp) != dead_mountp) {
        vinfo->vi_fsid = vp->v_mount->mnt_vfsstat.f_fsid;
    } else {
        vinfo->vi_fsid.val[0] = 0;
        vinfo->vi_fsid.val[1] = 0;
    }
    vinfo->vi_type = vnode_vtype(vp);

out:
    return error;
}


/*
 * Returns the fdp pointer for the specified
 * process.
 */
static inline volatile struct filedesc *
proc_fdp(proc_t p)
{
    return (volatile struct filedesc *)&p->p_fd;
}

int
fill_fileinfo(struct fileproc * fp, proc_t p, int fd, struct proc_fileinfo * fi)
{
    uint32_t openflags = 0;
    uint32_t status = 0;
    off_t offset = 0;
    int32_t type = 0;
    uint32_t guardflags = 0;

    if (fp != FILEPROC_NULL) {
        openflags = fp->fp_glob->fg_flag;
        offset = fp->fp_glob->fg_offset;
        type = FILEGLOB_DTYPE(fp->fp_glob);

        if (os_ref_get_count_raw(&fp->fp_glob->fg_count) > 1) {
            status |= PROC_FP_SHARED;
        }

        if (p != PROC_NULL) {
            // The caller must hold proc_fdlock(p) (which keeps fp alive); read
            // the per-fd flags directly. This routine no longer takes the lock
            // itself so it can be called from a section that already holds it.
            if (fp->fp_flags & FP_CLOEXEC) {
                status |= PROC_FP_CLEXEC;
            }
            if (fp->fp_flags & FP_CLOFORK) {
                status |= PROC_FP_CLFORK;
            }
        }

        if (fp->fp_guard_attrs != 0) {
            status |= PROC_FP_GUARDED;
            if (fp->fp_guard_attrs & GUARD_CLOSE) {
                guardflags |= PROC_FI_GUARD_CLOSE;
            }
            if (fp->fp_guard_attrs & GUARD_DUP) {
                guardflags |= PROC_FI_GUARD_DUP;
            }
            if (fp->fp_guard_attrs & GUARD_SOCKET_IPC) {
                guardflags |= PROC_FI_GUARD_SOCKET_IPC;
            }
            if (fp->fp_guard_attrs & GUARD_FILEPORT) {
                guardflags |= PROC_FI_GUARD_FILEPORT;
            }
        }

    }
    bzero(fi, sizeof(struct proc_fileinfo));
    fi->fi_openflags = openflags;
    fi->fi_status = status;
    fi->fi_offset = offset;
    fi->fi_type = type;
    fi->fi_guardflags = guardflags;

    return (0);
}

#pragma mark -
#pragma mark Forward-ported fd-table KPIs

/*
 * proc_fdlock()/proc_fdunlock() are private (in no third-party-linkable KPI),
 * so they are re-implemented here as the thin file-descriptor table mutex
 * wrappers they are in XNU. fill_fileinfo() above and the routines below rely
 * on them.
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
#define PROCFS_FD_NFILES_MAX  (1 << 20)
static boolean_t
procfs_fd_layout_ok(struct filedesc *fdp)
{
    int nfiles = fdp->fd_nfiles;
    int afterlast = fdp->fd_afterlast;
    return nfiles > 0 && nfiles <= PROCFS_FD_NFILES_MAX &&
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

    if (!procfs_fd_layout_ok(fdp)) {
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

/*
 * procfs_fg_get_data() - re-implementation of the private fg_get_data_volatile().
 * fg_data is a manually PAC-signed pointer (the struct field is a bare uintptr_t,
 * so the compiler does not auto-authenticate it like XNU_PTRAUTH_SIGNED_PTR
 * fields). Reading it raw yields a signed pointer that faults when dereferenced,
 * so authenticate it exactly as XNU does before use.
 */
static void *
procfs_fg_get_data(struct fileglob *fg)
{
    uintptr_t *store = &fg->fg_data;
    void *data = (void *)*store;
#if __has_feature(ptrauth_calls)
    if (data) {
        int type = FILEGLOB_DTYPE(fg);
        type ^= OS_PTRAUTH_DISCRIMINATOR("fileglob.fg_data");
        data = ptrauth_auth_data(data,
            ptrauth_key_process_independent_data,
            ptrauth_blend_discriminator(store, type));
    }
#endif
    return data;
}

/*
 * procfs_fd_vnode_info() - replaces the private fp_getfvp() for procfs's needs
 * without taking a fileproc iocount (the os_ref retain/release path bottoms out
 * in os_ref_*_internal, which a third-party kext cannot link). Under
 * proc_fdlock() - which keeps the fileproc alive - it validates that fd is a
 * vnode-backed descriptor of process p, captures the vnode and its vnode id,
 * and fills the proc_fileinfo. The caller then takes a vnode iocount with
 * vnode_getwithvid(*vidp); the id guards against the vnode being reclaimed
 * after the lock is dropped, so no fileproc reference is required. Returns
 * EBADF for an invalid or non-vnode descriptor.
 */
int
procfs_fd_vnode_info(proc_t p, int fd, struct vnode **vpp, uint32_t *vidp, struct proc_fileinfo *fi)
{
    struct filedesc *fdp = &p->p_fd;
    struct fileproc *fp;
    vnode_t vp;

    if (!procfs_fd_layout_ok(fdp)) {
        return EBADF;
    }

    proc_fdlock(p);
    if (fd < 0 || fd >= fdp->fd_nfiles ||
        (fp = fdp->fd_ofiles[fd]) == NULL || fp->fp_glob == NULL ||
        (fdp->fd_ofileflags[fd] & UF_RESERVED)) {
        proc_fdunlock(p);
        return EBADF;
    }
    if (FILEGLOB_DTYPE(fp->fp_glob) != DTYPE_VNODE) {
        proc_fdunlock(p);
        return EBADF;
    }
    vp = (vnode_t)procfs_fg_get_data(fp->fp_glob);
    if (vp == NULLVP) {
        proc_fdunlock(p);
        return EBADF;
    }
    *vpp = vp;
    *vidp = vnode_vid(vp);
    fill_fileinfo(fp, p, fd, fi);   /* we hold proc_fdlock */
    proc_fdunlock(p);
    return 0;
}

/*
 * procfs_fd_socket() - socket counterpart of procfs_fd_vnode_info(). Sockets
 * have no vnode-id equivalent to guard against reuse, so under proc_fdlock
 * (which keeps the fileproc, hence the socket, alive) it takes a socket
 * reference with sock_retain() that the caller must drop with sock_release().
 * It also fills the proc_fileinfo. Returns EBADF for an invalid or non-socket
 * descriptor.
 *
 * sock_retain() acquires the socket lock while proc_fdlock is held; this nests
 * in the natural order (descriptor layer above socket layer) and cannot
 * deadlock, since the socket layer never reaches back up to proc_fdlock.
 */
int
procfs_fd_socket(proc_t p, int fd, socket_t *sop, struct proc_fileinfo *fi)
{
    struct filedesc *fdp = &p->p_fd;
    struct fileproc *fp;
    socket_t so;

    if (!procfs_fd_layout_ok(fdp)) {
        return EBADF;
    }

    proc_fdlock(p);
    if (fd < 0 || fd >= fdp->fd_nfiles ||
        (fp = fdp->fd_ofiles[fd]) == NULL || fp->fp_glob == NULL ||
        (fdp->fd_ofileflags[fd] & UF_RESERVED)) {
        proc_fdunlock(p);
        return EBADF;
    }
    if (FILEGLOB_DTYPE(fp->fp_glob) != DTYPE_SOCKET) {
        proc_fdunlock(p);
        return EBADF;
    }
    so = (socket_t)procfs_fg_get_data(fp->fp_glob);
    if (so == NULL) {
        proc_fdunlock(p);
        return EBADF;
    }
    sock_retain(so);
    fill_fileinfo(fp, p, fd, fi);   /* we hold proc_fdlock */
    proc_fdunlock(p);

    *sop = so;
    return 0;
}

