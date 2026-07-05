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
#include <sys/proc_info.h>
#include <sys/proc_internal.h>
#include <sys/sysctl.h>
#include <sys/vnode.h>

/*
 * Private-KPI function pointers, all left NULL (never resolved) on this build:
 * kernel memory scanning is not possible under ARM64 PAC enforcement, so the
 * call sites below are guarded by `_sym != NULL` and fall back to a public-KPI
 * path (or a zeroed result). What can be reached is resolved instead from the
 * on-disk kernelcache via libklookup - see the procfs_kl_* block at the bottom.
 *
 * These declarations remain only because a handful of call sites still
 * reference them behind those NULL guards; they are being phased out stage by
 * stage in favour of forward-ported functions and the procfsd daemon.
 */

#pragma mark -
#pragma mark Process misc functions.

/*
 * Is the passed-in process tainted by uid or gid changes system call?
 * Returns 1 if tainted, 0 if not tainted.
 */
extern int                      (*_proc_issetugid)(proc_t p);
#define                         proc_issetugid(p) \
                                _proc_issetugid(p)

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

/*
 * Set at load if libklookup validates against the staged kernel-symbol file (see
 * lib/libkprocfs/symbols.c, tools/procfs_ksyms.c). No callable private symbols
 * remain resolved through it - every former consumer now goes through the
 * procfsd daemon - so this is vestigial and removed with the staging pipeline.
 */
extern boolean_t                procfs_klookup_ok;

#endif /* _symbols_h */
