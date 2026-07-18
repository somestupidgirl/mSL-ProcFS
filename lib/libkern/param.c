/*
 * Copyright (c) 2000-2019 Apple Inc. All rights reserved.
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
 * xnu/bsd/conf/param.c: only the routines a third-party kernel extension needs are
 * retained, and private KPIs that such a kext cannot link have been
 * re-implemented in terms of public ones. See README.md.
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


#include <kern.h>
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

