/*
 * Copyright (c) 2000-2020 Apple Inc. All rights reserved.
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
 * xnu/bsd/kern/kern_proc.c: only the routines a third-party kernel extension needs are
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

