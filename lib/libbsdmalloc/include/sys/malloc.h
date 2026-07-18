/*-
 * SPDX-License-Identifier: BSD-3-Clause AND MIT
 *
 * The malloc()/free() declarations and the M_ZERO zero-in-place malloc() macro
 * below are derived from FreeBSD's sys/sys/malloc.h:
 *
 * Copyright (c) 1987, 1993
 *	The Regents of the University of California.
 * Copyright (c) 2005, 2009 Robert N. M. Watson
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * The remaining code in this file - the #include_next wrapper arrangement, the
 * _MALLOC()/_FREE() re-declarations and the XNU compatibility guards - is
 * Copyright (c) 2022-2026 Sunneva N. Mariu and is licensed under the MIT
 * License. See the LICENSE file in this directory for both licenses in full.
 */

/*
 * libbsdmalloc - BSD-style _MALLOC()/_FREE() for third-party kexts.
 *
 * The kernel's <sys/malloc.h> only prototypes _MALLOC()/_FREE() in its
 * !XNU_KERNEL_PRIVATE branch (they are deprecated in favour of kalloc_type()),
 * so they are invisible in a kernel-private build. The symbols are still
 * exported BSD KPI, however, so this wrapper pulls in the real <sys/malloc.h>
 * behind it (via #include_next) and re-declares them, along with the classic
 * malloc()/free() spelling.
 *
 * Because this header is itself named <sys/malloc.h>, it must be reached first
 * on the include path; #include_next then continues on to the kernel header.
 */
#ifndef LIBBSDMALLOC_SYS_MALLOC_H
#define LIBBSDMALLOC_SYS_MALLOC_H

#include <string.h>
#include <sys/cdefs.h>
#include <sys/sysctl.h>

#include_next <sys/malloc.h>

#ifndef __malloc_like
#define __malloc_like __attribute__((__malloc__))
#endif

/*
 * FreeBSD spells the branch hints __predict_true()/__predict_false(); XNU's
 * <sys/cdefs.h> calls them __probable()/__improbable() and does not define the
 * FreeBSD names. Supply them so the malloc() macro below builds on either.
 */
#ifndef __predict_true
#define __predict_true(exp)  __builtin_expect(!!(exp), 1)
#endif
#ifndef __predict_false
#define __predict_false(exp) __builtin_expect(!!(exp), 0)
#endif

extern void *_MALLOC(size_t size, int type, int flags);
extern void  _FREE(void *addr, int type);

void 	free(void *addr, int type);
void	*malloc(size_t size, int type, int flags) __malloc_like
	    __result_use_check __alloc_size(1);

/*
 * Try to optimize malloc(..., ..., M_ZERO) allocations by doing zeroing in
 * place if the size is known at compilation time.
 *
 * Passing the flag down requires malloc to blindly zero the entire object.
 * In practice a lot of the zeroing can be avoided if most of the object
 * gets explicitly initialized after the allocation. Letting the compiler
 * zero in place gives it the opportunity to take advantage of this state.
 *
 * Note that the operation is only applicable if both flags and size are
 * known at compilation time. If M_ZERO is passed but M_WAITOK is not, the
 * allocation can fail and a NULL check is needed. However, if M_WAITOK is
 * passed we know the allocation must succeed and the check can be elided.
 *
 *	_malloc_item = malloc(_size, type, (flags) &~ M_ZERO);
 *	if (((flags) & M_WAITOK) != 0 || _malloc_item != NULL)
 *		bzero(_malloc_item, _size);
 *
 * If the flag is set, the compiler knows the left side is always true,
 * therefore the entire statement is true and the callsite is:
 *
 *	_malloc_item = malloc(_size, type, (flags) &~ M_ZERO);
 *	bzero(_malloc_item, _size);
 *
 * If the flag is not set, the compiler knows the left size is always false
 * and the NULL check is needed, therefore the callsite is:
 *
 * 	_malloc_item = malloc(_size, type, (flags) &~ M_ZERO);
 *	if (_malloc_item != NULL)
 *		bzero(_malloc_item, _size);			
 *
 * The implementation is a macro because of what appears to be a clang 6 bug:
 * an inline function variant ended up being compiled to a mere malloc call
 * regardless of argument. gcc generates expected code (like the above).
 */
#define	malloc(size, type, flags) ({					\
	void *_malloc_item;						\
	size_t _size = (size);						\
	if (__builtin_constant_p(size) && __builtin_constant_p(flags) &&\
	    ((flags) & M_ZERO) != 0) {					\
		_malloc_item = malloc(_size, type, (flags) &~ M_ZERO);	\
		if (((flags) & M_WAITOK) != 0 ||			\
		    __predict_true(_malloc_item != NULL))		\
			memset(_malloc_item, 0, _size);			\
	} else {							\
		_malloc_item = malloc(_size, type, flags);		\
	}								\
	_malloc_item;							\
})

#endif /* LIBBSDMALLOC_SYS_MALLOC_H */
