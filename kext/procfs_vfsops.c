/*
 * Copyright (c) 2015 Kim Topley
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_vfsops.c
 *
 * VFS operations for the ProcFS file system.
 */
#include <kern/locks.h>
#include <libkern/libkern.h>
#include <libkext/libkext.h>
#include <libkern/OSAtomic.h>
#include <libkern/OSMalloc.h>
#include <libkext/libkext.h>
#include <sys/mount.h>
#include <sys/systm.h>
#include <sys/vnode.h>

#include <fs/procfs/procfs.h>

#pragma mark Local Definitions

// The fixed mounted device name for this file system. The first
// instance is called "proc", the second is "proc2" and so on.
#define MOUNTED_DEVICE_NAME "proc"

// Block size for this file system. A meaningless value.
#define BLOCK_SIZE 4096

// The numer of hash buckets required. This *MUST* be
// a power of two.
#define HASH_BUCKET_COUNT (1 << 6)

// Each separate mount of the file system requires a unique id,
// which is also used by every node in the file system. This is
// equivalent to the dev_t associated with a real file system.
STATIC int32_t procfs_mount_id = 0;

#pragma mark -
#pragma mark External References

/* -- External references -- */
// Vnode ops descriptor for this file system.
extern struct vnodeopv_desc *procfs_vnodeops_list[1];

// Pointer to the constructed vnode operations vector. Set
// when the file system is registered and used when creating
// vnodes.
extern int (**procfs_vnodeop_p)(void *);

// Initialization routine. Only called once during the kext
// start routine in procfs.c - Included here as an external
// reference for the procfs_vfsops structure.
extern int procfs_init(struct vfsconf *vfsconf);

#pragma mark -
#pragma mark Function Prototypes

STATIC int procfs_mount(struct mount *mp, vnode_t devvp, user_addr_t data, vfs_context_t context);
STATIC int procfs_unmount(struct mount *mp, int mntflags, vfs_context_t context);
STATIC int procfs_root(struct mount *mp, struct vnode **vpp, vfs_context_t context);
STATIC int procfs_getattr(struct mount *mp, struct vfs_attr *fsap, vfs_context_t context);

STATIC void populate_statfs_info(struct mount *mp, struct vfsstatfs *statfsp);
STATIC void populate_vfs_attr(struct mount *mp, struct vfs_attr *fsap);
STATIC int procfs_create_root_vnode(mount_t mp, pfsnode_t *pnp, vnode_t *vpp);

#pragma mark -
#pragma mark VFS Operations and Entry Structures

vfstable_t procfs_vfs_table_ref;

/*
 * VFS OPS structure maps VFS-level operations to
 * the functions that implement them, all of which
 * are in this file.
 */
struct vfsops procfs_vfsops = {
    .vfs_mount          = procfs_mount,
    .vfs_unmount        = procfs_unmount,
    .vfs_root           = procfs_root,
    .vfs_getattr        = procfs_getattr,
    .vfs_init           = procfs_init,
};

struct vfs_fsentry procfs_vfsentry = {
    .vfe_vfsops         = &procfs_vfsops,
    .vfe_vopcnt         = ARRAY_SIZE(procfs_vnodeops_list),
    .vfe_opvdescs       = procfs_vnodeops_list,
    .vfe_fstypenum      = 0,
    .vfe_fsname         = "procfs",
    .vfe_flags          = PROCFS_VFS_FLAGS
};

#pragma mark -
#pragma mark Static Data

/* Number of mounted instances of procfs */
STATIC int mounted_instance_count = 0;

#pragma mark -
#pragma mark VFS Operations

/* --- VFS OPERATIONS --- */

/*
 * Performs the mount operation for the procfs file system. Gets the options passed to the
 * mount(2) system call from user space, allocates a procfs_mount_t structure, initializes
 * it and links it to the system's mount structure. On the first mount, the file system
 * node structure is created and file system initialization is completed.
 *
 * NOTE: mounts and unmounts are serialized by the mnt_rwlock in the VFS mount structure, so we do
 * not need to make this code reentrant or worry about being mounted and unmounted at the same time.
 */
STATIC int
procfs_mount(struct mount *mp, __unused vnode_t devvp, user_addr_t data, __unused vfs_context_t context)
{
    pfsmount_t *procfs_mp = MPTOPMP(mp);
    if (procfs_mp == NULL) {
        // First mount. Get the mount options from user space.
        pfsmount_args_t mount_args;
        mount_args.mnt_options = 0;  // Default: procperms enabled.
        if (data != USER_ADDR_NULL) {
            int error = copyin(data, &mount_args, sizeof(mount_args));
            if (error != 0) {
                printf("procfs: failed to copyin mount options, using defaults\n");
                mount_args.mnt_options = 0;
            }
        }

        // Allocate the procfs mount structure and link it to the VFS structure.
        procfs_mp = OSMalloc(sizeof(pfsmount_t), procfs_osmalloc_tag);
        if (procfs_mp == NULL) {
            printf("procfs: Failed to allocate pfsmount_t");
            return ENOMEM;
        }

        OSAddAtomic(1, &procfs_mount_id);
        procfs_mp->pmnt_id = procfs_mount_id;
        procfs_mp->pmnt_mp = mp;
        nanotime(&procfs_mp->pmnt_mount_time);
        vfs_setfsprivate(mp, procfs_mp);

        // Install procfs-specific flags and augment the generic mount flags.
        // NOT MNT_RDONLY: procfs has writable nodes (the per-process "note"), and
        // the VFS layer rejects every write on a read-only mount before it can
        // reach vnop_write. Non-writable nodes still reject writes themselves
        // (procfs_vnop_write returns EROFS/EISDIR; their modes carry no write bit).
        vfs_setflags(mp, MNT_NOSUID|MNT_NOEXEC|MNT_NODEV|MNT_NOATIME|MNT_LOCAL);

        // Increment the mounted instance count so that each mount of the file system
        // has a unique name as seen by the mount(1) command.
        OSAddAtomic(1, &mounted_instance_count);

        // Set up the statfs structure in the VFS mount with mostly
        // boilerplate default values.
        struct vfsstatfs *statfsp = vfs_statfs(mp);
        populate_statfs_info(mp, statfsp);

        // Complete setup of procfs data. Does nothing after first mount.
        procfs_structure_init();

        // Initialize static data that is only required after an instance of the file
        // system has been mounted.
        lck_mtx_lock(pfsnode_hash_mutex);
        if (pfsnode_hash_buckets == NULL) {
            // Set up the hash buckets only on first mount. Rather than define a
            // a new BSD zone, we use the existing zone M_CACHE.
            pfsnode_hash_buckets = hashinit(HASH_BUCKET_COUNT, M_CACHE, &pfsnode_hash_to_bucket_mask);
        }
        lck_mtx_unlock(pfsnode_hash_mutex);
    }

    return 0;
}

/*
 * Performs file system unmount. Clears out any cached vnodes, forcing reclaim, disconnects the
 * file system's pfsmount_t structure from the system mount structure and releases it.
 *
 * NOTE: mounts and unmounts are serialized by the mnt_rwlock in the VFS mount structure, so we do
 * not need to make this code reentrant or worry about being mounted and unmounted at the same time.
 */
STATIC int
procfs_unmount(struct mount *mp, __unused int mntflags, __unused vfs_context_t context)
{
    pfsmount_t *procfs_mp = MPTOPMP(mp);
    if (procfs_mp != NULL) {
        // We are currently mounted. Release resources and disconnect.

        // Flush out cached vnodes.
        vflush(mp, NULLVP, FORCECLOSE);

        vfs_setfsprivate(mp, NULL);
        OSFree(procfs_mp, sizeof(pfsmount_t), procfs_osmalloc_tag);

        if (procfs_mp != NULL) {
            procfs_mp = NULL;
        }

        // Decrement mounted instance count.
        OSAddAtomic(-1, &mounted_instance_count);
    }
    procfs_structure_free();

    return 0;
}

/*
 * Gets the root vnode for the file system. If the vnode has already been
 * created, it may be still be in the cache. If not, or if this is the
 * first call to this function after mount, the root vnode and its
 * accompanying pfsnode_t are created and added to the cache.
 */
STATIC int
procfs_root(struct mount *mp, vnode_t *vpp, __unused vfs_context_t context)
{
    vnode_t root_vnode;
    pfsnode_t *root_pfsnode;

    // Find the root vnode in the cache, or create it if it does not exist.
    int error = procfsnode_find(MPTOPMP(mp), PROCFS_ROOT_NODE_ID, procfs_structure_root_node(),
                                &root_pfsnode, &root_vnode,
                                (create_vnode_func)&procfs_create_root_vnode, mp);

    // Return the root vnode pointer to the caller, if it was created.
    *vpp = error == 0 ? root_vnode : NULLVP;

    return error;
}

/*
 * Implementation of the VFS_GETATTR() function for the procfs file system.
 * The vfs_attr structure is populated with values that have meaning for 
 * procfs. Most of them are dummy values and none of them change once the
 * file system has been mounted.
 */
STATIC int
procfs_getattr(struct mount *mp, struct vfs_attr *fsap, __unused vfs_context_t context)
{
    populate_vfs_attr(mp, fsap);
    return 0;
}

#pragma mark -
#pragma mark Root Vnode Creation

/*
 * Creates the root vnode for an instance of the file system and
 * links it to its pfsnode_t. No internal locks are held when this
 * function is called.
 */
STATIC int
procfs_create_root_vnode(mount_t mp, pfsnode_t *pnp, vnode_t *vpp)
{
    struct vnode_fsparam vnode_create_params;

    memset(&vnode_create_params, 0, sizeof(vnode_create_params));
    vnode_create_params.vnfs_mp = mp;
    vnode_create_params.vnfs_vtype = VDIR;
    vnode_create_params.vnfs_str = "procfs root vnode";
    vnode_create_params.vnfs_dvp = NULLVP;
    vnode_create_params.vnfs_fsnode = pnp;
    vnode_create_params.vnfs_vops = procfs_vnodeop_p;
    vnode_create_params.vnfs_markroot = 1;
    vnode_create_params.vnfs_flags = VNFS_CANTCACHE;

    // Create the vnode, if possible.
    vnode_t root_vnode;
    int error = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &vnode_create_params, &root_vnode);

    // Return the root vnode pointer to the caller, if it was created.
    *vpp = error == 0 ? root_vnode : NULLVP;

    return error;
}

#pragma mark -
#pragma mark File System Attributes

/*
 * Initializes a vfsstatfs structure with values that are
 * appropriate for a given mount of this file system. Most
 * values are fixed because this structure has limited meaning
 * for this file system.
 */
STATIC void
populate_statfs_info(struct mount *mp, struct vfsstatfs *statfsp)
{
    statfsp->f_bsize = BLOCK_SIZE;
    statfsp->f_iosize = BLOCK_SIZE;
    statfsp->f_blocks = 0;
    statfsp->f_bfree = 0;
    statfsp->f_bavail = 0;
    statfsp->f_bused = 0;
    statfsp->f_files = 0;
    statfsp->f_ffree = 0;

    // Compose fsid_t from the mount point id and the file system
    // type number, which was assigned when the file system was
    // registered. This pair of values just has to be unique.
    statfsp->f_fsid.val[0] = MPTOPMP(mp)->pmnt_id;
    statfsp->f_fsid.val[1] = vfs_typenum(mp);

    bzero(statfsp->f_mntfromname, sizeof(statfsp->f_mntfromname));
    if (mounted_instance_count == 1) {
        // First mount -- just use the base name.
        bcopy(MOUNTED_DEVICE_NAME, statfsp->f_mntfromname, strlen(MOUNTED_DEVICE_NAME));
    } else {
        // Subsequent mounts have the instance count + 1 added to the name.
        snprintf(statfsp->f_mntfromname, sizeof(statfsp->f_mntfromname) - 1,
                 "%s%d", MOUNTED_DEVICE_NAME, mounted_instance_count);
    }
}

/*
 * Populates a vfs_attr structure with values that are appropriate
 * for this file system. As with the vfsstatfs structure, most of the
 * files of the vfs_attr do not have any meaning for procfs.
 */
STATIC void
populate_vfs_attr(struct mount *mp, struct vfs_attr *fsap)
{
    struct vfsstatfs *statfsp = vfs_statfs(mp);
    pfsmount_t *procfs_mp = MPTOPMP(mp);

    VFSATTR_RETURN(fsap, f_objcount, 0);
    VFSATTR_RETURN(fsap, f_filecount, 0);
    VFSATTR_RETURN(fsap, f_dircount, 0);
    VFSATTR_RETURN(fsap, f_maxobjcount, 0);
    VFSATTR_RETURN(fsap, f_bsize, BLOCK_SIZE);
    VFSATTR_RETURN(fsap, f_iosize, BLOCK_SIZE);
    VFSATTR_RETURN(fsap, f_blocks, 0);
    VFSATTR_RETURN(fsap, f_bfree, 0);
    VFSATTR_RETURN(fsap, f_bavail, 0);
    VFSATTR_RETURN(fsap, f_bused, 0);
    VFSATTR_RETURN(fsap, f_files, 0);
    VFSATTR_RETURN(fsap, f_ffree, 0);
    VFSATTR_RETURN(fsap, f_fsid, statfsp->f_fsid);
    VFSATTR_RETURN(fsap, f_owner, statfsp->f_owner);
    VFSATTR_RETURN(fsap, f_create_time, procfs_mp->pmnt_mount_time);
    VFSATTR_RETURN(fsap, f_modify_time, procfs_mp->pmnt_mount_time);
    VFSATTR_RETURN(fsap, f_access_time, procfs_mp->pmnt_mount_time);
}
