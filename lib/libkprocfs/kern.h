/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 */
#ifndef _procfs_kern_h
#define _procfs_kern_h

extern int proc_pidshortbsdinfo(proc_t p, struct proc_bsdshortinfo * pbsd_shortp, int zombie);
extern int proc_pidtaskinfo(proc_t p, struct proc_taskinfo * ptinfo);
extern int proc_pidthreadinfo(proc_t p, uint64_t arg, bool thuniqueid, struct proc_threadinfo *pthinfo);

extern int fill_fileinfo(struct fileproc * fp, proc_t p, int fd, struct proc_fileinfo * fi);
extern int fill_vnodeinfo(vnode_t vp, struct vnode_info *vinfo, boolean_t check_fsgetpath);

/*
 * Validates a vnode-backed descriptor of a target process and returns its
 * vnode, vnode id, and proc_fileinfo, all captured under proc_fdlock. The
 * caller takes a vnode iocount via vnode_getwithvid(*vid). Replaces the private
 * fp_getfvp() without needing a fileproc iocount (whose os_ref retain/release
 * path is unlinkable from a third-party kext). Returns EBADF for an invalid or
 * non-vnode descriptor.
 */
extern int procfs_fd_vnode_info(proc_t p, int fd, struct vnode **vpp, uint32_t *vidp, struct proc_fileinfo *fi);

/*
 * Socket counterpart of procfs_fd_vnode_info(): validates a socket descriptor
 * of a target process and returns the socket with a sock_retain() reference the
 * caller must release with sock_release(), plus the proc_fileinfo. Returns
 * EBADF for an invalid or non-socket descriptor.
 */
extern int procfs_fd_socket(proc_t p, int fd, socket_t *sop, struct proc_fileinfo *fi);

/*
 * Enumerate the system's held byte-range file locks into an sbuf in Linux's
 * /proc/locks format. Walks every process's descriptors down to their vnodes
 * and emits each vnode's advisory-lock list once (de-duplicated), snapshotting
 * under the vnode mutex. See procfs_build_locks() for details.
 */
struct sbuf;
extern void procfs_build_locks(struct sbuf *sb, vfs_context_t ctx);

/*
 * Fill *unslid with the un-slid kernel address of a process's representative
 * thread's continuation (its wchan), or 0 if it has none. Returns 0 on success.
 * The continuation-field offset is discovered once at runtime. See kern.c.
 */
extern int procfs_thread_wchan_unslid(proc_t p, uint64_t *unslid);

#endif