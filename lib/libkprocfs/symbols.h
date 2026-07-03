/*
 * symbols.h
 *
 * Declarations for the symbol resolver and related stuff.
 *
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 */
#ifndef _symbols_h
#define _symbols_h

#if defined(__x86_64__)
#include <i386/cpuid.h>
#endif
#include <sys/bsdtask_info.h>
#include <sys/file_internal.h>
#include <sys/filedesc.h>
#include <sys/proc_info.h>
#include <sys/proc_internal.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>

struct tty;
#define TTY_NULL (struct tty *)NULL

#pragma mark -
#pragma mark Process misc functions.

/*
 * Returns the 32-byte name if it exists, otherwise returns the 16-byte name
 */
extern char *                   (*_proc_best_name)(proc_t p);
#define                         proc_best_name(p) \
                                _proc_best_name(p)
/*
 * Set process start time. Returns 0 on success.
 */
extern int                      (*_proc_starttime)(proc_t p, struct timeval *tv);
#define                         proc_starttime(p, tv)   \
                                _proc_starttime(p, tv)
/*
 * Is the passed-in process tainted by uid or gid changes system call?
 * Returns 1 if tainted, 0 if not tainted.
 */
extern int                      (*_proc_issetugid)(proc_t p);
#define                         proc_issetugid(p) \
                                _proc_issetugid(p)

extern void                     (*_psignal)(proc_t p, int signum);
#define                         psignal(p, signum) \
                                _psignal(p, signum)

extern int                      (*_tsleep)(void *chan, int pri, const char *wmesg, int timo);
#define                         tsleep(chan, pri, wmesg, timo) \
                                _tsleep(chan, pri, wmesg, timo)
/*
 * Mutex lock for process.
 */
extern void                     (*_proc_lock)(proc_t p);
#define                         proc_lock(p) \
                                _proc_lock(p)
/*
 * Mutex unlock for process.
 */
extern void                     (*_proc_unlock)(proc_t p);
#define                         proc_unlock(p) \
                                _proc_unlock(p)
/*
 * Mutex lock for process list.
 */
extern void                     (*_proc_list_lock)(void);
#define                         proc_list_lock() \
                                _proc_list_lock()
/*
 * Mutex unlock for process list.
 */
extern void                     (*_proc_list_unlock)(void);
#define                         proc_list_unlock() \
                                _proc_list_unlock()
/*
 * Process iteration.
 */
extern void                     (*_proc_iterate)(unsigned int flags, proc_iterate_fn_t callout, void *arg, proc_iterate_fn_t filterfn, void *filterarg);
#define                         proc_iterate(flags, co, arg, ff, fa) \
                                _proc_iterate(flags, co, arg, ff, fa)

#pragma mark -
#pragma mark Process Group

/*
 * Get process group by passed-in process.
 */
extern struct pgrp *            (*_proc_pgrp)(proc_t);
#define                         proc_pgrp(p) \
                                _proc_pgrp(p)
/*
 * Get process group by passed-in PID.
 */
extern struct pgrp *            (*_pgfind)(pid_t);
#define                         pgfind(pid) \
                                _pgfind(pid)
/*
 * Release process group.
 */
extern void                     (*_pg_rele)(struct pgrp * pgrp);
#define                         pg_rele(pgrp) \
                                _pg_rele(pgrp)
/*
 * Process group mutex lock.
 */
extern void                     (*_pgrp_lock)(struct pgrp * pgrp);
#define                         pgrp_lock(pgrp) \
                                _pgrp_lock(pgrp)
/*
 * Process group mutex unlock.
 */
extern void                     (*_pgrp_unlock)(struct pgrp * pgrp);
#define                         pgrp_unlock(pgrp) \
                                _pgrp_unlock(pgrp)
/*
 * Process group iteration.
 */
extern void                     (*_pgrp_iterate)(struct pgrp *pgrp, unsigned int flags, proc_iterate_fn_t callout, void *arg, proc_iterate_fn_t filterfn, void *filterarg);
#define                         pgrp_iterate(pgrp, flags, co, arg, ff, fa) \
                                _pgrp_iterate(pgrp)

#pragma mark -
#pragma mark TTY

/*
 * Mutex lock for TTY.
 */
extern void                     (*_tty_lock)(struct tty *tp);
#define                         tty_lock(tp) \
                                _tty_lock(tp)
/*
 * Mutex unlock for TTY.
 */
extern void                     (*_tty_unlock)(struct tty *tp);
#define                         tty_unlock(tp) \
                                _tty_unlock(tp)

#pragma mark -
#pragma mark File descriptor

extern struct filedesc          (*_filedesc0);
#define                         filedesc0 \
                                *_filedesc0;

/*
 * fdcopy
 *
 * Description: Copy a filedesc structure.  This is normally used as part of
 *      forkproc() when forking a new process, to copy the per process
 *      open file table over to the new process.
 *
 * Parameters:  p               Process whose open file table
 *                      is to be copied (parent)
 *      uth_cdir            Per thread current working
 *                      cirectory, or NULL
 *
 * Returns: NULL                Copy failed
 *      !NULL               Pointer to new struct filedesc
 *
 * Locks:   This function internally takes and drops proc_fdlock()
 *
 * Notes:   Files are copied directly, ignoring the new resource limits
 *      for the process that's being copied into.  Since the descriptor
 *      references are just additional references, this does not count
 *      against the number of open files on the system.
 *
 *      The struct filedesc includes the current working directory,
 *      and the current root directory, if the process is chroot'ed.
 *
 *      If the exec was called by a thread using a per thread current
 *      working directory, we inherit the working directory from the
 *      thread making the call, rather than from the process.
 *
 *      In the case of a failure to obtain a reference, for most cases,
 *      the file entry will be silently dropped.  There's an exception
 *      for the case of a chroot dir, since a failure to to obtain a
 *      reference there would constitute an "escape" from the chroot
 *      environment, which must not be allowed.  In that case, we will
 *      deny the execve() operation, rather than allowing the escape.
 */
extern struct filedesc *        (*_fdcopy)(proc_t p, vnode_t uth_cdir);
#define                         fdcopy(p, uth_cdir) \
                                _fdcopy(p, uth_cdir)
/*
 * fdfree
 *
 * Description: Release a filedesc (per process open file table) structure;
 *      this is done on process exit(), or from forkproc_free() if
 *      the fork fails for some reason subsequent to a successful
 *      call to fdcopy()
 *
 * Parameters:  p               Pointer to process going away
 *
 * Returns: void
 *
 * Locks:   This function internally takes and drops proc_fdlock()
 */
extern void                     (*_fdfree)(proc_t p);
#define                         fdfree(p) \
                                _fdfree(p)
/*
 * forkproc
 *
 * Description: Create a new process structure, given a parent process
 *      structure.
 *
 * Parameters:  parent_proc     The parent process
 *
 * Returns: !NULL           The new process structure
 *      NULL            Error (insufficient free memory)
 *
 * Note:    When successful, the newly created process structure is
 *      partially initialized; if a caller needs to deconstruct the
 *      returned structure, they must call forkproc_free() to do so.
 */
extern proc_t                   (*_forkproc)(proc_t parent_proc);
#define                         forkproc(parent_proc) \
                                _forkproc(parent_proc)
/*
 * Destroy a process structure that resulted from a call to forkproc(), but
 * which must be returned to the system because of a subsequent failure
 * preventing it from becoming active.
 *
 * Parameters:  p           The incomplete process from forkproc()
 *
 * Returns: (void)
 *
 * Note:    This function should only be used in an error handler following
 *      a call to forkproc().
 *
 *      Operations occur in reverse order of those in forkproc().
 */
extern void                     (*_forkproc_free)(proc_t);
#define                         forkproc_free(p) \
                                _forkproc_free(p)

#pragma mark -
#pragma mark KPI functions from libproc.

/*
 * Update the darwin background action state in the flags field for libproc.
 */
extern int                      (*_proc_get_darwinbgstate)(task_t task, uint32_t * flagsp);
#define                         proc_get_darwinbgstate(task, flagsp) \
                                _proc_get_darwinbgstate(task, flagsp)
/*
 * Fill the proc_taskinfo_internal structure.
 */
extern int                      (*_fill_taskprocinfo)(task_t task, struct proc_taskinfo_internal * ptinfo);
#define                         fill_taskprocinfo(task, ptinfo) \
                                _fill_taskprocinfo(task, ptinfo)
/*
 * Fill the proc_threadinfo_internal structure.
 */
extern int                      (*_fill_taskthreadinfo)(task_t task, uint64_t thaddr, bool thuniqueid, struct proc_threadinfo_internal * ptinfo, void * vpp, int *vidp);
#define                         fill_taskthreadinfo(task, thaddr, thuniqueid, ptinfo, vpp, vidp) \
                                _fill_taskthreadinfo(task, thaddr, thuniqueid, ptinfo, vpp, vidp)
/*
 * Fill the socket_info structure.
 */
extern int                      (*_fill_socketinfo)(socket_t so, struct socket_info *si);
#define                         fill_socketinfo(so, si) \
                                _fill_socketinfo(so, si)

#pragma mark -
#pragma mark Task/Thread

/*
 * Returns the thread for the specified process.
 */
extern thread_t                 (*_proc_thread)(proc_t);
#define                         proc_thread(p) \
                                _proc_thread(p)
/*
 * Get the thread info.
 */
extern kern_return_t            (*_thread_info)(thread_t thread, thread_flavor_t flavor, thread_info_t thread_info, mach_msg_type_number_t *thread_info_count);
#define                         thread_info(thread, flavor, thread_info, thread_info_count) \
                                _thread_info(thread, flavor, thread_info, thread_info_count)
/*
 * Get the BSD thread info. Returns the uthread field from struct thread.
 */
extern void *                   (*_get_bsdthread_info)(thread_t);
#define                         get_bsdthread_info(t) \
                                _get_bsdthread_info(t)

extern void                     (*_bsd_threadcdir)(void * uth, void *vptr, int *vidp);
#define                         bsd_threadcdir(uth, vptr, vidp) \
                                _bsd_threadcdir(uth, vptr, vidp)

extern void *                   (*_uthread_alloc)(task_t task, thread_t thread, int noinherit);
#define                         uthread_alloc(task, thread, noinherit) \
                                _uthread_alloc(task, thread, noinherit)
/*
 * Task threads. Returns KERN_SUCCESS on success.
 */
extern kern_return_t            (*_task_threads)(task_t task, thread_act_array_t *threads_out, mach_msg_type_number_t *count);
#define                         task_threads(task, threads_out, count) \
                                _task_threads(task, threads_out, count)
/*
 * Convert port to thread.
 */
extern thread_t                 (*_convert_port_to_thread)(ipc_port_t port);
#define                         convert_port_to_thread(port) \
                                _convert_port_to_thread(port)
/*
 * Returns the Mach thread associated with a vfs_context_t.
 */
extern thread_t                 (*_vfs_context_thread)(vfs_context_t ctx);
#define                         vfs_context_thread(ctx) \
                                _vfs_context_thread(ctx)

#pragma mark -
#pragma mark Mount

/*
 * Global variable that tells if a mount point is dead.
 */
extern struct mount *           (*_dead_mountp);
#define                         dead_mountp \
                                *_dead_mountp

#pragma mark -
#pragma mark Vnode

/*
 * Vnode status.
 */
extern int                      (*_vn_stat)(struct vnode *vp, void * sb, kauth_filesec_t *xsec, int isstat64, int needsrealdev, vfs_context_t ctx);
#define                         vn_stat(vp, sb, xsec, isstat64, needsrealdev, ctx) \
                                _vn_stat(vp, sb, xsec, isstat64, needsrealdev, ctx)
/*
 * Mutex lock for vnode.
 */
extern void                     (*_vnode_lock)(struct vnode *);
#define                         vnode_lock(vp) \
                                _vnode_lock(vp)
/*
 * Mutex unlock for vnode.
 */
extern void                     (*_vnode_unlock)(struct vnode *);
#define                         vnode_unlock(vp) \
                                _vnode_unlock(vp)

#pragma mark -
#pragma mark CPU

#if defined(__x86_64__)
/*
 * Fills the i386_cpu_info structure and returns its pointer.
 */
extern i386_cpu_info_t *        (*_cpuid_info)(void);
#define                         cpuid_info() \
                                _cpuid_info()
/*
 * Global variable that stores the TSC frequency of the current CPU.
 */
extern uint64_t                 (*_tscFreq);
#define                         tscFreq \
                                *_tscFreq
#endif /* __x86_64__ */

/*
 * Global variable that stores the processor count on the current CPU.
 */
extern unsigned int             (*_processor_count);
#define                         processor_count \
                                *_processor_count

extern uint32_t                 (*_avenrun)[3];
#define                         avenrun \
                                *_avenrun

/*
 * Private kernel symbols resolved at load via libklookup from the staged
 * kernel-symbol file (see kext/lib/symbols.c, tools/procfs_ksyms.c). NULL when
 * unavailable - callers must check. procfs_proc_gettty is PAC-signed and
 * directly callable.
 */
extern boolean_t                procfs_klookup_ok;
extern int                      (*procfs_proc_gettty)(proc_t p, vnode_t *vpp);
extern task_t                   (*procfs_kl_proc_task)(proc_t p);
extern void *                   procfs_kl_cpu_to_processor;
extern void *                   procfs_kl_get_task_map;
extern void *                   procfs_kl_mach_vm_region;
extern unsigned int *           procfs_vm_page_wire_count;

#endif /* _symbols_h */