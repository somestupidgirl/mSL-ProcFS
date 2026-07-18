/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_auxv.c
 *
 * Process auxiliary-vector node (/proc/<pid>/auxv).
 *
 * macOS executables are Mach-O, not ELF, so there is no classic ELF auxiliary
 * vector (the AT_* key/value array). XNU's direct equivalent is dyld's "apple[]"
 * array: the NUL-terminated key=value strings the kernel places after argv and
 * envp (executable_path=, stack_guard=, malloc_entropy=, ptr_munge=, dyld_file=,
 * main_stack=, arm64e_abi=, ...). This node emits that array.
 *
 * The apple strings live at the tail of the flattened KERN_PROCARGS2 argument
 * region (after the argv and env strings), which procfs_read_procargs() (shared
 * with procfs_cmdline.c) reads through the target's pmap and splits into its
 * argv / env / apple sections. This node emits the apple span.
 *
 * A human-readable, Linux-style AT_* rendering is provided separately in
 * procfs_linux.c, selected at runtime by the procfs.linux sysctl.
 */
#include <stdint.h>

#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <sys/uio.h>
#include <fs/procfs/procfs.h>

int
procfs_doauxv(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;

    if (uio_rw(uio) != UIO_READ) {
        return EOPNOTSUPP;       /* the node is read-only */
    }

    /* Linux presentation mode renders synthesized AT_* text (procfs_linux.c). */
    if (procfs_linux_mode) {
        return procfs_doauxv_linux(pnp, uio, ctx);
    }

    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p == PROC_NULL) {
        return ESRCH;
    }

    /* Zombies and system processes have no user stack to read from. */
    if ((p->p_stat == SZOMB) || (p->p_flag & P_SYSTEM) != 0) {
        error = procfs_copy_data("", 0, uio);           /* empty (EOF) */
    } else {
        uint8_t *buf = NULL;
        size_t   n = 0, argv_off = 0, env_off = 0, apple_off = 0;
        if (procfs_read_procargs(p, &buf, &n, &argv_off, &env_off, &apple_off) == 0) {
            error = procfs_copy_data((const char *)(buf + apple_off),
                                     (int)(n - apple_off), uio);
            free(buf, M_TEMP);
        } else {
            error = procfs_copy_data("", 0, uio);        /* nothing readable */
        }
    }

    proc_rele(p);
    return error;
}
