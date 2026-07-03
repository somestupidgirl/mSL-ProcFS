/*
 * Copyright (c) 2026 Sunneva N. Mariu
 *
 * procfs_extensions.c
 *
 * /proc/extensions - a macOS-style listing of the loaded kernel extensions
 * (kextstat-like: index, refs, address, size, name and version). A kext cannot
 * enumerate loaded kexts itself without the private C++ OSKext class, so the
 * listing is produced by the procfsd daemon (KextManagerCopyLoadedKextInfo) and
 * delivered over the kernel-control bridge. The list is far larger than one
 * bridge payload, so it is streamed in chunks and reassembled here.
 */
#include <sys/errno.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/sbuf.h>

#include <fs/procfs/procfs.h>
#include <fs/procfs/procfs_ctl.h>

int
procfs_doextensions(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 4096, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    /* The daemon formats the kextstat-style listing; without it, the node is
     * empty (there is no in-kernel source we can link against). */
    int error = procfs_ctl_request_blob(PROCFS_REQ_EXTENSIONS, &sb);
    if (error != 0) {
        sbuf_delete(&sb);
        return (error == ENOTCONN) ? 0 : error;    /* no daemon -> empty node */
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);
    return error;
}
