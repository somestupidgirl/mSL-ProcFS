/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_limit.c
 *
 * Based on FreeBSD's procfs_rlimit.c
 *
 * Process resource-limit node (/proc/<pid>/limit).
 *
 */
/*-
 * SPDX-License-Identifier: BSD-4-Clause
 *
 * Copyright (c) 1999 Adrian Chadd
 * Copyright (c) 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Jan-Simon Pendry.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
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
 */
/*
 * To get resource.h to include our rlimit_ident[] array of rlimit identifiers
 */
#include <mach/vm_types.h>

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <sys/resourcevar.h>
#include <sys/resource.h>
#include <sys/sbuf.h>
#include <sys/types.h>
#include <sys/vnode.h>

#include <fs/procfs/procfs.h>

/*
 * p_limit is an SMR-protected pointer; validate the raw pointer before deref.
 * pmap_find_phys + kernel_pmap are com.apple.kpi.unsupported (linkable).
 */
extern ppnum_t pmap_find_phys(pmap_t pmap, addr64_t va);
extern pmap_t kernel_pmap;

#define PROCFS_KPTR_MIN ((uintptr_t)0xfffffe0000000000ULL)

/*
 * Resource-limit string identifiers, indexed by RLIMIT_*. macOS defines nine
 * limits (RLIM_NLIMITS), which are the first nine of FreeBSD's list; the extra
 * FreeBSD limits (sbsize, vmem, npts, ...) do not exist here.
 */
static const char *const rlimit_ident[RLIM_NLIMITS] = {
	"cpu",      /* RLIMIT_CPU     */
	"fsize",    /* RLIMIT_FSIZE   */
	"data",     /* RLIMIT_DATA    */
	"stack",    /* RLIMIT_STACK   */
	"core",     /* RLIMIT_CORE    */
	"rss",      /* RLIMIT_AS (a.k.a. RLIMIT_RSS) */
	"memlock",  /* RLIMIT_MEMLOCK */
	"nproc",    /* RLIMIT_NPROC   */
	"nofile",   /* RLIMIT_NOFILE  */
};

/*
 * Reads the data for the "limit" node: one "<name> <cur> <max>" line per
 * resource limit, with -1 for RLIM_INFINITY (FreeBSD's procfs_rlimit format).
 */
int
procfs_dolimit(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
	int error = 0;

	proc_t p = proc_find(pnp->node_id.nodeid_pid);
	if (p == PROC_NULL) {
		return ESRCH;
	}

	/*
	 * p_limit is SMR-protected and the safe accessor proc_limitget() is stripped
	 * from the arm64 kernel, so read the pointer raw (smr_unsafe_load is a plain
	 * macro) and pmap-validate it before dereferencing. Resource limits change
	 * very rarely, so this racy snapshot is acceptable; a torn-down/garbage
	 * pointer is rejected rather than faulted.
	 */
	struct plimit *limp = (struct plimit *)smr_unsafe_load(&p->p_limit);
	if ((uintptr_t)limp < PROCFS_KPTR_MIN ||
	    pmap_find_phys(kernel_pmap, (addr64_t)(uintptr_t)limp) == 0) {

		proc_rele(p);
		return EIO;
	}

	/* Snapshot the limits, then drop the proc reference. */
	struct rlimit rl[RLIM_NLIMITS];
	for (int i = 0; i < RLIM_NLIMITS; i++) {
		rl[i] = limp->pl_rlimit[i];
	}
	proc_rele(p);

	char buf[1024];
	struct sbuf sb;
	if (sbuf_new(&sb, buf, sizeof(buf), SBUF_FIXEDLEN) == NULL) {
		return ENOMEM;
	}

	for (int i = 0; i < RLIM_NLIMITS; i++) {
		sbuf_printf(&sb, "%s ", rlimit_ident[i]);
		if (rl[i].rlim_cur == RLIM_INFINITY) {
			sbuf_printf(&sb, "-1 ");
		} else {
			sbuf_printf(&sb, "%llu ", (unsigned long long)rl[i].rlim_cur);
		}
		if (rl[i].rlim_max == RLIM_INFINITY) {
			sbuf_printf(&sb, "-1\n");
		} else {
			sbuf_printf(&sb, "%llu\n", (unsigned long long)rl[i].rlim_max);
		}
	}
	sbuf_finish(&sb);

	error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
	sbuf_delete(&sb);

	return error;
}