/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_kern.c - kernel-side helpers that belong to procfs proper rather than
 * to the libkern forward-porting library.
 *
 * Three groups live here, all of which depend on this kext and therefore
 * cannot sit in a standalone library:
 *
 *   1. proc_pidtaskinfo()/proc_pidthreadinfo(). XNU's private fillers
 *      (fill_taskprocinfo, fill_taskthreadinfo) are stripped from the arm64
 *      kernel, and re-implementing them in-kernel is not possible: they need
 *      the opaque Mach struct task/struct thread layout (task_lock, the
 *      task->threads queue, task->ledger, per-task counters) plus several
 *      private symbols (vm_map_adjusted_size, recount_task_times,
 *      ledger_get_balance, counter_load). The procfsd daemon supplies the real
 *      structs instead (PROCFS_REQ_TASKINFO / PROCFS_REQ_THREADINFO); these
 *      zeroed stubs are the no-daemon fallback and the callers in
 *      procfs_status.c recompute what they can.
 *
 *   2. The /proc/locks renderer, which allocates from the kext's OSMalloc tag.
 *
 *   3. The /proc/<pid>/wchan continuation discovery, which calls the
 *      thread/pointer helpers in procfs_subr.c.
 */
#include <ptrauth.h>

#include <kern/locks.h>
#include <kern/kern_types.h>

#include <libkern/OSMalloc.h>
#include <libkern/OSAtomic.h>

#include <mach/mach_types.h>

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


#include <sys/sbuf.h>

#include <fs/procfs/procfs.h>

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

/* Defined in procfs_node.c. */
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
