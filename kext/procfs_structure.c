/*
 * Copyright (c) 2015 Kim Topley
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_structure.c
 *
 * Definition and management of the file system layout. The
 * layout is defined by a tree of pfssnode_t
 * objects, starting with the root of the file system. The
 * structure is created in the procfs_structure_init() function
 * and is used while servicing VNOP_LOOKUP and VNOP_READDIR. To
 * add new file system nodes, add the corresponding entries in
 * procfs_structure_init() and make any necessary changes in
 * the procfs_vnop_lookup() and procfs_vnop_readdir() functions.
 * When adding files, it's also necessary to add functions that
 * return the file's data and its size, unless the size is
 * fixed. To do that, add the required functions in the
 * file procfs_data.c and link to them from the pfssnode_t.
 */
#include <string.h>
#include <kern/assert.h>
#include <kern/debug.h>
#include <libkern/OSMalloc.h>
#include <mach/boolean.h>
#include <sys/proc_info.h>
#include <sys/vnode.h>

#include <fs/procfs/procfs.h>

#pragma mark -
#pragma mark Function Prototypes

STATIC pfssnode_t *add_node(pfssnode_t *parent, const char *name, pfstype type, pfsbaseid_t node_id, uint16_t flags, size_t size,
                        procfs_node_size_fn node_size_fn, procfs_read_data_fn node_read_data_fn);

STATIC pfssnode_t *add_directory(pfssnode_t *parent, const char *name, pfstype type, pfsbaseid_t node_id, uint16_t flags, boolean_t raw,
                        procfs_node_size_fn node_size_fn, procfs_read_data_fn node_read_data_fn);

STATIC pfssnode_t *add_file(pfssnode_t *parent, const char *name, pfsbaseid_t node_id, uint16_t flags, size_t size,
                        procfs_node_size_fn node_size_fn, procfs_read_data_fn node_read_data_fn);

STATIC void release_node(pfssnode_t *node);

// Next node id. No need to lock this value because access
// is guaranteed to be single-threaded. Start at 2 because the
// root node is always 1.
STATIC uint16_t next_node_id = PROCFS_ROOT_NODE_BASE_ID + 1;

// The root of the file system structure.
static pfssnode_t *root_node;

#pragma mark - 
#pragma mark Externally Visible Functions

// Gets the root node of the file system structure.
pfssnode_t *
procfs_structure_root_node(void)
{
    return root_node;
}

// Initializes the procfs structures. Should only be called
// while mounting a file system. Given that restriction, we do
// not need to lock access to the structure data when deciding
// whether to creating it.
//
// NOTE: it is essential that the entries that expand to dynamic
// content be the last in their parent's child list. This makes
// the code that implements the readdir operation as simple as
// possible.
void
procfs_structure_init(void)
{
    // Only do this on first mount.
    if (root_node == NULL) {
        // The root directory of the file system. This happens to be the only node
        // that has the same node id on all instance of this file system.
        root_node = add_directory(NULL, "/", PFSroot, PROCFS_ROOT_NODE_BASE_ID, 0, 0, NULL, NULL);

        // A directory that contains all of the visible processes, listed by command name.
        // Each entry in this directory is a symbolic link to the process entry in root (e.g. "../123).
        pfssnode_t *proc_by_name_dir = add_directory(root_node, "byname",
                        PFSdir, next_node_id++, 0, 0, NULL, NULL);

        // Linux-compatible /proc/cpuinfo
        pfssnode_t *cpuinfo = add_node(root_node, "cpuinfo",
                        PFScpuinfo, next_node_id++, 0, 0, NULL, procfs_docpuinfo);

        // A link in the root node to the current process entry. This will become a symbolic link.
        pfssnode_t *curproc = add_node(root_node, "curproc",
                        PFScurproc, next_node_id++, 0, 0, NULL, NULL);

        // Linux-compatible /proc/cpuinfo
        pfssnode_t *loadavg = add_node(root_node, "loadavg",
                        PFSloadavg, next_node_id++, 0, 0, NULL, procfs_doloadavg);

        // Linux-compatible /proc/meminfo
        pfssnode_t *meminfo = add_node(root_node, "meminfo",
                        PFSmeminfo, next_node_id++, 0, 0, NULL, procfs_domeminfo);

        // Linux-compatible /proc/mtab
        pfssnode_t *mtab = add_node(root_node, "mtab",
                        PFSmtab, next_node_id++, 0, 0, NULL, procfs_domtab);

        // Linux-compatible /proc/mounts (same mounted-filesystem table as mtab).
        pfssnode_t *mounts = add_node(root_node, "mounts",
                        PFSmtab, next_node_id++, 0, 0, NULL, procfs_domtab);

        // Linux-compatible /proc/stat
        pfssnode_t *stat = add_node(root_node, "stat",
                        PFSstat, next_node_id++, 0, 0, NULL, procfs_dostat);

        // Linux-compatible /proc/vmstat
        pfssnode_t *vmstat = add_node(root_node, "vmstat",
                        PFSvmstat, next_node_id++, 0, 0, NULL, procfs_dovmstat);

        pfssnode_t *partitions = add_node(root_node, "partitions",
                        PFSpartitions, next_node_id++, 0, 0, NULL, procfs_dopartitions);

        // Linux-compatible /proc/version
        pfssnode_t *version = add_node(root_node, "version",
                        PFSversion, next_node_id++, 0, 0, NULL, procfs_doversion);

        // Linux-compatible /proc/uptime
        pfssnode_t *uptime = add_node(root_node, "uptime",
                        PFSuptime, next_node_id++, 0, 0, NULL, procfs_douptime);

        // Linux-compatible /proc/swaps
        pfssnode_t *swaps = add_node(root_node, "swaps",
                        PFSswaps, next_node_id++, 0, 0, NULL, procfs_doswaps);

        // Linux-compatible /proc/filesystems
        pfssnode_t *filesystems = add_node(root_node, "filesystems",
                        PFSfilesystems, next_node_id++, 0, 0, NULL, procfs_dofilesystems);

        // Linux-compatible /proc/sys - a dynamic mirror of the sysctl tree. This
        // single PFSsysctl node backs /proc/sys and every descendant; the
        // specific sysctl oid is carried per-vnode in the node id's objectid
        // (0 == the tree root). No static children or read fn (see procfs_sysctl.c).
        pfssnode_t *sysctl = add_node(root_node, "sys",
                        PFSsysctl, next_node_id++, 0, 0, NULL, NULL);

        // macOS-style loaded kernel-extension listing (data from procfsd).
        pfssnode_t *extensions = add_node(root_node, "extensions",
                        PFSextensions, next_node_id++, 0, 0, NULL, procfs_doextensions);

        // Linux-style loaded kernel-module listing (same data, /proc/modules format).
        pfssnode_t *modules = add_node(root_node, "modules",
                        PFSmodules, next_node_id++, 0, 0, NULL, procfs_domodules);

        // Linux-style block-device I/O statistics (IOKit IOBlockStorageDriver).
        pfssnode_t *diskstats = add_node(root_node, "diskstats",
                        PFSdiskstats, next_node_id++, 0, 0, NULL, procfs_dodiskstats);

        // Linux-style char/block device major listing (derived from /dev by procfsd).
        pfssnode_t *devices = add_node(root_node, "devices",
                        PFSdevices, next_node_id++, 0, 0, NULL, procfs_dodevices);

        // Linux-style /proc/allocinfo (per-zone allocation stats via mach_zone_info in procfsd).
        pfssnode_t *allocinfo = add_node(root_node, "allocinfo",
                        PFSallocinfo, next_node_id++, 0, 0, NULL, procfs_doallocinfo);

        // Linux-style /proc/apm (power/battery state via IOKit power sources in procfsd).
        pfssnode_t *apm = add_node(root_node, "apm",
                        PFSapm, next_node_id++, 0, 0, NULL, procfs_doapm);

        // Linux-style kernel boot command line (macOS boot-args / kern.bootargs).
        pfssnode_t *kcmdline = add_node(root_node, "cmdline",
                        PFSkcmdline, next_node_id++, 0, 0, NULL, procfs_dokcmdline);

        // Linux-compatible /proc/self (symlink to the calling process; resolves
        // exactly like "curproc", the BSD name).
        pfssnode_t *self = add_node(root_node, "self",
                        PFScurproc, next_node_id++, 0, 0, NULL, NULL);

        // Linux-style /proc/net/ subdirectory and its network-interface stats file.
        pfssnode_t *net_dir = add_directory(root_node, "net",
                        PFSdir, next_node_id++, 0, 0, NULL, NULL);
        pfssnode_t *netdev = add_node(net_dir, "dev",
                        PFSnetdev, next_node_id++, 0, 0, NULL, procfs_donetdev);

        // A pseudo-entry below "byname" that is replaced by nodes for all of the visible processes.
        // NOTE: this must be the last child entry for the "byname" node.
        pfssnode_t *proc_name_dir = add_directory(proc_by_name_dir, "__Process_N__",
                        PFSprocnamedir, next_node_id++, PSN_FLAG_PROCESS, 0, procfs_process_node_size, NULL);

        // A pseudo-entry below "/" that is replaced by nodes for all of the visible processes.
        // NOTE: this must be the last child entry for the root node.
        pfssnode_t *one_proc_dir = add_directory(root_node, "__Process__",
                       PFSproc, next_node_id++, PSN_FLAG_PROCESS, 0, procfs_process_node_size, NULL);

        // A directory below the node for a process to hold all the file descriptors for that process.
        pfssnode_t *fd_dir = add_directory(one_proc_dir, "fd",
                       PFSdir, next_node_id++, PSN_FLAG_PROCESS, 0, NULL, NULL);

        // A pseudo-entry below the "fd" node that is replaced by nodes for all the open files of
        // the current process.
        // NOTE: this must be the last child entry for the "fd" node.
        pfssnode_t *one_fd_dir = add_directory(fd_dir, "__File__",
                      PFSfd, next_node_id++, PSN_FLAG_PROCESS, 0, procfs_fd_node_size, NULL);

        // A directory below the node for a process to hold all the threads for that process.
        pfssnode_t *threads_dir = add_directory(one_proc_dir, "threads",
                       PFSdir, next_node_id++, PSN_FLAG_PROCESS, 0, NULL, NULL);

        // A pseudo-entry below the "threads" node that is replaced by nodes for all the threads of
        // the current process.
        // NOTE: this must be the last child entry for the threads node.
        pfssnode_t *one_thread_dir = add_directory(threads_dir, "__Thread__",
                      PFSthread, next_node_id++, PSN_FLAG_PROCESS | PSN_FLAG_THREAD, 0, procfs_thread_node_size, NULL);

        // The Linux name for the same per-thread view: /proc/<pid>/task/<tid>.
        // It mirrors "threads" (a separate structure node, so thread fileids do
        // not collide between the two).
        pfssnode_t *task_dir = add_directory(one_proc_dir, "task",
                       PFSdir, next_node_id++, PSN_FLAG_PROCESS, 0, NULL, NULL);

        // A pseudo-entry below "task" that is replaced by nodes for all the
        // threads of the current process.
        // NOTE: this must be the last child entry for the task node.
        pfssnode_t *one_task_dir = add_directory(task_dir, "__Thread__",
                      PFSthread, next_node_id++, PSN_FLAG_PROCESS | PSN_FLAG_THREAD, 0, procfs_thread_node_size, NULL);

        // --- Per-proccess sub-directories and files.

        // Files that returns the process's pid, parent pid, process group id,
        // session id and controlling terminal name.
        add_file(one_proc_dir, "cmdline", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_doprocargs);
        add_file(one_proc_dir, "pid", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_dopid);
        add_file(one_proc_dir, "ppid", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_doppid);
        add_file(one_proc_dir, "pgid", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_dopgid);
        add_file(one_proc_dir, "sid", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_dosid);
        add_file(one_proc_dir, "tty", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_dotty);
        add_file(one_proc_dir, "status", next_node_id++, PSN_FLAG_PROCESS, sizeof(struct proc_bsdshortinfo), NULL, procfs_dostatus);
        add_file(one_proc_dir, "taskinfo", next_node_id++, PSN_FLAG_PROCESS, sizeof(struct proc_taskinfo), NULL, procfs_dotaskinfo);

        add_file(one_proc_dir, "note", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_donote);
        add_file(one_proc_dir, "limit", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_dolimit);
        add_file(one_proc_dir, "mem", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_domem);
        add_file(one_proc_dir, "map", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_domap);
        add_file(one_proc_dir, "maps", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_domaps);
        add_file(one_proc_dir, "smaps", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_dosmaps);
        add_file(one_proc_dir, "smaps_rollup", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_dosmaps_rollup);
        add_file(one_proc_dir, "numa_maps", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_donuma_maps);

        // Linux-compatible per-process symlinks: exe/cwd/root (target resolved
        // by node name in vnop_readlink; read fn is NULL).
        add_node(one_proc_dir, "exe",  PFSproclink, next_node_id++, PSN_FLAG_PROCESS, 0, NULL, NULL);
        add_node(one_proc_dir, "cwd",  PFSproclink, next_node_id++, PSN_FLAG_PROCESS, 0, NULL, NULL);
        add_node(one_proc_dir, "root", PFSproclink, next_node_id++, PSN_FLAG_PROCESS, 0, NULL, NULL);

        // Linux-compatible per-process text files.
        add_file(one_proc_dir, "comm", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_docomm);
        add_file(one_proc_dir, "stat", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_doprocstat);
        add_file(one_proc_dir, "statm", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_dostatm);
        add_file(one_proc_dir, "io", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_doio);
        add_file(one_proc_dir, "environ", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_doenviron);

        // Native Mach register dumps for the process's representative thread.
        add_file(one_proc_dir, "regs", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_doregs);
        add_file(one_proc_dir, "fpregs", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_dofpregs);

        // Auxiliary vector (XNU's apple[] array - the macOS auxv equivalent).
        add_file(one_proc_dir, "auxv", next_node_id++, PSN_FLAG_PROCESS, 0, NULL, procfs_doauxv);

        // --- Per thread files.
        add_file(one_thread_dir, "info", next_node_id++, PSN_FLAG_PROCESS | PSN_FLAG_THREAD, sizeof(struct proc_threadinfo), NULL, procfs_dothreadinfo);
        add_file(one_task_dir, "info", next_node_id++, PSN_FLAG_PROCESS | PSN_FLAG_THREAD, sizeof(struct proc_threadinfo), NULL, procfs_dothreadinfo);

        // Linux-compatible per-thread files under /proc/<pid>/task/<tid>/.
        // maps reuses the process map walk (threads share the address space).
        add_file(one_task_dir, "comm", next_node_id++, PSN_FLAG_PROCESS | PSN_FLAG_THREAD, 0, NULL, procfs_dothreadcomm);
        add_file(one_task_dir, "stat", next_node_id++, PSN_FLAG_PROCESS | PSN_FLAG_THREAD, 0, NULL, procfs_dothreadstat);
        add_file(one_task_dir, "status", next_node_id++, PSN_FLAG_PROCESS | PSN_FLAG_THREAD, 0, NULL, procfs_dothreadstatus);
        add_file(one_task_dir, "sched", next_node_id++, PSN_FLAG_PROCESS | PSN_FLAG_THREAD, 0, NULL, procfs_dothreadsched);
        add_file(one_task_dir, "maps", next_node_id++, PSN_FLAG_PROCESS | PSN_FLAG_THREAD, 0, NULL, procfs_domaps);

        // --- Per file descriptor files.
        add_file(one_fd_dir, "details", next_node_id++, PSN_FLAG_PROCESS, sizeof(struct vnode_fdinfowithpath), NULL, procfs_dofd);
        add_file(one_fd_dir, "socket", next_node_id++, PSN_FLAG_PROCESS, sizeof(struct socket_fdinfo), NULL, procfs_dosocket);
    }
}

// Frees the memory for the procfs structures. Should only be called
// while unmounting the last instance of the file system. Given that
// restriction, we do not need to lock access to the structure data
// when freeing it.
void
procfs_structure_free()
{
    if (root_node != NULL) {
        // Release the root node. This recursively releases
        // all descendent nodes.
        release_node(root_node);
        root_node = NULL;
    }
}

#pragma mark -
#pragma mark Creation of Structure Nodes

/*
 * Adds a node to the file system structure. This is a low-level
 * function that is called by add_file() and add_directory(). It
 * should not be called directly.
 */
STATIC pfssnode_t *
add_node(pfssnode_t *parent, const char *name, pfstype type, pfsbaseid_t node_id, uint16_t flags, size_t size,
            procfs_node_size_fn node_size_fn, procfs_read_data_fn node_read_data_fn)
{
    pfssnode_t *node = (pfssnode_t *)OSMalloc(sizeof(pfssnode_t), procfs_osmalloc_tag);
    if (node == NULL) {
        panic("Unable to allocate memory for pfssnode_t");
    }

    bzero(node, sizeof(pfssnode_t));
    node->psn_node_type = type;
    strlcpy(node->psn_name, name, sizeof(node->psn_name));
    node->psn_base_node_id = node_id;
    node->psn_flags = flags;
    node->psn_parent = parent;
    node->psn_node_size = size;
    node->psn_getsize_fn = node_size_fn;
    node->psn_read_data_fn = node_read_data_fn;

    TAILQ_INIT(&node->psn_children);
    if (parent != NULL) {
        // Add this node to the tail of its parent's child list.
        TAILQ_INSERT_TAIL(&parent->psn_children, node, psn_next);

        // Propagate the PSN_FLAG_PROCESS and PSN_FLAG_THREAD flags downward.
        node->psn_flags |= (parent->psn_flags & (PSN_FLAG_PROCESS | PSN_FLAG_THREAD));
    }
    return node;
}

/*
 * Adds a directory node to the file system structure. Since all directories
 * must have "." and ".." entries, these are added here by a recursive call
 * with the raw argument set to 1 to avoid infinite recursion.
 */
STATIC pfssnode_t *
add_directory(pfssnode_t *parent, const char *name, pfstype type, pfsbaseid_t node_id, uint16_t flags, boolean_t raw,
                procfs_node_size_fn node_size_fn, procfs_read_data_fn node_read_data_fn)
{
    // Add the directory node.
    pfssnode_t *snode = add_node(parent, name, type, node_id, flags, 0, node_size_fn, node_read_data_fn);

    // Add the "." and ".." directory entries, preserving the flags that indicate whether
    // the node is process- and/or thread-specific. The "raw" argument is used to stop
    // this being a recursive process.
    if (!raw) {
        add_directory(snode, ".", PFSdirthis, next_node_id++, flags & (PSN_FLAG_PROCESS | PSN_FLAG_THREAD), 1, NULL, NULL);
        add_directory(snode, "..", PFSdirparent, next_node_id++, flags & (PSN_FLAG_PROCESS | PSN_FLAG_THREAD), 1, NULL, NULL);
    }
    return snode;
}

/*
 * Adds a file to the file system structure. Files are always leaf
 * elements (although that is not checked).
 */
STATIC pfssnode_t *
add_file(pfssnode_t *parent, const char *name, pfsbaseid_t node_id, uint16_t flags, size_t size,
            procfs_node_size_fn node_size_fn, procfs_read_data_fn node_read_data_fn)
{
    return add_node(parent, name, PFSfile, node_id, flags, size, node_size_fn, node_read_data_fn);
}

#pragma mark -
#pragma mark Clean up of Structure Nodes

/*
 * Removes a node from the file system structure and releases its
 * memory. This happens only when the last instance of the file
 * system is unmounted.
 */
void
release_node(pfssnode_t *snode)
{
    // Remove from its parent's children list, if it has one.
    if (snode->psn_parent != NULL) {
        TAILQ_REMOVE(&snode->psn_parent->psn_children, snode, psn_next);
    }

    // Release all child nodes.
    pfssnode_t *child = TAILQ_FIRST(&snode->psn_children);
    while (child != NULL) {
        TAILQ_REMOVE(&snode->psn_children, child, psn_next);
        release_node(child);
        child = TAILQ_FIRST(&snode->psn_children);
    }

    // Free this node's memory.
    OSFree(snode, sizeof(pfssnode_t), procfs_osmalloc_tag);
}
