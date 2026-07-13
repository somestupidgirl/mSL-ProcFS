/*
 * Copyright (c) 2026 Sunneva N. Mariu
 *
 * procfs_extensions.c
 *
 * /proc/extensions - a macOS/kextstat-style listing of the loaded kernel
 * extensions (index, refs, address, size, name/version). The Linux-format
 * counterpart, /proc/modules, lives in procfs_linux.c and shares the transfer
 * helper below.
 *
 * A kext cannot enumerate loaded kexts itself without the private C++ OSKext
 * class, so the listing is produced by the procfsd daemon
 * (KextManagerCopyLoadedKextInfo) and delivered over the kernel-control bridge.
 * The list is far larger than one bridge payload, so it is streamed in chunks
 * and reassembled here.
 */
#include <sys/errno.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/sbuf.h>

#include <fs/procfs/procfs.h>
#include <fs/procfs/procfs_ctl.h>

/*
 * Reassemble the daemon's chunked kext listing for `type` (PROCFS_REQ_EXTENSIONS
 * or PROCFS_REQ_MODULES, which selects the format) and copy it out. Shared by
 * /proc/extensions here and /proc/modules in procfs_linux.c.
 */
int
procfs_dokextlist(uint32_t type, uio_t uio)
{
    int error = 0;

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 4096, SBUF_AUTOEXTEND) == NULL) {
        return (error == ENOTCONN);
    }

    error = procfs_ctl_request_blob(type, &sb);
    if (error != 0) {
        sbuf_delete(&sb);
        return (error == ENOTCONN) ? 0 : error;    /* no daemon -> empty node */
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);
    return error;
}

int
procfs_doextensions(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    return procfs_dokextlist(PROCFS_REQ_EXTENSIONS, uio);
}
