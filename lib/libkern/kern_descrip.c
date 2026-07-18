/*
 * Copyright (c) 2000-2016 Apple Inc. All rights reserved.
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
 * xnu/bsd/kern/kern_descrip.c: only the routines a third-party kernel extension needs are
 * retained, and private KPIs that such a kext cannot link have been
 * re-implemented in terms of public ones. See README.md.
 */






#include <sys/file_internal.h>


/*
 * The running arm64 kernel builds struct lockf with IMPORTANCE_INHERITANCE, so
 * define it before <sys/lockf.h>. (The extra lf_boosted int lands in off_t's
 * alignment padding, so the fields we read are at the same offsets either way,
 * but we match the kernel's definition to be safe.)
 */
#ifndef IMPORTANCE_INHERITANCE
#define IMPORTANCE_INHERITANCE 1
#endif


#include <kern.h>

/*
 * fg_get_data_volatile() - re-implementation of the private fg_get_data_volatile().
 * fg_data is a manually PAC-signed pointer (the struct field is a bare uintptr_t,
 * so the compiler does not auto-authenticate it like XNU_PTRAUTH_SIGNED_PTR
 * fields). Reading it raw yields a signed pointer that faults when dereferenced,
 * so authenticate it exactly as XNU does before use.
 */
void *
fg_get_data_volatile(struct fileglob *fg)
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
