/*
 * Copyright (c) 2015 Kim Topley
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_subr.c
 *
 * Utility functions for the ProcFS file system.
 */
#include <kern/task.h>
#include <kern/thread.h>
#include <libkern/OSMalloc.h>
#include <mach/mach_types.h>
#include <mach/message.h>
#include <mach/task.h>
#include <sys/kauth.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <sys/user.h>
#include <sys/uio.h>
#include <sys/proc_info.h>
#include <sys/ucred.h>
#include <sys/vnode.h>

#include <bsdcompat/sys/malloc.h>

#include <fs/procfs/procfs.h>
#include <fs/procfs/procfs_ctl.h>

#include <libkprocfs/symbols.h>

/* 
 * Allocate a pfsnode/vnode pair. Gets the vnode type that is appropriate
 * for a given structure node type.
 */
enum vtype
procfs_allocvp(pfstype pfs_type)
{
    switch (pfs_type) {
    case PFSroot:           /* FALLTHROUGH */
    case PFSproc:           /* FALLTHROUGH */
    case PFSthread:         /* FALLTHROUGH */
    case PFSdir:            /* FALLTHROUGH */
    case PFSdirthis:        /* FALLTHROUGH */
    case PFSdirparent:      /* FALLTHROUGH */
    case PFSfd:             /* FALLTHROUGH */
    case PFSsysctl:         /* default; the true vtype is per-oid (see vnops) */
        return VDIR;

    case PFSfile:           /* FALLTHROUGH */
    case PFScpuinfo:        /* FALLTHROUGH */
    case PFSloadavg:        /* FALLTHROUGH */
    case PFSpartitions:     /* FALLTHROUGH */
    case PFSversion:        /* FALLTHROUGH */
    case PFSmeminfo:        /* FALLTHROUGH */
    case PFSmtab:           /* FALLTHROUGH */
    case PFSstat:           /* FALLTHROUGH */
    case PFSvmstat:         /* FALLTHROUGH */
    case PFSuptime:         /* FALLTHROUGH */
    case PFSswaps:          /* FALLTHROUGH */
    case PFSfilesystems:    /* FALLTHROUGH */
    case PFSextensions:     /* FALLTHROUGH */
    case PFSmodules:        /* FALLTHROUGH */
    case PFSdiskstats:      /* FALLTHROUGH */
    case PFSdevices:        /* FALLTHROUGH */
    case PFSnetdev:         /* FALLTHROUGH */
    case PFSallocinfo:      /* FALLTHROUGH */
    case PFSapm:            /* FALLTHROUGH */
    case PFSbootconfig:     /* FALLTHROUGH */
    case PFSbuddyinfo:      /* FALLTHROUGH */
    case PFSpcidevices:     /* FALLTHROUGH */
    case PFSdma:            /* FALLTHROUGH */
    case PFSrtc:            /* FALLTHROUGH */
    case PFSexecdomains:    /* FALLTHROUGH */
    case PFSfb:             /* FALLTHROUGH */
    case PFSnfsexports:     /* FALLTHROUGH */
    case PFSinterrupts:     /* FALLTHROUGH */
    case PFSirq:            /* FALLTHROUGH */
    case PFStty:            /* FALLTHROUGH */
    case PFSkcmdline:       /* FALLTHROUGH */
        return VREG;

    case PFSprocnamedir:    /* FALLTHROUGH */
    case PFScurproc:        /* FALLTHROUGH */
    case PFSproclink:       /* FALLTHROUGH */
        return VLNK;
    }

    // Unknown type: make it a file.
    return VREG;
}

/*
 * Given a vnode that corresponds to a pfsnode_t, returns the corresponding
 * process id and proc_t reference. If the node does not have a corresponding
 * process (i.e. it is the file system root node), the returned pid is 
 * PRNODE_NO_PID and the proc_t reference is NULL. If the process no longer
 * exists, returns ENOENT.
 */
int
procfs_get_process_info(vnode_t vp, pid_t *pidp, proc_t *procp)
{
    pfsnode_t *pfsnode = VTOPFS(vp);
    pfssnode_t *snode = pfsnode->node_structure_node;
    pfstype node_type = snode->psn_node_type;

    int pid = procfsnode_to_pid(pfsnode);
    proc_t p = pid == PRNODE_NO_PID ? NULL : proc_find(pid); // Process for the vnode, if there is one.
    if (p == NULL && procfs_node_type_has_pid(node_type)) {
        // Process must have gone -- return an error
        return ENOENT;
    }

    *procp = p;
    *pidp = pid;
    return 0;
}

/*
 * Returns whether a node of a given type must have an
 * associated process id.
 */
boolean_t
procfs_node_type_has_pid(pfstype node_type)
{
    return node_type != PFSroot && node_type != PFScurproc
        && node_type != PFSdir && node_type != PFScpuinfo
        && node_type != PFSversion && node_type != PFSmeminfo
        && node_type != PFSmtab && node_type != PFSstat
        && node_type != PFSvmstat && node_type != PFSuptime
        && node_type != PFSswaps && node_type != PFSfilesystems
        && node_type != PFSsysctl && node_type != PFSextensions
        && node_type != PFSmodules && node_type != PFSdiskstats
        && node_type != PFSdevices && node_type != PFSnetdev
        && node_type != PFSallocinfo && node_type != PFSapm
        && node_type != PFSbootconfig && node_type != PFSbuddyinfo
        && node_type != PFSpcidevices && node_type != PFSdma
        && node_type != PFSrtc && node_type != PFSexecdomains
        && node_type != PFSfb && node_type != PFSnfsexports
        && node_type != PFSinterrupts && node_type != PFSirq
        && node_type != PFStty && node_type != PFSkcmdline;
}

/*
 * Gets the file id for a given node. There is no obvious way to create
 * a unique and reproducible file id for a node that doesn't have any
 * persistent storage, so we synthesize one based on the base node id
 * from the file system structure, the owning process id if there is one
 * and the owning object id (which is a thread or a file descriptor). 
 * This may not be unique because we can only include part of the object id. 
 * It should, however, be good enough.
 */
uint64_t
procfs_get_node_fileid(pfsnode_t *pnp)
{
    pfsid_t node_id = pnp->node_id;
    return procfs_get_fileid(node_id.nodeid_pid, node_id.nodeid_objectid, pnp->node_structure_node->psn_base_node_id);
}

/*
 * Constructs a file id for a given process id, object id and structure node
 * base id. This may not be unique because we can only include part of the object
 * id. It should, however, be good enough.
 */
uint64_t
procfs_get_fileid(int pid, uint64_t objectid, pfsbaseid_t base_id)
{
    uint64_t id = base_id;
    if (pid != PRNODE_NO_PID) {
        id |= pid << 8;
    }
    id |= objectid << 24;
    return id;
}

/*
 * Attempts to convert a string to a positive integer. Returns
 * the value, or -1 if the string does not start with an integer
 * value. *end_ptr is set to point to the first non-numeric 
 * character encountered.
 */
int
procfs_atoi(const char *p, const char **end_ptr)
{
    int value = 0;
    const char *next = p;
    char c;

    while ((c = *next++) != (char)0 && c >= '0' && c <= '9') {
        value = value * 10 + c - '0';
    }
    *end_ptr = next - 1;

    // Invalid if the first character was not a digit.
    return next == p + 1 ? -1 : value;
}

/*
 * Function used to iterate the process list to collect
 * process ids. If the procfs_pidlist_data structure has
 * credentials, the process id id added only if it should
 * be accessible to an entity with those credentials.
 */
STATIC int
procfs_get_pid(proc_t p, void *udata)
{
    struct procfs_pidlist_data *data = udata;
    kauth_cred_t creds = data->creds;

    if (creds == NULL || procfs_check_can_access_process(creds, p) == 0) {
        if (data->num_procs >= data->max_procs) {
            // Workaround race-condition between read of nprocs and the kernel
            // spawning more processes.
            return PROC_RETURNED;
        }

        data->pids[data->num_procs] = proc_pid(p);
        data->num_procs++;
    }

    return PROC_RETURNED;
}

/*
 * Gets a list of all of the running processes in the system that
 * can be seen by a process with given credentials. If the creds
 * argument is NULL, no access check is made and the process ids
 * of all active processes are returned.
 * This function allocates memory for the list of pids and
 * returns it in the location pointed to by pidpp and the
 * number of valid entries in *pid_count. The total size of the
 * allocated memory is returned in *sizep. The caller must
 * call procfs_release_pids() to free the memory, passing in
 * the values that it received from this function. 
 */
void
procfs_get_pids(pid_t **pidpp, int *pid_count, uint32_t *sizep, kauth_cred_t creds)
{
    /*
     * Enumerate processes using proc_find() over the PID space.
     * proc_find() is public KPI; we must call proc_rele() after each use.
     * macOS PIDs range from 0 to 99999.
     */
#define PROCFS_MAX_PID   99999
#define PROCFS_MAX_PROCS 1024

    uint32_t pidsize = PROCFS_MAX_PROCS * sizeof(pid_t);
    pid_t *pidp = OSMalloc(pidsize, procfs_osmalloc_tag);
    if (pidp == NULL) {
        *pidpp = NULL;
        *sizep = 0;
        *pid_count = 0;
        return;
    }

    int count = 0;
    for (pid_t pid = 0; pid <= PROCFS_MAX_PID && count < PROCFS_MAX_PROCS; pid++) {
        proc_t p = proc_find(pid);
        if (p != PROC_NULL) {
            if (creds == NULL || procfs_check_can_access_process(creds, p) == 0) {
                pidp[count++] = pid;
            }
            proc_rele(p);
        }
    }

    *pidpp = pidp;
    *sizep = pidsize;
    *pid_count = count;
}

/*
 * Frees a list of process id obtained from an earlier
 * invocation of procfs_get_pids().
 */
void
procfs_release_pids(pid_t *pidp, uint32_t size)
{
    OSFree(pidp, size, procfs_osmalloc_tag);
}

int
procfs_issuser(kauth_cred_t creds)
{
    if (kauth_cred_getuid(creds) == 0) {
        return 0;
    }
    return EPERM;
}

/*
 * Gets the number of active processes that are visible to a
 * process with given credentials.
 */
int
procfs_get_process_count(kauth_cred_t creds)
{
    pid_t *pidp;
    int process_count;
    uint32_t size;

    boolean_t is_suser = procfs_issuser(creds) == 0;

    procfs_get_pids(&pidp, &process_count, &size, is_suser ? NULL : creds);
    procfs_release_pids(pidp, size);
    if (pidp != NULL) {
        pidp = NULL;
    }
    
    return process_count;
}

/*
 * Thread enumeration.
 *
 * The Mach thread KPIs (task_threads / thread_info / convert_port_to_thread)
 * are neither linkable (com.apple.kpi.private) nor present in the stripped
 * kernel symbol table, and struct task / struct thread are opaque, so we cannot
 * walk task->threads. Instead we enumerate threads through the BSD side, whose
 * layout *is* known at compile time: a proc owns p_uthlist, a TAILQ of struct
 * uthread linked by uu_list (both fields sit before any CONFIG_* conditional,
 * so their offsets are config-independent).
 *
 * A uthread is allocated immediately after its thread, so XNU's get_machthread()
 * is simply (thread_t)((uintptr_t)uth - sizeof(struct thread)). sizeof(struct
 * thread) is opaque, but constant, so we recover it once at runtime: the current
 * thread's uthread is the unique uthread in current_proc()'s list lying just
 * above current_thread(). From there each thread's id is thread_tid(uth - size).
 */
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern pmap_t  kernel_pmap;

static uintptr_t g_thread_struct_size  = 0;     /* sizeof(struct thread)        */
static boolean_t g_thread_size_known   = FALSE;

#define PROCFS_THREAD_SIZE_MAX 262144 /* sane upper bound on sizeof(struct thread) */
#define PROCFS_MAX_THREADS     100000
#define PROCFS_KPTR_MIN        ((uintptr_t)0xfffffe0000000000ULL)

/*
 * True iff va is a plausible, currently-mapped ARM64 kernel address. Used to
 * gate every dereference of a list pointer so a torn-down uthread can never
 * fault the walk.
 */
static boolean_t
procfs_kptr_ok(uintptr_t va)
{
    if (va < PROCFS_KPTR_MIN) {
        return FALSE;
    }
    return pmap_find_phys(kernel_pmap, (addr64_t)va) != 0;
}

/*
 * Recover sizeof(struct thread) once. The current thread's uthread sits exactly
 * sizeof(struct thread) bytes above current_thread() and is a member of
 * current_proc()'s p_uthlist. Other threads' uthreads live in unrelated
 * allocations, so the current thread's uthread is the lowest-addressed uthread
 * in the list above current_thread(); its distance is the struct size.
 */
static void
procfs_thread_size_init(void)
{
    if (g_thread_size_known) {
        return;
    }

    proc_t    cp = current_proc();
    uintptr_t th = (uintptr_t)current_thread();
    if (cp == PROC_NULL || th == 0) {
        return;
    }

    uintptr_t best = 0;
    boolean_t have_best = FALSE;
    uthread_t uth = TAILQ_FIRST(&cp->p_uthlist);
    for (int i = 0; uth != NULL && i < PROCFS_MAX_THREADS; i++) {
        uintptr_t u = (uintptr_t)uth;
        if (!procfs_kptr_ok(u)) {
            break;
        }
        if (u > th && (u - th) < PROCFS_THREAD_SIZE_MAX) {
            if (!have_best || u < best) {
                best = u;
                have_best = TRUE;
            }
        }
        uth = TAILQ_NEXT(uth, uu_list);
    }

    if (have_best) {
        g_thread_struct_size = best - th;
        g_thread_size_known  = TRUE;
    }
}

/*
 * Gets a list of the thread ids for the threads belonging to a process. The
 * array is allocated here and must be freed with procfs_release_thread_ids().
 *
 * p_uthlist is walked without proc_lock (not linkable here); every uthread is
 * pmap-validated before dereference and the walk is iteration-capped, so a
 * thread torn down mid-walk yields a possibly-stale id rather than a fault.
 */
int
procfs_get_thread_ids_for_task(proc_t p, uint64_t **thread_ids, int *thread_count)
{
    *thread_ids = NULL;
    *thread_count = 0;

    procfs_thread_size_init();
    if (!g_thread_size_known || p == PROC_NULL) {
        return KERN_NOT_SUPPORTED;
    }

    /* First pass: count the uthreads. */
    int count = 0;
    uthread_t uth = TAILQ_FIRST(&p->p_uthlist);
    while (uth != NULL && count < PROCFS_MAX_THREADS) {
        if (!procfs_kptr_ok((uintptr_t)uth)) {
            break;
        }
        count++;
        uth = TAILQ_NEXT(uth, uu_list);
    }
    if (count == 0) {
        return KERN_SUCCESS;
    }

    uint64_t *ids = (uint64_t *)OSMalloc((uint32_t)(count * sizeof(uint64_t)), procfs_osmalloc_tag);
    if (ids == NULL) {
        return KERN_RESOURCE_SHORTAGE;
    }

    /* Second pass: collect the thread ids (uthread -> thread -> thread_id). */
    int n = 0;
    uth = TAILQ_FIRST(&p->p_uthlist);
    while (uth != NULL && n < count) {
        uintptr_t u = (uintptr_t)uth;
        if (!procfs_kptr_ok(u)) {
            break;
        }
        thread_t thread = (thread_t)(u - g_thread_struct_size);
        if (procfs_kptr_ok((uintptr_t)thread)) {
            ids[n++] = thread_tid(thread);
        }
        uth = TAILQ_NEXT(uth, uu_list);
    }

    *thread_ids = ids;
    *thread_count = n;
    return KERN_SUCCESS;
}

/*
 * Releases the memory allocated by an earlier successful call to
 * the get_thread_ids_for_task() function.
 */
void
procfs_release_thread_ids(uint64_t *thread_ids, int thread_count)
{
    OSFree(thread_ids, thread_count * sizeof(uint64_t), procfs_osmalloc_tag);
}

/*
 * Get the number of threads for a given process.
 */
int
procfs_get_task_thread_count(proc_t p)
{
    int thread_count = 0;
    uint64_t *thread_ids;

    if (procfs_get_thread_ids_for_task(p, &thread_ids, &thread_count) == 0) {
        procfs_release_thread_ids(thread_ids, thread_count);
    }

    return thread_count;
}

/*
 * Returns the open file descriptors of a process using proc_fdlist(), the
 * exported KPI that locks the fd table internally (the proc_fdlock primitives
 * are not available to kexts on this kernel). On success *fdlist points to a
 * malloc'd array of *count proc_fdinfo entries that the caller must release
 * with procfs_release_fd_list(); otherwise *fdlist is NULL, *count is 0 and a
 * non-zero errno is returned.
 */
int
procfs_get_fd_list(proc_t p, struct proc_fdinfo **fdlist, size_t *count)
{
    *fdlist = NULL;
    *count = 0;

    if (p == PROC_NULL) {
        return EINVAL;
    }

    // Prefer the procfsd daemon (proc_pidinfo PROC_PIDLISTFDS): the in-kernel
    // fd-table walk depends on struct filedesc offsets that drift across kernel
    // point-releases. One payload holds up to PROCFS_CTL_MAXPAYLOAD/8 fds; a
    // process with more has its list truncated (rare for interactive use).
    struct proc_fdinfo *dbuf = malloc(PROCFS_CTL_MAXPAYLOAD, M_TEMP, M_WAITOK);
    if (dbuf != NULL) {
        uint32_t got = 0;
        if (procfs_ctl_request(PROCFS_REQ_FDLIST, proc_pid(p), 0,
                dbuf, PROCFS_CTL_MAXPAYLOAD, &got) == 0) {
            *fdlist = dbuf;
            *count  = got / sizeof(struct proc_fdinfo);
            return 0;
        }
        free(dbuf, M_TEMP);     /* no daemon -> fall back to the in-kernel walk */
    }

    // The first call (NULL buffer) returns an upper bound on the fd count.
    size_t capacity = 0;
    int error = proc_fdlist(p, NULL, &capacity);
    if (error != 0 || capacity == 0) {
        return error;
    }

    struct proc_fdinfo *buf = malloc(capacity * sizeof(struct proc_fdinfo), M_TEMP, M_WAITOK);
    if (buf == NULL) {
        return ENOMEM;
    }

    // The second call fills the buffer and reports the actual number written.
    size_t actual = capacity;
    error = proc_fdlist(p, buf, &actual);
    if (error != 0) {
        free(buf, M_TEMP);
        return error;
    }

    *fdlist = buf;
    *count = actual;
    return 0;
}

/*
 * Releases the file-descriptor list allocated by procfs_get_fd_list().
 */
void
procfs_release_fd_list(struct proc_fdinfo *fdlist)
{
    if (fdlist != NULL) {
        free(fdlist, M_TEMP);
    }
}

/*
 * Determines whether an entity with given credentials can
 * access a given process. The determination is based on the 
 * real and effective user/group ids of the process. Returns 
 * 0 if access is allowed and EACCES otherwise.
 */
int
procfs_check_can_access_process(kauth_cred_t creds, proc_t p)
{
    posix_cred_t posix_creds = &creds->cr_posix;
    kauth_cred_t proc_cred = kauth_cred_proc_ref(p);
    int error = EACCES;

    // Allow access if the effective user id matches the
    // effective or real user id of the process.
    uid_t cred_euid = posix_creds->cr_uid;
    if (cred_euid == kauth_cred_getuid(proc_cred) || cred_euid == kauth_cred_getruid(proc_cred)) {
        error = 0;
    }

    // Also allow access if the effective group id matches
    // the effective or saved group id of the process.
    gid_t cred_egid = posix_creds->cr_groups[0];
    if (error != 0 && (cred_egid == kauth_cred_getgid(proc_cred) || cred_egid == kauth_cred_getrgid(proc_cred))) {
        error = 0;
    }

    kauth_cred_unref(&proc_cred);

    return error;
}

/*
 * Determines whether an entity with given credentials can
 * access the process with a given process id. The determination
 * is based on the real and effective user/group ids of the 
 * process. Returns 0 if access is allowed, ESRCH if there is
 * no process with the given pid and EACCES otherwise.
 */
int
procfs_check_can_access_proc_pid(kauth_cred_t creds, pid_t pid)
{
    int error = 0;

    proc_t p = proc_find(pid);
    if (p != PROC_NULL) {
        error = procfs_check_can_access_process(creds, p);
        proc_rele(p);
    } else {
        error = ESRCH;
    }

    return error;
}

/*
 * Copies data from the local buffer "data" into the area described
 * by a uio structure. The first byte of "data" is assumed to 
 * correspond to a zero offset, so if the uio structure has its
 * uio_offset set to N, the first byte of data that will be copied
 * is at data[N].
 */
int
procfs_copy_data(const char *data, int data_len, uio_t uio)
{
    int error = 0;

    off_t start_offset = uio_offset(uio);
    data_len -= start_offset;

    if ((data_len >= 0) && (data != NULL)) {
        error = uiomove(data + start_offset, data_len, uio);
    } else if ((data_len >= 0) && (data == NULL)) {
        error = EFAULT;
    } else if ((data_len <= 0) && (data != NULL)) {
        error = EIO;
    } else {
        error = ENOTSUP;
    }

    return error;
}
