/*
 * Copyright (c) 2015 Kim Topley
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs.h
 *
 * Definitions for the procfs file system. The content of this
 * header is available to both user-level and kernel components.
 */
#ifndef procfs_h
#define procfs_h

#pragma mark -
#pragma mark Definitions for both user-level and kernel components

/* Lock group name */
#define PROCFS_LCKGRP_NAME      BUNDLEID_S ".lckgrp"

/* Mount option flags. */
#define PROCFS_MOPT_NOPROCPERMS (1 << 0) // Do not apply process permissions to the pid entries in /proc.
#define MOPT_PROCFS             { "procperms", 1, PROCFS_MOPT_NOPROCPERMS, 0 }

#define PROCFS_NOTELEN      64  /* max length of a note (/proc/$pid/note) */
#define PROCFS_MAXNAMLEN    255

/*
 * The procfs mount structure, created by mount_procfs
 * and passed to the kernel by the mount(2) system call.
 */
typedef struct pfsmount_args {
    int mnt_options;      // The procfs mount options.
} pfsmount_args_t;

#pragma mark -
#pragma mark Internel Definitions - Kernel Only

#ifndef __FSBUNDLE__

#include <kern/locks.h>
#include <libkern/OSMalloc.h>
#include <libkext/libkext.h>
#include <mach/task.h>
#include <sys/kernel_types.h>
#include <sys/mount.h>
#include <sys/queue.h>
#include <sys/vnode.h>

#pragma mark -
#pragma mark External References

// Lock used to protect the hash table.
extern lck_grp_t *pfsnode_lck_grp;
extern lck_mtx_t *pfsnode_hash_mutex;

// Tag used for memory allocation.
extern OSMallocTag procfs_osmalloc_tag;

// The buckets for the pfsnode hash table. The number of buckets
// is always a power of two.
extern LIST_HEAD(procfs_hash_head, pfsnode) *pfsnode_hash_buckets;

// The mask used to get the bucket number from a pfsnode hash.
extern u_long pfsnode_hash_to_bucket_mask;

#pragma mark -
#pragma mark Type Definitions

/*
 * The different types of node in a procfs filesystem
 */
typedef enum {
    PFSroot,        /* the filesystem root */
    PFSproc,        /* a process-specific sub-directory */
    PFSthread,      /* the thread dir */
    PFSdir,         /* an ordinary directory */
    PFSfile,        /* a file */
    PFSdirthis,     /* representation of "." */
    PFSdirparent,   /* representation of ".." */
    PFScurproc,     /* symbolic link for curproc */
    PFSprocnamedir, /* directory for a process labeled with its command line */
    PFSfd,          /* a directory containing the processes open fd's */
    PFScpuinfo,     /* Linux-compatible /proc/cpuinfo */
    PFSloadavg,     /* Linux-compatible /proc/loadavg */
    PFSpartitions,  /* Linux-compatible /proc/partitions */
    PFSversion,     /* Linux-compatible /proc/version */
    PFSmeminfo,     /* Linux-compatible /proc/meminfo */
    PFSmtab,        /* Linux-compatible /proc/mtab */
    PFSstat,        /* Linux-compatible /proc/stat */
    PFSvmstat,      /* Linux-compatible /proc/vmstat */
    PFSuptime,      /* Linux-compatible /proc/uptime */
    PFSswaps,       /* Linux-compatible /proc/swaps */
    PFSfilesystems, /* Linux-compatible /proc/filesystems */
    PFSproclink,    /* per-process symlink: exe/cwd/root (target by node name) */
    PFSsysctl,      /* dynamic /proc/sys node (objectid = struct sysctl_oid *, 0 = root) */
    PFSextensions,  /* macOS-style loaded kernel-extension listing (root) */
    PFSmodules,     /* Linux-style loaded kernel-module listing (root) */
    PFSdiskstats,   /* Linux-style block-device I/O statistics (root) */
    PFSkcmdline,    /* Linux-style kernel boot command line (root /proc/cmdline) */
} pfstype;

typedef struct pfsnode pfsnode_t;
typedef struct pfsid pfsid_t;
typedef struct pfsmount pfsmount_t;
typedef struct pfssnode pfssnode_t;
typedef struct procfs_hash_head procfs_hash_head;

// Callback function used to create vnodes, called from within the
// procfsnode_find() function. "params" is used to pass the details that
// the function needs in order to create the correct vnode. It is obtained
// from the "create_vnode_params" argument passed to procfsnode_find(),
// "pnp" is a pointer to the pfsnode_t that the vnode should be linked to
// and "vpp" is where the created vnode will be stored, if the call was successful.
// Returns 0 on success or an error code (from errno.h) if not.
typedef int (*create_vnode_func)(void *params, pfsnode_t *pnp, vnode_t *vpp);

// Type for the base node id field of a structure node.
typedef uint16_t pfsbaseid_t;

// Type of a function that reports the size for a procfs node.
typedef size_t (*procfs_node_size_fn)(pfsnode_t *pnp, kauth_cred_t creds);

// Type of a function that reads the data for a procfs node.
typedef int (*procfs_read_data_fn)(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);

#pragma mark -
#pragma mark Common Definitions

// VFS flags
#define PROCFS_VFS_FLAGS  ( \
        VFS_TBL64BITREADY       | \
        VFS_TBLFSNODELOCK       | \
        VFS_TBLLOCALVOL         | \
        VFS_TBLNOTYPENUM        | \
        VFS_TBLNOMACLABEL       | \
        VFS_TBLREADDIR_EXTENDED | \
        0                         \
)

// Bit values for the psn_flags field.
#define PSN_FLAG_PROCESS    (1 << 0)
#define PSN_FLAG_THREAD     (1 << 1)

// Special values for the nodeid_pid and nodeid_objectid fields.
#define PRNODE_NO_PID       ((int)-1)
#define PRNODE_NO_OBJECTID  ((uint64_t)0)

// Root node id value.
#define PROCFS_ROOT_NODE_BASE_ID ((pfsbaseid_t)1)

// Largest name of a structure node.
#define MAX_STRUCT_NODE_NAME_LEN 16

#pragma mark -
#pragma mark Structure Definitions

/*
 * Definitions for the data structures that determine the
 * layout of nodes in the procfs file system.
 * The layout is constructed by building a tree of
 * structures of type pfssnode_t. The layout
 * is the same for each file system instance and is created
 * when the first instance of the file system is mounted.
 */

/*
 * An entry in the procfs file system layout. All fields of this
 * structure are set on creation and do not change, so no locking
 * is required to access them.
 *
 * The psn_node_type field is the type of the structure node. These types
 * are mapped to the usual vnode types by the file system when getting
 * node attributes and are used during node lookup and other vnode operations.
 *
 * The psn_name field is the name that should be used for the node in the
 * file system. For nodes of type PROCFS_PROCDIR and PROCFS_PROCNAME_DIR,
 * the process id of the associated process is used and for PROCFS_THREADDIR, 
 * the associated thread's id is used.
 *
 * The psn_base_node_id field is a unique value that becomes part of the
 * full id of any pfsnode_t that is created from this structure node.
 * 
 * The PSN_FLAG_PROCESS and PSN_FLAG_THREAD flag values of a node are propagated
 * to all descendent nodes, so it is always possible to determine whether a
 * node is process- and/or thread-related just by examining the psn_flags
 * field of its pfssnode.
 */
struct pfssnode {
    pfstype                           psn_node_type;
    char                              psn_name[MAX_STRUCT_NODE_NAME_LEN];
    pfsbaseid_t                       psn_base_node_id;   // Base node id - unique.
    uint16_t                          psn_flags;          // Flags - PSN_XXX (see below)

    // Structure linkage. Immutable once set.
    struct pfssnode                  *psn_parent;                          // The parent node in the structure
    TAILQ_ENTRY(pfssnode)             psn_next;                            // Next sibling node within structure parent.
    TAILQ_HEAD(pfschildren, pfssnode) psn_children; // Children of this structure node.

    // --- Function hooks. Set to null to use the defaults.
    // The node's size value. This is the size value for the node itself.
    // For directory nodes, the sum of the size values of all of its children is
    // used as the actual size, so this value has meaning only for nodes of type
    // PFSfile. It is not used if the procfs_node_size_fn field is set.
    size_t                            psn_node_size;

    // Gets the value for the node's size attribute. If NULL, psn_node_size
    // is used instead.
    procfs_node_size_fn               psn_getsize_fn;

    // Reads the file content.
    procfs_read_data_fn               psn_read_data_fn;
};

/*
 * Composite identifier for a node in the procfs file system.
 * There must only ever be one node for each unique identifier
 * in any given instance of the file system (i.e. per mount).
 */
struct pfsid {
    int           nodeid_pid;         // The owning process, or PRNODE_NO_PID if not process-linked
    uint64_t      nodeid_objectid;    // The owning object within the process, or PRNODE_NO_OBJECTID if none.
    pfsbaseid_t   nodeid_base_id;     // The id of the structure node to which this node is linked.
};

/*
 * Procfs per-moount data structure. Typically, there is only one
 * instance of this file system, but the implementation does not
 * preclude multiple mounts.
 */
struct pfsmount {
    int32_t           pmnt_id;            // A unique identifier for this mount. Shared by all nodes.
    int               pmnt_flags;         // Flags, set from the mount command (PROCFS_MOPT_XXX).
    struct mount     *pmnt_mp;            // VFS-level mount structure.
    struct timespec   pmnt_mount_time;    // Time at which the file system was mounted.
};

/*
 * The filesystem-dependent vnode private data for procfs.
 * There is one insance of this structure for each active node.
 */
struct pfsnode {
    // Linkage for the node hash. Protected by the node hash lock.
    LIST_ENTRY(pfsnode) node_hash;

    // Pointer to the associated vnode. Protected by the node hash lock.
    vnode_t             node_vnode;

    // Records whether this node is currently being attached to a vnode.
    // Only one thread can be allowed to link the node to a vnode. If a
    // thread that wants to create a pfsnode and link it to a vnode
    // finds this field set to true, it must release the node hash lock
    // and wait until the field is reset to false, then check again whether
    // some or all of the work that it needed to do has been completed.
    // Protected by the node hash lock.
    boolean_t           node_attaching_vnode;

    // Records whether a thread is awaiting the outcome of vnode attachment.
    // Protected by the node hash lock.
    boolean_t           node_thread_waiting_attach;

    // node_mnt_id and node_id taken together uniquely identify a node. There
    // must only ever be one procnfsnode instance (and hence one vnode) for each
    // (node_mnt_id, node_id) combination. The node_mnt_id value can be obtained
    // from the pmnt_id field of the pfsmount structure for the owning mount.
    int32_t             node_mnt_id;            // Identifier of the owning mount.
    pfsid_t             node_id;                // The identifer of this node.

    // Pointer to the pfssnode_t for this node.
    pfssnode_t         *node_structure_node;    // Set when allocated, never changes.
};

/*
 * Structure used to keep track of pid collection.
 */
struct procfs_pidlist_data
{
    int             num_procs;
    int             max_procs;
    kauth_cred_t    creds;     // Credential to use for access check, or NULL
    int            *pids;
};

#pragma mark -
#pragma mark Macros

#define CNEQ(cnp, s, len) \
     ((cnp)->cn_namelen == (len) && \
      (memcmp((s), (cnp)->cn_nameptr, (len)) == 0))

#define PROCFS_FILENO(pid, type, fd) \
    (((type) < PFSproc) ? ((type) + 2) : \
    (((fd) == -1) ? ((((pid)+1) << 5) + ((int) (type))) : \
    ((((pid)+1) << 16) | ((fd) << 5) | ((int) (type)))))

#define PROCFS_TYPE(type)   ((type) & 0x1f)

/*
 * Convert between pfsmp vfsmp.
 */
#define MPTOPMP(mp)   vfs_mp_to_procfs_mp(mp)
#define PMPTOMP(pmp)   procfs_mp_to_vfs_mp(pmp)

/*
 * Convert between pfsnode vnode.
 */
#define VTOPFS(vp)      vnode_to_procfsnode(vp)
#define PFSTOV(pfs)     pfsnode_to_vnode(pfs)

/*
 * Zero out vnode attributes.
 */
#define VATTR_NULL(vap) bzero(vap, sizeof(struct vnode_attr))

#pragma mark -
#pragma mark Inline Conversion Functions

/* Convert from procfs vnode pointer to VFS vnode pointer. */
static inline vnode_t
pfsnode_to_vnode(pfsnode_t *pnp)
{
    return pnp->node_vnode;
}

/* Convert from VFS vnode pointer to procfs vnode pointer. */
static inline pfsnode_t *
vnode_to_procfsnode(vnode_t vp)
{
    return (pfsnode_t *)vnode_fsnode(vp);
}

/* Convert from procfs mount pointer to VFS mount pointer. */
static inline struct mount *
procfs_mp_to_vfs_mp(pfsmount_t *pmp)
{
    return pmp->pmnt_mp;
}

/* Convert from VFS mount pointer to procfs mount pointer. */
static inline pfsmount_t *
vfs_mp_to_procfs_mp(struct mount *vmp)
{
    return(pfsmount_t *)vfs_fsprivate(vmp);
}

#pragma mark -
#pragma mark Inline Convenience Functions

/* Returns whether access checks should apply to the vnodes on a given mount point. */
static inline boolean_t
procfs_should_access_check(pfsmount_t *pmp)
{
    return (pmp->pmnt_flags & PROCFS_MOPT_NOPROCPERMS) == 0;
}

/* Returns whether a given node type represents a directory. */
static inline boolean_t
procfs_is_directory_type(pfstype type)
{
    return type != PFScurproc && type != PFSloadavg
        && type != PFSfile && type != PFScpuinfo
        && type != PFSpartitions && type != PFSversion
        && type != PFSmeminfo && type != PFSmtab
        && type != PFSstat && type != PFSvmstat
        && type != PFSuptime && type != PFSproclink
        && type != PFSswaps && type != PFSfilesystems
        && type != PFSsysctl && type != PFSextensions
        && type != PFSmodules && type != PFSdiskstats
        && type != PFSkcmdline;
}

/* Gets the pid_t for the process corresponding to a pfsnode_t. */
static inline int
procfsnode_to_pid(pfsnode_t *pfsnode)
{
    return pfsnode->node_id.nodeid_pid;
}

#pragma mark -
#pragma mark Global Definitions

/* Identifier for the root node of the file system. */
extern const pfsid_t PROCFS_ROOT_NODE_ID;

/* Public API */
extern int procfsnode_find(pfsmount_t *pmp, pfsid_t node_id, pfssnode_t *snode, pfsnode_t **pnpp,
                           vnode_t *vnpp, create_vnode_func create_vnode_func, void *create_vnode_params);
extern void procfsnode_free_node(pfsnode_t *pfsnode);
extern void procfs_get_parent_node_id(pfsnode_t *pnp, pfsid_t *idp);

/* Gets the root node of the file system structure. */
extern pfssnode_t *procfs_structure_root_node(void);

/*
 * Initializes the procfs structures. Should only be called
 * while mounting a file system.
 */
extern void procfs_structure_init(void);

/*
 * Frees the memory for the procfs structures. Should only be called
 * while unmounting the last instance of the file system.
*/
extern void procfs_structure_free(void);

/* 
 * Allocate a pfsnode/vnode pair. Gets the vnode type that is appropriate
 * for a given structure node type.
 */
extern enum vtype procfs_allocvp(pfstype);

/*
 * Copies data from the local buffer "data" into the area described
 * by a uio structure. The first byte of "data" is assumed to 
 * correspond to a zero offset, so if the uio structure has its
 * uio_offset set to N, the first byte of data that will be copied
 * is at data[N].
 */
extern int procfs_copy_data(const char *data, int data_len, uio_t uio);

/* Functions that copy pfsnode_t data to a buffer described by a uio_t structure. */
extern int procfs_dopid(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_doppid(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_dopgid(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_dosid(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_dotty(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_dostatus(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_dotaskinfo(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_dothreadinfo(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_dofd(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_dosocket(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_doprocargs(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
/* Shared reader of the flattened argument region: buffer + argv/env/apple offsets
 * (used by cmdline, environ and the native auxv node). Caller holds proc_find. */
extern int procfs_read_procargs(proc_t p, uint8_t **bufp, size_t *lenp,
        size_t *argv_off, size_t *env_off, size_t *apple_off);
extern int procfs_domem(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);

/*
 * One VM region, as handed to a map formatter. Plain scalars so formatters in
 * either file need no Mach VM headers; `prot`/`max_prot` carry VM_PROT_* bits.
 */
struct sbuf;
struct procfs_region {
    uint64_t     start;
    uint64_t     end;
    uint64_t     offset;
    int          prot;
    int          max_prot;
    int          shared;
    unsigned int wired;
};
typedef void (*procfs_region_fmt_fn)(struct sbuf *sb, const struct procfs_region *r);

/* Shared VM-region walk (procfs_map.c): enumerates the process's regions and
 * calls `fmt` per region. procfs_domap formats NetBSD-style here; procfs_domaps
 * (Linux-style) lives in procfs_linux.c and reuses this. */
extern int procfs_map_render(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx,
                             procfs_region_fmt_fn fmt);
/* Sum a task's virtual and resident sizes via the VM-region walk (procfs_map.c);
 * the offset-free source for proc_taskinfo's size fields on arm64. */
extern int procfs_task_vm_sizes(proc_t p, uint64_t *vsize, uint64_t *rsize);

/* Kernel-control bridge to the procfsd daemon (procfs_ctl.c). */
extern kern_return_t procfs_ctl_register(void);
extern void          procfs_ctl_deregister(void);
extern int           procfs_ctl_request(uint32_t type, int pid, uint64_t arg,
                                         void *out, uint32_t outcap, uint32_t *outlen);
extern int           procfs_ctl_request_named(uint32_t type, int pid, uint64_t arg,
                                         const char *name, void *out, uint32_t outcap,
                                         uint32_t *outlen);
struct sbuf;
extern int           procfs_ctl_request_blob(uint32_t type, struct sbuf *sb);
extern int           procfs_dokextlist(uint32_t type, uio_t uio);
extern int           procfs_doextensions(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int           procfs_domodules(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int           procfs_dodiskstats(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int           procfs_dokcmdline(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_domap(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_domaps(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);

/* Process machine state (NetBSD-style binary register dumps) and the auxiliary
 * vector (XNU's apple[] array). */
extern int procfs_doregs(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_dofpregs(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_doauxv(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
/* Linux-compat-mode text variants, selected at runtime by procfs_linux_mode. */
extern int procfs_doregs_linux(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_dofpregs_linux(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_doauxv_linux(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);

/*
 * Presentation mode: 0 = native (BSD/XNU), 1 = Linux-compatible. Toggled live
 * from userspace via the `procfs.linux` sysctl. Nodes that have both a native
 * and a Linux rendering (regs/fpregs/auxv) dispatch on it.
 */
extern int  procfs_linux_mode;
extern void procfs_sysctl_register(void);
extern void procfs_sysctl_unregister(void);

/* Linux-compatible per-thread files (/proc/<pid>/task/<tid>/). */
extern int procfs_dothreadcomm(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_dothreadstat(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_dothreadstatus(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_dothreadsched(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);

/* Linux-compatible per-process text nodes. */
extern int procfs_doprocstatus_linux(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_docomm(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_dostatm(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_doprocstat(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_doenviron(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_douptime(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_doswaps(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_dofilesystems(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);

/* BSD/Linux-compatible features */
extern int procfs_docpuinfo(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_dolimit(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_doloadavg(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_domeminfo(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_domtab(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_dostat(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_dovmstat(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern void procfs_loadavg_start(void);
extern void procfs_loadavg_stop(void);
extern int procfs_dopartitions(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_doversion(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);
extern int procfs_donote(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx);

/* Functions that return the data size for a node. */
extern size_t procfs_get_node_size_attr(pfsnode_t *pnp, kauth_cred_t creds);
extern size_t procfs_process_node_size(pfsnode_t *pnp, kauth_cred_t creds);
extern size_t procfs_thread_node_size(pfsnode_t *pnp, kauth_cred_t creds);
extern size_t procfs_fd_node_size(pfsnode_t *pnp, kauth_cred_t creds);

/* Subroutine functions. */
extern boolean_t procfs_node_type_has_pid(pfstype node_type);
extern int procfs_get_process_info(vnode_t vp, pid_t *pidp, proc_t *procp);
extern uint64_t procfs_get_node_fileid(pfsnode_t *pnp);
extern uint64_t procfs_get_fileid(pid_t pid, uint64_t objectid, pfsbaseid_t base_id);
extern int procfs_atoi(const char *p, const char **end_ptr);
extern void procfs_get_pids(pid_t **pidpp, int *pid_count, uint32_t *sizep, kauth_cred_t creds);
extern void procfs_release_pids(pid_t *pidp, uint32_t size);
extern int procfs_get_thread_ids_for_task(proc_t p, uint64_t **thread_ids, int *thread_count);
extern void procfs_release_thread_ids(uint64_t *thread_ids, int thread_count);
struct proc_fdinfo;
extern int procfs_get_fd_list(proc_t p, struct proc_fdinfo **fdlist, size_t *count);
extern int procfs_proclink_path(int pid, const char *name, char *buf, int buflen);

/* Dynamic /proc/sys sysctl mirror (procfs_sysctl.c). objectid is a
 * struct sysctl_oid * (0 = the tree root). */
extern boolean_t procfs_sysctl_is_node(uint64_t objectid);
extern boolean_t procfs_sysctl_find(uint64_t dir_objectid, const char *name, uint64_t *out_objectid);
extern int       procfs_sysctl_child_at(uint64_t dir_objectid, int index,
                     const char **name, boolean_t *is_node, uint64_t *objectid);
extern int       procfs_sysctl_read(uint64_t objectid, uio_t uio);
extern boolean_t procfs_sysctl_parent(uint64_t objectid, uint64_t *parent_objectid);
extern void procfs_release_fd_list(struct proc_fdinfo *fdlist);
extern int procfs_check_can_access_process(kauth_cred_t creds, proc_t p);
extern int procfs_check_can_access_proc_pid(kauth_cred_t creds, pid_t pid);
extern int procfs_issuser(kauth_cred_t creds);
extern int procfs_get_process_count(kauth_cred_t creds);
extern int procfs_get_task_thread_count(proc_t p);

#endif /* __FSBUNDLE__ */

#endif /* procfs_h */
