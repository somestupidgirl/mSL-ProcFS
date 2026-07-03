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

#include <libkprocfs/symbols.h>
#include <libkprocfs/kern.h>

/*
 * Reads the data associated with a file descriptor node. The data is 
 * a vnode_infowithpath structure containing information about both the target
 * vnode and the file itself.
 */
int
procfs_dofd(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx)
{
    int error = 0;

    // We need the file descriptor and the process id. We get
    // both of them from the node id.
    int fd = pnp->node_id.nodeid_objectid;
    proc_t p = proc_find(pnp->node_id.nodeid_pid);

    if (p == PROC_NULL) {
        return ESRCH;
    }

    struct vnode_fdinfowithpath info;
    bzero(&info, sizeof(info));

    // Validate the descriptor and capture the vnode + vnode id + file info
    // under proc_fdlock (which keeps the fileproc alive). No fileproc iocount
    // is taken: the os_ref retain/release path is unavailable to a third-party
    // kext, so instead we re-acquire the vnode by id once the lock is dropped.
    vnode_t vp = NULLVP;
    uint32_t vid = 0;
    error = procfs_fd_vnode_info(p, fd, &vp, &vid, &info.pfi);
    if (error == 0) {
        // vnode_getwithvid() takes an iocount and fails cleanly if the vnode
        // was reclaimed between dropping proc_fdlock and here.
        error = vnode_getwithvid(vp, vid);
        if (error == 0) {
            error = fill_vnodeinfo(vp, &info.pvip.vip_vi, FALSE);
            if (error == 0) {
                // Add the file path and copy the data out to user space.
                int count = MAXPATHLEN;
                vn_getpath(vp, info.pvip.vip_path, &count);
                info.pvip.vip_path[MAXPATHLEN-1] = 0;
                // vn_getpath()/build_path() constructs the path backwards from
                // the end of the buffer and leaves a stale copy in the tail;
                // zero everything past the returned string so the node holds
                // the path exactly once.
                size_t plen = strlen(info.pvip.vip_path);
                bzero(info.pvip.vip_path + plen, MAXPATHLEN - plen);
                error = procfs_copy_data((const char *)&info, sizeof(info), uio);
            }
            vnode_put(vp);
        }
    }

    proc_rele(p);
    return error;
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
 * Reads the data associated with a file descriptor that refers to a socket.
 */
int
procfs_dosocket(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;
    int fd = pnp->node_id.nodeid_objectid;
    proc_t p = proc_find(pnp->node_id.nodeid_pid);

    if (p == PROC_NULL) {
        return ESRCH;
    }

    struct socket_fdinfo info;
    bzero(&info, sizeof(info));
    socket_t so = NULL;

    // Validate the socket descriptor and take a sock_retain() reference under
    // proc_fdlock so the socket survives once the lock is dropped (sockets have
    // no vnode-id reclaim guard). info.pfi is filled while the lock is held.
    error = procfs_fd_socket(p, fd, &so, &info.pfi);
    if (error == 0) {
        procfs_fill_socketinfo(so, &info.psi);
        sock_release(so);
        error = procfs_copy_data((const char *)&info, sizeof(info), uio);
    }

    proc_rele(p);
    return error;
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
