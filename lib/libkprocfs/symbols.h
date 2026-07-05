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
#include <sys/proc_internal.h>
#include <sys/sysctl.h>

#pragma mark -
#pragma mark CPU

#if defined(__x86_64__)
/*
 * x86-only NULL stubs (never resolved). The x86 /proc/cpuinfo path still
 * references these; they are the last symbols.h consumers and are migrated to
 * the procfsd daemon next, after which this header is removed.
 *
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
 * Set at load if libklookup validates against the staged kernel-symbol file (see
 * lib/libkprocfs/symbols.c, tools/procfs_ksyms.c). No callable private symbols
 * remain resolved through it - every former consumer now goes through the
 * procfsd daemon - so this is vestigial and removed with the staging pipeline.
 */
extern boolean_t                procfs_klookup_ok;

#endif /* _symbols_h */
