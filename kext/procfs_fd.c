/*
 * Copyright (c) 2015 Kim Topley
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_fd.c
 *
 * Data functions for the per-process file descriptor nodes (fd/<n>/details and fd/<n>/socket).
 */
#include <libkern/libkern.h>
#include <sys/bsdtask_info.h>
#include <sys/file.h>
#include <sys/filedesc.h>
#include <sys/kauth.h>
#include <sys/kpi_socket.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#include <sys/proc_internal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <sys/vnode.h>
#include <netinet/in.h>

#include <fs/procfs/procfs.h>
#include <fs/procfs/procfs_ctl.h>

#include <libkprocfs/kern.h>

/*
 * Reads the data associated with a file descriptor node: a
 * vnode_fdinfowithpath structure describing the target vnode and the file.
 *
 * Served by the procfsd daemon (proc_pidfdinfo(PROC_PIDFDVNODEPATHINFO)) rather
 * than an in-kernel fd-table walk, which depends on struct filedesc offsets that
 * drift across kernel point-releases. Returns the daemon's errno (ENOTCONN with
 * no daemon).
 */
int
procfs_dofd(pfsnode_t *pnp, __unused uio_t uio, __unused vfs_context_t ctx)
{
    int fd  = pnp->node_id.nodeid_objectid;
    int pid = pnp->node_id.nodeid_pid;

    struct vnode_fdinfowithpath info;
    uint32_t got = 0;
    int error = procfs_ctl_request(PROCFS_REQ_FDINFO, pid, (uint64_t)fd,
                                   &info, sizeof(info), &got);
    if (error != 0) {
        return error;
    }
    if (got != sizeof(info)) {
        return EIO;
    }
    return procfs_copy_data((const char *)&info, sizeof(info), uio);
}

/*
 * Fills a socket_info from a socket using only the public, layout-independent
 * sock_* KPIs (the private fill_socketinfo() can neither be linked nor safely
 * forward-ported through the deep socket/inpcb/tcpcb structs). This covers the
 * common fields plus the addresses for IP and UNIX sockets; the per-protocol
 * deep state (e.g. TCP window/state) is left zeroed.
 */
static void
procfs_fill_socketinfo(socket_t so, struct socket_info *si)
{
    int dom = 0, type = 0, proto = 0;

    sock_gettype(so, &dom, &type, &proto);
    si->soi_so = (uint64_t)so;
    si->soi_type = type;
    si->soi_protocol = proto;
    si->soi_family = dom;

    if (dom == AF_INET || dom == AF_INET6) {
        struct in_sockinfo *ini;
        if (proto == IPPROTO_TCP) {
            si->soi_kind = SOCKINFO_TCP;
            ini = &si->soi_proto.pri_tcp.tcpsi_ini;
        } else {
            si->soi_kind = SOCKINFO_IN;
            ini = &si->soi_proto.pri_in;
        }

        struct sockaddr_storage ss;
        if (sock_getsockname(so, (struct sockaddr *)&ss, sizeof(ss)) == 0) {
            if (dom == AF_INET) {
                struct sockaddr_in *s = (struct sockaddr_in *)&ss;
                ini->insi_vflag = INI_IPV4;
                ini->insi_lport = s->sin_port;
                ini->insi_laddr.ina_46.i46a_addr4 = s->sin_addr;
            } else {
                struct sockaddr_in6 *s = (struct sockaddr_in6 *)&ss;
                ini->insi_vflag = INI_IPV6;
                ini->insi_lport = s->sin6_port;
                ini->insi_laddr.ina_6 = s->sin6_addr;
            }
        }
        if (sock_getpeername(so, (struct sockaddr *)&ss, sizeof(ss)) == 0) {
            if (dom == AF_INET) {
                struct sockaddr_in *s = (struct sockaddr_in *)&ss;
                ini->insi_fport = s->sin_port;
                ini->insi_faddr.ina_46.i46a_addr4 = s->sin_addr;
            } else {
                struct sockaddr_in6 *s = (struct sockaddr_in6 *)&ss;
                ini->insi_fport = s->sin6_port;
                ini->insi_faddr.ina_6 = s->sin6_addr;
            }
        }
    } else if (dom == AF_UNIX) {
        struct un_sockinfo *un = &si->soi_proto.pri_un;
        si->soi_kind = SOCKINFO_UN;
        sock_getsockname(so, (struct sockaddr *)&un->unsi_addr.ua_sun,
            sizeof(un->unsi_addr.ua_sun));
        sock_getpeername(so, (struct sockaddr *)&un->unsi_caddr.ua_sun,
            sizeof(un->unsi_caddr.ua_sun));
    } else {
        si->soi_kind = SOCKINFO_GENERIC;
    }
}

/*
 * Reads the data associated with a file descriptor that refers to a socket:
 * a socket_fdinfo. Served by the procfsd daemon
 * (proc_pidfdinfo(PROC_PIDFDSOCKETINFO)) - which fills the full per-protocol
 * socket_info from userspace - rather than the in-kernel fd-table walk.
 */
int
procfs_dosocket(pfsnode_t *pnp, __unused uio_t uio, __unused vfs_context_t ctx)
{
    int fd  = pnp->node_id.nodeid_objectid;
    int pid = pnp->node_id.nodeid_pid;

    struct socket_fdinfo info;
    uint32_t got = 0;
    int error = procfs_ctl_request(PROCFS_REQ_FDSOCKET, pid, (uint64_t)fd,
                                   &info, sizeof(info), &got);
    if (error != 0) {
        return error;
    }
    if (got != sizeof(info)) {
        return EIO;
    }
    return procfs_copy_data((const char *)&info, sizeof(info), uio);
}

/*
 * Gets the size for the node that represents the file descriptors 
 * of a process. Counts one for every open file in the process.
 */
size_t
procfs_fd_node_size(pfsnode_t *pnp, __unused kauth_cred_t creds)
{
    size_t count = 0;

    int pid = pnp->node_id.nodeid_pid;
    proc_t p = proc_find(pid);

    if (p != PROC_NULL) {
        struct proc_fdinfo *fdlist = NULL;
        size_t fd_count = 0;
        if (procfs_get_fd_list(p, &fdlist, &fd_count) == 0) {
            count = fd_count;
        }
        procfs_release_fd_list(fdlist);
        proc_rele(p);
    }

    return count;
}

/*
 * Resolve the target path of a per-process symlink (exe/cwd/root) for `pid` into
 * `buf`. "exe" -> the executable vnode (p_textvp); "cwd"/"root" -> the process's
 * fd-table current/root directory (p_fd is embedded in struct proc). An iocount
 * is taken (vnode_get) before vn_getpath, as in the fd/ path resolution; the
 * read is best-effort (no fd lock), and vnode_get fails cleanly for a stale
 * vnode. "root" with no explicit root vnode resolves to "/". Returns 0 or errno.
 */
int
procfs_proclink_path(int pid, const char *name, char *buf, int buflen)
{
    proc_t p = proc_find(pid);
    if (p == PROC_NULL) {
        return ESRCH;
    }

    vnode_t   vp      = NULLVP;
    boolean_t is_root = FALSE;
    if (strcmp(name, "exe") == 0) {
        vp = p->p_textvp;
    } else if (strcmp(name, "cwd") == 0) {
        vp = p->p_fd.fd_cdir;
    } else if (strcmp(name, "root") == 0) {
        is_root = TRUE;
        vp = p->p_fd.fd_rdir;
    }

    int error = ENOENT;
    if (vp != NULLVP && vnode_get(vp) == 0) {
        int len = buflen;
        if (vn_getpath(vp, buf, &len) == 0) {
            error = 0;
        }
        vnode_put(vp);
    } else if (is_root) {
        strlcpy(buf, "/", buflen);          /* no explicit root vnode -> "/" */
        error = 0;
    }

    proc_rele(p);
    return error;
}
