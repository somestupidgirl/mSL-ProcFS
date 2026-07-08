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
#include <sys/sbuf.h>
#include <kern/locks.h>
#include <kern/kern_types.h>
#include <libkern/OSMalloc.h>
#include <libkern/OSAtomic.h>
#include <mach/mach_types.h>
#include <ptrauth.h>

/*
 * The running arm64 kernel builds struct lockf with IMPORTANCE_INHERITANCE, so
 * define it before <sys/lockf.h>. (The extra lf_boosted int lands in off_t's
 * alignment padding, so the fields we read are at the same offsets either way,
 * but we match the kernel's definition to be safe.)
 */
#ifndef IMPORTANCE_INHERITANCE
#define IMPORTANCE_INHERITANCE 1
#endif
#include <sys/lockf.h>


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
    /*
     * vn_stat() (for vi_stat) and the dead_mountp global (for vi_fsid) are
     * com.apple.kpi.private/unresolved on arm64, so those fields are left zero;
     * fd fileinfo consumers rely on vi_type, which the public vnode_vtype() KPI
     * supplies. (vnode_getattr()/vfs_statfs() could fill the rest as a later
     * public-KPI enhancement.)
     */
    struct stat64 sb;
    bzero(&sb, sizeof(sb));
    munge_vinfo_stat(&sb, &vinfo->vi_stat);     /* deterministically zeroed */

    vinfo->vi_fsid.val[0] = 0;
    vinfo->vi_fsid.val[1] = 0;
    vinfo->vi_type = vnode_vtype(vp);

    return 0;
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

/*
 * procfs_build_locks() - render Linux's /proc/locks: the byte-range (advisory)
 * file locks the kernel currently holds. XNU keeps these per-vnode on
 * vp->v_lockf with no global registry. Rather than the fd-table walk (whose
 * struct filedesc offsets drift across kernel point-releases - which is why
 * /proc/<pid>/fd itself prefers the daemon), we enumerate vnodes directly with
 * the public VFS iterators: vfs_iterate() over every mount, vnode_iterate() over
 * each mount's vnodes. Each vnode is visited once (no de-dup needed) with a
 * reference held for the duration of the callout, so we can safely read its
 * v_lockf list and, if non-empty, resolve its identity with vnode_getattr().
 *
 * The v_lockf list is protected by the vnode mutex vp->v_lock, which we take
 * with the public lck_mtx KPI - this forward-ports vnode_lock(), which is
 * exactly lck_mtx_lock(&vp->v_lock), rather than linking the private symbol. We
 * snapshot the locks under the mutex, drop it, then format outside it (sbuf and
 * vnode_getattr must not run under v_lock). The line layout mirrors Linux's
 * fs/locks.c; XNU has no mandatory locking, so every lock is ADVISORY:
 *
 *   <n>: <POSIX|FLOCK|OFDLCK>  ADVISORY  <READ|WRITE>  <pid>  <maj:min:ino> <start> <end|EOF>
 *
 * Best-effort under load; an empty file (no locks held) is the common result.
 */
struct procfs_lockrec {
    short   flags;
    short   type;
    off_t   start;
    off_t   end;
    pid_t   pid;
};

#define PROCFS_LOCKS_MAX          1024   /* global cap on emitted lock lines */
#define PROCFS_LOCKS_PER_VNODE    128    /* cap on locks snapshotted per vnode */

/* Defined by the kext (procfs_node.c); libkprocfs links into the same kext. */
extern OSMallocTag procfs_osmalloc_tag;

struct procfs_locks_ctx {
    struct sbuf           *sb;
    vfs_context_t          ctx;
    struct procfs_lockrec *recs;    /* scratch: PROCFS_LOCKS_PER_VNODE entries */
    int                    emitted;
};

/* Called by vnode_iterate() for each vnode of a mount, with a reference held. */
static int
procfs_locks_vnode_cb(struct vnode *vp, void *arg)
{
    struct procfs_locks_ctx *c = arg;

    if (c->emitted >= PROCFS_LOCKS_MAX) {
        return VNODE_RETURNED_DONE;
    }

    /* Snapshot this vnode's lock list under the vnode mutex. */
    int nrec = 0;
    lck_mtx_lock(&vp->v_lock);
    if (vp->v_lockf != NULL) {
        for (struct lockf *lf = vp->v_lockf;
             lf != NULL && nrec < PROCFS_LOCKS_PER_VNODE;
             lf = lf->lf_next) {
            c->recs[nrec].flags = lf->lf_flags;
            c->recs[nrec].type  = lf->lf_type;
            c->recs[nrec].start = lf->lf_start;
            c->recs[nrec].end   = lf->lf_end;
            c->recs[nrec].pid   = (lf->lf_owner != NULL)
                ? proc_pid(lf->lf_owner) : -1;
            nrec++;
        }
    }
    lck_mtx_unlock(&vp->v_lock);

    if (nrec > 0) {
        /* maj:min:inode of the file, resolved once per vnode. */
        uint32_t vmaj = 0, vmin = 0;
        uint64_t ino  = 0;
        struct vnode_attr va;
        VATTR_INIT(&va);
        VATTR_WANTED(&va, va_fileid);
        VATTR_WANTED(&va, va_fsid);
        if (vnode_getattr(vp, &va, c->ctx) == 0) {
            if (VATTR_IS_SUPPORTED(&va, va_fileid)) {
                ino = va.va_fileid;
            }
            if (VATTR_IS_SUPPORTED(&va, va_fsid)) {
                vmaj = major(va.va_fsid);
                vmin = minor(va.va_fsid);
            }
        }

        for (int r = 0; r < nrec && c->emitted < PROCFS_LOCKS_MAX; r++) {
            const char *class =
                (c->recs[r].flags & F_OFD_LOCK) ? "OFDLCK" :
                (c->recs[r].flags & F_FLOCK)    ? "FLOCK " : "POSIX ";
            const char *mode =
                (c->recs[r].type == F_WRLCK) ? "WRITE" :
                (c->recs[r].type == F_RDLCK) ? "READ"  : "UNLCK";

            sbuf_printf(c->sb,
                "%d: %s ADVISORY  %-5s %d %02x:%02x:%llu %lld ",
                ++c->emitted, class, mode, (int)c->recs[r].pid,
                vmaj, vmin, (unsigned long long)ino,
                (long long)c->recs[r].start);
            if (c->recs[r].end == -1) {
                sbuf_printf(c->sb, "EOF\n");
            } else {
                sbuf_printf(c->sb, "%lld\n", (long long)c->recs[r].end);
            }
        }
    }

    return VNODE_RETURNED;   /* release the reference vnode_iterate holds */
}

/* Called by vfs_iterate() for each mount: iterate that mount's vnodes. */
static int
procfs_locks_mount_cb(struct mount *mp, void *arg)
{
    struct procfs_locks_ctx *c = arg;
    (void)vnode_iterate(mp, 0, procfs_locks_vnode_cb, arg);
    return (c->emitted >= PROCFS_LOCKS_MAX) ? VFS_RETURNED_DONE : VFS_RETURNED;
}

void
procfs_build_locks(struct sbuf *sb, vfs_context_t ctx)
{
    const uint32_t recs_sz = PROCFS_LOCKS_PER_VNODE * sizeof(struct procfs_lockrec);
    struct procfs_lockrec *recs = OSMalloc(recs_sz, procfs_osmalloc_tag);
    if (recs == NULL) {
        return;
    }

    struct procfs_locks_ctx c = {
        .sb = sb, .ctx = ctx, .recs = recs, .emitted = 0,
    };

    (void)vfs_iterate(0, procfs_locks_mount_cb, &c);

    OSFree(recs, recs_sz, procfs_osmalloc_tag);
}


/*
 * ================= /proc/<pid>/wchan support =================
 *
 * Name the kernel function a task is blocked in. XNU has no KPI for this and
 * struct thread is opaque, so its field offsets are unknown at compile time. We
 * recover the offset of the 'continuation' field - the function a blocked thread
 * resumes at, i.e. exactly the wchan we want - at RUNTIME and config-
 * independently: start a helper kernel thread, block it with a known continuation
 * via thread_block_parameter(), then scan its struct thread for the field holding
 * that pointer (PAC-stripped for the arm64e signature). The offset is cached.
 *
 * For a target process we read its representative thread's continuation, strip
 * its PAC, un-slide it to the kernel link address (vm_kernel_unslide_...), and
 * hand that to the daemon to symbolize (PROCFS_REQ_KSYM_LOOKUP). A thread with a
 * NULL continuation yields wchan "0".
 */
extern kern_return_t kernel_thread_start(thread_continue_t, void *, thread_t *);
extern wait_result_t assert_wait(event_t, wait_interrupt_t);
extern wait_result_t thread_block_parameter(thread_continue_t, void *);
extern wait_result_t thread_block(thread_continue_t);
extern kern_return_t thread_wakeup_prim(event_t, boolean_t, wait_result_t);
extern kern_return_t thread_terminate(thread_t);
extern void          thread_deallocate(thread_t);
extern void          vm_kernel_unslide_or_perm_external(vm_offset_t, vm_offset_t *);
extern void          IODelay(unsigned microseconds);
extern boolean_t     procfs_kernel_ptr_ok(uintptr_t va);       /* procfs_subr.c */
extern thread_t      procfs_get_representative_thread(proc_t p); /* procfs_subr.c */
extern int           procfs_get_thread_ptrs(proc_t p, thread_t *out, int max); /* procfs_subr.c */

#define PROCFS_WCHAN_SCAN_MAX  2048     /* struct-thread bytes scanned for the offset */

static volatile int32_t g_wchan_init        = 0;   /* 0 = unclaimed, 1 = claimed */
static volatile long    g_continuation_off  = -1;  /* offset, or <0 = not available */

/* The helper's continuation: the pointer we search for. On resume, exit. */
static void
procfs_wchan_probe_cont(void *param, wait_result_t wr)
{
    (void)param;
    (void)wr;
    thread_terminate(current_thread());
    thread_block(THREAD_CONTINUE_NULL);
    /* NOTREACHED */
}

struct procfs_wchan_probe {
    int event;
};

/* Helper thread: block with the known continuation so we can locate its field. */
static void
procfs_wchan_probe_thread(void *param, wait_result_t wr)
{
    struct procfs_wchan_probe *pr = param;
    (void)wr;
    assert_wait(&pr->event, THREAD_UNINT);
    thread_block_parameter(procfs_wchan_probe_cont, param);
    /* NOTREACHED */
}

/* Discover the continuation offset once (idempotent, guarded by a CAS claim). */
static void
procfs_wchan_discover(void)
{
    if (g_continuation_off != -1) {
        return;                         /* already resolved (>=0) or failed (-2) */
    }
    if (!OSCompareAndSwap(0, 1, (volatile UInt32 *)&g_wchan_init)) {
        return;                         /* another thread owns the discovery */
    }

    struct procfs_wchan_probe pr = { .event = 0 };
    thread_t hthread = THREAD_NULL;
    if (kernel_thread_start(procfs_wchan_probe_thread, &pr, &hthread) != KERN_SUCCESS) {
        g_continuation_off = -2;
        return;
    }

    uintptr_t target = (uintptr_t)ptrauth_strip((void *)procfs_wchan_probe_cont,
        ptrauth_key_function_pointer);
    long found = -1;
    for (int tries = 0; tries < 100000 && found < 0; tries++) {
        if (procfs_kernel_ptr_ok((uintptr_t)hthread)) {
            const uintptr_t *w = (const uintptr_t *)hthread;
            for (long off = 0;
                 off + (long)sizeof(uintptr_t) <= PROCFS_WCHAN_SCAN_MAX;
                 off += (long)sizeof(uintptr_t)) {
                uintptr_t v = w[off / (long)sizeof(uintptr_t)];
                if (v == 0) {
                    continue;
                }
                if ((uintptr_t)ptrauth_strip((void *)v,
                        ptrauth_key_function_pointer) == target) {
                    found = off;
                    break;
                }
            }
        }
        if (found < 0) {
            IODelay(10);
        }
    }

    /* Wake the helper so it exits, whether or not we located the field. */
    thread_wakeup_prim(&pr.event, FALSE, THREAD_AWAKENED);
    thread_deallocate(hthread);

    g_continuation_off = (found >= 0) ? found : -2;
}

/*
 * Fill *unslid with the un-slid kernel address of the process's representative
 * thread's continuation (the wchan), or 0 if it has none. Returns 0 on success.
 */
#define PROCFS_WCHAN_MAX_THREADS 64

/*
 * Fill *runtime with the (PAC-stripped) runtime kernel address of the process's
 * representative blocked thread's continuation - its wchan - or 0 if it has none.
 * The caller subtracts the kernel slide to get the symbol-source address and
 * symbolizes it. Returns 0 on success.
 */
int
procfs_thread_continuation(proc_t p, uint64_t *runtime)
{
    *runtime = 0;

    procfs_wchan_discover();
    if (g_continuation_off < 0) {
        return ENOTSUP;                 /* discovery unavailable */
    }

    thread_t th[PROCFS_WCHAN_MAX_THREADS];
    int nth = procfs_get_thread_ptrs(p, th, PROCFS_WCHAN_MAX_THREADS);

    for (int i = 0; i < nth; i++) {
        uintptr_t base = (uintptr_t)th[i];
        uintptr_t addr = base + (uintptr_t)g_continuation_off;
        if (!procfs_kernel_ptr_ok(base) || !procfs_kernel_ptr_ok(addr)) {
            continue;
        }
        uintptr_t v = *(const uintptr_t *)addr;
        if (v != 0) {                   /* first thread with a continuation */
            *runtime = (uint64_t)(uintptr_t)ptrauth_strip((void *)v,
                ptrauth_key_function_pointer);
            break;
        }
    }
    return 0;
}
