/*	$NetBSD: procfs_note.c,v 1.15 2006/11/16 01:33:38 christos Exp $	*/

/*
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
 *	@(#)procfs_note.c	8.2 (Berkeley) 1/21/94
 */

/*
 * Copyright (c) 1993 Jan-Simon Pendry
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
 *
 *	@(#)procfs_note.c	8.2 (Berkeley) 1/21/94
 */
/*
 * Copyright (c) 2026 Sunneva N. Mariu
 */
#include <sys/errno.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/signal.h>
#include <sys/proc.h>
#include <string.h>

#include <fs/procfs/procfs.h>

/*
 * Reads a user-supplied note from a write uio into buf (NUL-terminated, with
 * trailing newlines stripped). Mirrors NetBSD's vfs_getuserstr(), which XNU
 * does not provide. *buflenp is the buffer capacity on entry and the note
 * length on return.
 */
static int
procfs_getuserstr(uio_t uio, char *buf, int *buflenp)
{
	size_t xlen = (size_t)uio_resid(uio);

	if (xlen > (size_t)*buflenp) {
		return EMSGSIZE;
	}

	int error = uiomove(buf, (int)xlen, uio);
	if (error) {
		return error;
	}

	buf[xlen] = '\0';
	while (xlen > 0 && buf[xlen - 1] == '\n') {
		buf[--xlen] = '\0';
	}

	*buflenp = (int)xlen;
	return 0;
}

/*
 * Map a Plan 9-style note string to a signal number, or -1 if unrecognised.
 * A plain decimal number is taken as the signal number directly; otherwise the
 * common note names are matched (case-sensitive, lower case). This is how notes
 * are "delivered" on macOS, which has no native note primitive: the closest
 * equivalent - and the historical meaning of procfs notes - is a signal.
 */
static int
procfs_note_to_signal(const char *note)
{
	static const struct { const char *name; int sig; } tbl[] = {
		{ "hup",       SIGHUP },
		{ "hangup",    SIGHUP },
		{ "int",       SIGINT },
		{ "interrupt", SIGINT },
		{ "quit",      SIGQUIT },
		{ "kill",      SIGKILL },
		{ "term",      SIGTERM },
		{ "terminate", SIGTERM },
		{ "stop",      SIGSTOP },
		{ "cont",      SIGCONT },
		{ "usr1",      SIGUSR1 },
		{ "usr2",      SIGUSR2 },
	};

	if (note[0] >= '0' && note[0] <= '9') {
		int sig = 0;
		for (const char *p = note; *p != '\0'; p++) {
			if (*p < '0' || *p > '9') {
				return -1;
			}
			sig = sig * 10 + (*p - '0');
			if (sig >= NSIG) {
				return -1;
			}
		}
		return (sig >= 1) ? sig : -1;
	}

	for (size_t i = 0; i < sizeof(tbl) / sizeof(tbl[0]); i++) {
		if (strcmp(note, tbl[i].name) == 0) {
			return tbl[i].sig;
		}
	}
	return -1;
}

/*
 * /proc/<pid>/note
 *
 * Modeled on NetBSD's procfs_donote(). The node is write-only (reads return
 * EINVAL, as on NetBSD). Writing a note delivers it to the target process as a
 * signal (Plan 9 note semantics): a recognised note name or a numeric signal
 * posts that signal via proc_signal(); an unrecognised note returns EINVAL.
 * Permission is enforced by the node's write mode (owner/group, or root under
 * the noprocperms mount option), matching the rest of the filesystem.
 */
int
procfs_donote(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
	char note[PROCFS_NOTELEN + 1];
	int xlen;
	int error;

	if (uio_rw(uio) != UIO_WRITE) {
		return EINVAL;
	}

	xlen = PROCFS_NOTELEN;
	error = procfs_getuserstr(uio, note, &xlen);
	if (error) {
		return error;
	}

	int sig = procfs_note_to_signal(note);
	if (sig < 0) {
		return EINVAL;
	}

	proc_signal(pnp->node_id.nodeid_pid, sig);
	return 0;
}
