/*
 * Copyright (c) 2000-2021 Apple Inc. All rights reserved.
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
 * kern.h - public interface of libkern.
 *
 * Declarations for the XNU routines forward-ported by this library. Each
 * declaration mirrors its counterpart in the XNU header that originally
 * exported it (bsd/sys/proc_internal.h, bsd/sys/file_internal.h,
 * bsd/sys/proc_info.h); the implementations live in the correspondingly
 * named source files here.
 *
 * This header assumes the usual kernel headers (sys/proc.h, sys/vnode.h,
 * sys/proc_info.h, sys/file_internal.h, ...) have already been included by
 * the consumer, exactly as the XNU internal headers do.
 */
#ifndef _libkern_kern_h
#define _libkern_kern_h

/*
 * =========== From bsd/kern/proc_info.c ===========
 */
extern int proc_pidshortbsdinfo(proc_t p, struct proc_bsdshortinfo * pbsd_shortp, int zombie);

/*
 * proc_fdlock()/proc_fdunlock()/proc_fdlist() - re-implementations of the
 * private descriptor-table routines. proc_fdlist() mirrors XNU's
 * proc_fdlist_internal(): with buf == NULL it returns an upper bound on the
 * descriptor count, otherwise it fills up to *count entries and updates *count.
 */
extern void proc_fdlock(struct proc *p);
extern void proc_fdunlock(struct proc *p);
extern int  proc_fdlist(proc_t p, struct proc_fdinfo *buf, size_t *count);

#endif /* _libkern_kern_h */
