/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_cmdline.c
 *
 * Process argument vector for the "cmdline" node. The entry points are named
 * after NetBSD's procfs_cmdline.c (procfs_doprocargs / procfs_doprocargs_helper)
 * for easy cross-reference:
 *   https://github.com/NetBSD/src/blob/trunk/sys/miscfs/procfs/procfs_cmdline.c
 *
 * The argument-fetching mechanism necessarily diverges. NetBSD's helper is an
 * output callback for the kernel's copy_procargs(); macOS has no equivalent
 * callback KPI a kext may link (KERN_PROCARGS2's vm_map_copyin() path is
 * com.apple.kpi.private). Rather than translate the target's user stack through
 * its pmap in-kernel (which needs proc_task and the unsupported pmap
 * primitives), the flattened argument region is fetched from the procfsd daemon,
 * which reads it with the KERN_PROCARGS2 sysctl (PROCFS_REQ_PROCARGS). That is a
 * root sysctl rather than task_for_pid, so - unlike the maps/mem nodes - it also
 * works for SIP/AMFI-protected and hardened targets. Because the implementation
 * differs substantially from NetBSD's, that file's source license header is not
 * reproduced here.
 */
#include <stdint.h>
#include <string.h>
#include <libkern/libkern.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <bsdcompat/sys/malloc.h>

#include <fs/procfs/procfs.h>
#include <fs/procfs/procfs_ctl.h>

/* apple[0] (and the environ/apple boundary) is the first "executable_path="
 * entry in the KERN_PROCARGS2 region (see sysctl_procargsx() in XNU's
 * kern_sysctl.c). */
#define PROCFS_EXEC_KEY     "executable_path="

/* Upper bound on how much of the args region we accumulate from the daemon. This
 * comfortably covers any real command line and environment while bounding the
 * work (and kernel memory) for a pathological one. */
#define PROCFS_CMDLINE_MAX  (256 * 1024)

/*
 * Accumulate the target's flattened KERN_PROCARGS2 argument region from the
 * procfsd daemon into a freshly malloc'd buffer (free with M_TEMP). The daemon
 * returns the region in MAXPAYLOAD-sized slices keyed by byte offset; we grow
 * the buffer and re-request until a short slice marks the end, capping at
 * PROCFS_CMDLINE_MAX. Returns 0 with the buffer and length set, else an errno
 * (ENOTSUP with no daemon; EINVAL/EPERM from the sysctl for zombies/system).
 */
static int
procfs_fetch_procargs(pid_t pid, uint8_t **bufp, size_t *lenp)
{
    size_t   cap = PROCFS_CTL_MAXPAYLOAD;
    uint8_t *buf = malloc(cap, M_TEMP, M_WAITOK);
    if (buf == NULL) {
        return ENOMEM;
    }

    size_t total = 0;
    for (;;) {
        if (total + PROCFS_CTL_MAXPAYLOAD > cap) {
            size_t ncap = cap * 2;
            if (ncap > PROCFS_CMDLINE_MAX) {
                ncap = PROCFS_CMDLINE_MAX;
            }
            if (ncap <= total) {
                break;              /* hit the cap - return what we have */
            }
            uint8_t *nbuf = malloc(ncap, M_TEMP, M_WAITOK);
            if (nbuf == NULL) {
                free(buf, M_TEMP);
                return ENOMEM;
            }
            memcpy(nbuf, buf, total);
            free(buf, M_TEMP);
            buf = nbuf;
            cap = ncap;
        }

        uint32_t got = 0;
        int rc = procfs_ctl_request(PROCFS_REQ_PROCARGS, pid, total,
                                    buf + total, PROCFS_CTL_MAXPAYLOAD, &got);
        if (rc != 0) {
            if (total == 0) {
                free(buf, M_TEMP);
                return (rc == ENOTCONN || rc == ETIMEDOUT) ? ENOTSUP : rc;
            }
            break;                  /* partial region already accumulated */
        }
        total += got;
        if (got < PROCFS_CTL_MAXPAYLOAD) {
            break;                  /* end of the region */
        }
    }

    if (total == 0) {
        free(buf, M_TEMP);
        return EIO;
    }
    *bufp = buf;
    *lenp = total;
    return 0;
}

/*
 * Emit the parenthesised command name, e.g. "(launchd)", the way ps(1) renders a
 * process with no readable arguments (zombies, system processes, kernel
 * threads). Used as a fallback when the real argument vector is unavailable.
 */
static int
procfs_cmdline_comm(proc_t p, uio_t uio)
{
    char comm[MAXCOMLEN + 1];
    char namebuf[MAXCOMLEN + 4];

    comm[0] = '\0';
    proc_name(proc_pid(p), comm, sizeof(comm));     /* public KPI, offset-independent */

    int len = snprintf(namebuf, sizeof(namebuf), "(%s)", comm[0] ? comm : "unknown");
    return procfs_copy_data(namebuf, len, uio);
}

/*
 * Fetch the target's flattened argument region (the KERN_PROCARGS2 layout) from
 * the daemon and locate its argv / env / apple[] sections. On success returns 0
 * with *bufp a malloc'd buffer (free with M_TEMP), *lenp its length, and the
 * byte offsets where each section begins:
 *
 *   [ argc ][ exec path \0 pad ][ argv strings ][ env strings ][ apple strings ]
 *     ^0      ^4                  ^*argv_off      ^*env_off       ^*apple_off
 *
 * The env/apple boundary is the first "executable_path=" entry (apple[0]); if no
 * apple section is found, *apple_off == *lenp. Shared by cmdline (argv span),
 * environ (env span) and the native auxv node (apple span). The caller holds the
 * proc_find() reference (which pins the pid the daemon reads).
 */
int
procfs_read_procargs(proc_t p, uint8_t **bufp, size_t *lenp,
    size_t *argv_off, size_t *env_off, size_t *apple_off)
{
    *bufp = NULL;
    *lenp = 0;
    *argv_off = *env_off = *apple_off = 0;

    uint8_t *buf = NULL;
    size_t   n   = 0;
    int rc = procfs_fetch_procargs(proc_pid(p), &buf, &n);
    if (rc != 0) {
        return rc;
    }
    if (n < sizeof(int)) {
        free(buf, M_TEMP);
        return EINVAL;
    }

    /* KERN_PROCARGS2 leads with argc; the exec path, argv, env and apple[]
     * strings follow, exactly the layout the rest of this parse expects. */
    int argc;
    memcpy(&argc, buf, sizeof(argc));
    if (argc <= 0) {
        free(buf, M_TEMP);
        return EINVAL;
    }
    size_t pos = sizeof(int);

    /* The bare exec path, possibly with NUL alignment padding, precedes argv. */
    size_t pathlen = strnlen((const char *)buf + pos, n - pos);
    pos += pathlen;
    if (pos < n) {
        pos++;                                       /* path's NUL */
        while (pos < n && buf[pos] == '\0') {
            pos++;                                   /* alignment padding */
        }
    }
    *argv_off = pos;

    /* Skip the argc argv strings -> start of env. */
    for (int got = 0; got < argc && pos < n; got++) {
        size_t remaining = n - pos;
        size_t arglen = strnlen((const char *)buf + pos, remaining);
        if (arglen < remaining) {
            arglen++;                                /* include the NUL */
        }
        pos += arglen;
    }
    *env_off = pos;

    /* apple[] begins at the first "executable_path=" entry (apple[0]). */
    const size_t keylen = sizeof(PROCFS_EXEC_KEY) - 1;
    size_t apple = n;
    for (size_t scan = pos; scan < n; ) {
        size_t remaining = n - scan;
        if (remaining >= keylen &&
            memcmp(buf + scan, PROCFS_EXEC_KEY, keylen) == 0) {
            apple = scan;
            break;
        }
        size_t slen = strnlen((const char *)buf + scan, remaining);
        if (slen == remaining) {
            break;                                   /* no terminator; stop */
        }
        scan += slen + 1;
    }
    *apple_off = apple;

    *bufp = buf;
    *lenp = n;
    return 0;
}

/*
 * Fetch and emit the argument vector for a live, non-system process: the argv
 * span of the argument region. Mirrors the role of NetBSD's
 * procfs_doprocargs_helper(). The caller holds the proc_find() reference.
 */
static int
procfs_doprocargs_helper(proc_t p, uio_t uio)
{
    uint8_t *buf = NULL;
    size_t   n = 0, argv_off = 0, env_off = 0, apple_off = 0;

    if (procfs_read_procargs(p, &buf, &n, &argv_off, &env_off, &apple_off) != 0) {
        return procfs_cmdline_comm(p, uio);          /* fall back to (comm) */
    }

    int error = procfs_copy_data((const char *)(buf + argv_off),
                                 (int)(env_off - argv_off), uio);
    free(buf, M_TEMP);
    return error;
}

/*
 * Reads the data for the "cmdline" node: the process's argument vector, with
 * each argument NUL-terminated (the Linux /proc/<pid>/cmdline format). Named
 * after NetBSD's procfs_doprocargs(); it dispatches the special zombie/system
 * cases here and delegates a live process to procfs_doprocargs_helper().
 */
int
procfs_doprocargs(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p == PROC_NULL) {
        return ESRCH;
    }

    /* The node is read-only. */
    if (uio_rw(uio) != UIO_READ) {
        proc_rele(p);
        return EOPNOTSUPP;
    }

    /*
     * Zombies and system processes have no user stack to read arguments from,
     * so report the command name in parentheses, as ps(1) does.
     */
    if ((p->p_stat == SZOMB) || (p->p_flag & P_SYSTEM) != 0) {
        error = procfs_cmdline_comm(p, uio);
    } else {
        error = procfs_doprocargs_helper(p, uio);
    }

    proc_rele(p);
    return error;
}

/*
 * Reads the "environ" node: the process's environment strings, NUL-separated
 * (the Linux /proc/<pid>/environ format). Same source and reader as cmdline,
 * emitting the env span of the argument region (between argv and apple[]) rather
 * than argv. Zombies/system processes have no user stack, so report empty.
 */
int
procfs_doenviron(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    if (uio_rw(uio) != UIO_READ) {
        return EOPNOTSUPP;
    }

    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p == PROC_NULL) {
        return ESRCH;
    }

    int error;
    if ((p->p_stat == SZOMB) || (p->p_flag & P_SYSTEM) != 0) {
        error = procfs_copy_data("", 0, uio);
    } else {
        uint8_t *buf = NULL;
        size_t   n = 0, argv_off = 0, env_off = 0, apple_off = 0;
        if (procfs_read_procargs(p, &buf, &n, &argv_off, &env_off, &apple_off) == 0) {
            error = procfs_copy_data((const char *)(buf + env_off),
                                     (int)(apple_off - env_off), uio);
            free(buf, M_TEMP);
        } else {
            error = procfs_copy_data("", 0, uio);
        }
    }

    proc_rele(p);
    return error;
}
