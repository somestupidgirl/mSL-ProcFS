/*
 * Copyright (c) 2015 Kim Topley
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_node.c
 *
 * Functions for the management of vnodes and pfsnodes.
 */
#include <kern/assert.h>
#include <libkern/OSMalloc.h>
#include <sys/malloc.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/vnode.h>

#include <fs/procfs/procfs.h>

#pragma mark -
#pragma mark Global Definitions

/*
 * Node identifier for the root node of the file system.
 */
const pfsid_t PROCFS_ROOT_NODE_ID = {
    .nodeid_pid =       PRNODE_NO_PID,
    .nodeid_objectid =  PRNODE_NO_OBJECTID,
    .nodeid_base_id =   PROCFS_ROOT_NODE_BASE_ID
};

#pragma mark -
#pragma mark Hash table for procfs nodes

/*
 * The buckets for the pfsnode hash table. The number of buckets
 * is always a power of two.
 */
struct procfs_hash_head *pfsnode_hash_buckets;

/*
 * The mask used to get the bucket number from a pfsnode hash.
 */
u_long pfsnode_hash_to_bucket_mask;

/*
 * Lock used to protect the hash table.
 */
lck_grp_t *pfsnode_lck_grp = NULL;
lck_mtx_t *pfsnode_hash_mutex = NULL;

/*
 * Tag used for memory allocation.
 */
OSMallocTag procfs_osmalloc_tag = NULL;

/*
 * Macro that gets the header of the bucket that corresponds to a given
 * hash value.
 */
#define	PROCFS_NODE_HASH_TO_BUCKET_HEADER(pfsnode_hash) \
        (procfs_hash_head *)(&pfsnode_hash_buckets[(pfsnode_hash) & pfsnode_hash_to_bucket_mask])

/*
 * Gets the hash value for a given mount id and identifier.
 */
#define HASH_FOR_MOUNT_AND_ID(mount_id, node_id) (int)(((mount_id) << 16) ^ (node_id.nodeid_pid) ^ (node_id.nodeid_objectid) ^ (node_id.nodeid_base_id))

#pragma mark -
#pragma mark Management of vnodes and pfsnodes

/*
 * Finds the pfsnode_t for a node with a given id and referencing a given structure node
 * on a given instance of the file system. If the node does not already exist, it is created, 
 * entered into the node has table and a vnode is created and attached to it. If the node already 
 * exists, it is returned along with its vnode. In both cases, the vnode has an additional iocount,
 * which the caller must remove at some point by calling vnode_put().
 *
 * Creation of a vnode cannot be performed by this function because the information required 
 * to initialize it is known only to the caller. The caller must supply a pointer to a function
 * that will create the vnode when required, along with an opaque context pointer that is passed
 * to the creation function, along with a pointer to the corresponding pfsnode_t. The creation
 * function must either create the vnode and link it to the pfsnode_t or return an error.
 *
 * The allocation of the pfsnode_t that is done here is reversed in the procfs_reclaim() function,
 * which is called when the node's associated vnode is being reclaimed.
 */
int
procfsnode_find(pfsmount_t *pmp, pfsid_t node_id, pfssnode_t *snode,
                pfsnode_t **pnpp, vnode_t *vnpp,
                create_vnode_func create_vnode_func,
                void *create_vnode_params)
{
    int error = 0;

    boolean_t locked = TRUE;
    pfsnode_t *target_pfsnode = NULL;     /* This is the node that we will return. */
    pfsnode_t *new_pfsnode = NULL;        /* Newly allocated node. Will be freed if not used. */
    vnode_t target_vnode = NULL;          /* Start by assuming we will not get a vnode. */
    int32_t mount_id = pmp->pmnt_id;      /* File system id. */

    /*
     * Lock the hash table. We'll keep this locked until we are done,
     * unless we need to allocate memory. In that case, we'll drop the
     * lock, but we'll have to revisit all of our assumptions when we
     * reacquire it, because another thread may have created the node
     * we are looking for.
     */
    lck_mtx_lock(pfsnode_hash_mutex);

    boolean_t done = FALSE;
    while (!done) {
        assert(locked);
        error = 0;

        /*
         * Select the correct hash bucket and walk along it, looking for an existing
         * node with the correct attributes.
         */
        int nodehash = HASH_FOR_MOUNT_AND_ID(mount_id, node_id);
        procfs_hash_head *hash_bucket = PROCFS_NODE_HASH_TO_BUCKET_HEADER(nodehash);
        LIST_FOREACH(target_pfsnode, hash_bucket, node_hash) {
            if (target_pfsnode->node_mnt_id == mount_id
                    && target_pfsnode->node_id.nodeid_pid == node_id.nodeid_pid
                    &&target_pfsnode->node_id.nodeid_objectid == node_id.nodeid_objectid
                    && target_pfsnode->node_id.nodeid_base_id == node_id.nodeid_base_id) {
                /*
                 * Matched.
                 */
                break;
            }
        }
        
        /*
         * We got a match if target_pfsnode is not NULL.
         */
        if (target_pfsnode == NULL) {
            /*
             * We did not find a match, so either allocate a new node or use the
             * one we created last time around this loop.
             */
            if (new_pfsnode == NULL) {
                /*
                 * We need to allocate a new node. Before doing that, unlock
                 * the node hash, because the memory allocation may block.
                 */
                lck_mtx_unlock(pfsnode_hash_mutex);
                locked = FALSE;

                new_pfsnode = (pfsnode_t *)OSMalloc(sizeof(pfsnode_t), procfs_osmalloc_tag);
                if (new_pfsnode == NULL) {
                    /*
                     * Allocation failure - bail. Nothing to clean up and
                     * we don't hold the lock.
                     */
                    error = ENOMEM;
                    break;
                }

                /*
                 * We got a new pfsnode. Relock the node hash, then go around the
                 * loop again. This is necessary because someone else may have created
                 * the same node after we dropped the lock. If that's the case, we'll
                 * find that node next time around and we'll use it. The one we just
                 * allocated will remain in target_pfsnode and will be freed before we return.
                 */
                lck_mtx_lock(pfsnode_hash_mutex);
                locked = TRUE;
                continue;
            } else {
                /*
                 * If we get here, we know that we need to use the node that we
                 * allocated last time around the loop, so promote it to target_pfsnode
                 */
                assert(locked);
                assert(new_pfsnode != NULL);

                target_pfsnode = new_pfsnode;

                /*
                 * Initialize the new node.
                 */
                memset(target_pfsnode, 0, sizeof(pfsnode_t));
                target_pfsnode->node_mnt_id = mount_id;
                target_pfsnode->node_id = node_id;
                target_pfsnode->node_structure_node = snode;

                /*
                 * Add the node to the node hash. We already know which bucket
                 * it belongs to.
                 */
                LIST_INSERT_HEAD(hash_bucket, target_pfsnode, node_hash);
            }
        }

        /*
         * At this point, we have a pfsnode_t, which either already existed
         * or was just created. We also have the lock for the node hash table.
         */
        assert(target_pfsnode != NULL);
        assert(locked);

        /*
         * Check whether another thread is already in the process of creating a
         * vnode for this pfsnode_t. If it is, wait until it's done and go
         * around the loop again.
         */
        if (target_pfsnode->node_attaching_vnode) {
            /*
             * Indicate that a wakeup is needed when the attaching thread
             * is done.
             */
            target_pfsnode->node_thread_waiting_attach = TRUE;

            /*
             * Sleeping will drop and relock the mutex.
             */
            msleep(target_pfsnode, pfsnode_hash_mutex, PINOD, "procfsnode_find", NULL);

            /*
             * Since anything can have changed while we were away, go around
             * the loop again.
             */
            continue;
        }

        target_vnode = target_pfsnode->node_vnode;
        if (target_vnode != NULL) {
            /*
             * We already have a vnode. We need to check if it has been reassigned.
             * To do that, unlock and check the vnode id.
             */
            uint32_t vid = vnode_vid(target_vnode);
            lck_mtx_unlock(pfsnode_hash_mutex);
            locked = FALSE;

            error = vnode_getwithvid(target_vnode, vid);
            if (error != 0) {
                /*
                 * Vnode changed identity, so we need to redo everything. Relock
                 * because we are expected to hold the lock at the top of the loop.
                 * Getting here means that the vnode was reclaimed and the pfsnode
                 * was removed from the hash and freed, so we will be restarting from scratch.
                 */
                lck_mtx_lock(pfsnode_hash_mutex);
                target_pfsnode = NULL;
                new_pfsnode = NULL;
                locked = TRUE;
                continue;
            }

            /*
             * The vnode was still present and has not changed id. All we need to do
             * is terminate the loop. We don't hold the lock, "locked" is FALSE and
             * we don't need to relock (and indeed doing so would introduce yet more
             * race conditions). vnode_getwithvid() added an iocount reference for us,
             * which the caller is expected to eventually release with vnode_put().
             */
            assert(error == 0);
            break;
        }

        /*
         * At this point, we have pfsnode_t in the node hash, but we don't have a
         * vnode. To create the vnode, we have to release the node hash lock and invoke
         * the caller's create_vnode_func callback. Before doing that, we need to set
         * node_attaching_vnode to force any other threads that come in here to wait for
         * this thread to create the vnode (or fail).
         */
        target_pfsnode->node_attaching_vnode = TRUE;
        lck_mtx_unlock(pfsnode_hash_mutex);
        locked = FALSE;

        error = (*create_vnode_func)(create_vnode_params, target_pfsnode, &target_vnode);
        assert(error != 0 || target_vnode != NULL);

        /*
         * Relock the hash table and clear node_attaching_vnode now that we are
         * safely back from the caller's callback.
         */
        lck_mtx_lock(pfsnode_hash_mutex);
        locked = TRUE;
        target_pfsnode->node_attaching_vnode = FALSE;

        /*
         * If there are threads waiting for the vnode attach to complete,
         * wake them up.
         */
        if (target_pfsnode->node_thread_waiting_attach) {
            target_pfsnode->node_thread_waiting_attach = FALSE;
            wakeup(target_pfsnode);
        }

        /*
         * Now check whether we succeeded.
         */
        if (error != 0) {
            /*
             * Failed to create the vnode -- this is fatal.
             * Remove the pfsnode_t from the hash table and
             * release it.
             */
            procfsnode_free_node(target_pfsnode);

            if (target_pfsnode != NULL) {
                target_pfsnode = NULL;
            }
            if (new_pfsnode != NULL) {
                new_pfsnode = NULL; /* To avoid double free. */
            }
            break;
        }

        /*
         * We got the new vnode and it's already linked to the pfsnode_t.
         * Link the pfsnode_t to it. Also add a file system reference to
         * the vnode itself.
         */
        target_pfsnode->node_vnode = target_vnode;
        vnode_addfsref(target_vnode);

        break;
    }

    /*
     * Unlock the hash table, if it is still locked.
     */
    if (locked) {
        lck_mtx_unlock(pfsnode_hash_mutex);
    }

    /*
     * Free the node we allocated, if we didn't use it. We do this
     * after releasing the hash lock just in case it might block.
     */
    if (new_pfsnode != NULL && new_pfsnode != target_pfsnode) {
        OSFree(new_pfsnode, sizeof(pfsnode_t), procfs_osmalloc_tag);

        if (new_pfsnode != NULL) {
            new_pfsnode = NULL;
        }
    }

    /*
     * Set the return value, or NULL if we failed.
     */
    *pnpp = error == 0 ? target_pfsnode : NULL;
    *vnpp = error == 0 ? target_vnode : NULL;

    return error;
}

 /*
  * Removes a pfsnode_t from its owning hash bucket and
  * releases its memory. This method must be called with the
  * hash table lock held.
  */
void
procfsnode_free_node(pfsnode_t *pfsnode)
{
    LIST_REMOVE(pfsnode, node_hash);
    OSFree(pfsnode, sizeof(pfsnode_t), procfs_osmalloc_tag);
}

/*
 * Given a pfsnode_t, returns the procfs node id for the node
 * that would be the parent of the given node. If the node is the
 * root node, returns its own node id. The caller passes the pfsid_t
 * structure in which the node id is returned.
 */
void
procfs_get_parent_node_id(pfsnode_t *pnp, pfsid_t *return_idp)
{
    pfssnode_t *snode = pnp->node_structure_node;
    pfssnode_t *parent_snode = snode == NULL ? NULL : snode->psn_parent;
    if (parent_snode == NULL) {
        /*
         * The root node is effectively its parent.
         */
        parent_snode = snode;
    }

    /*
     * Set the fields of the return node_id from the base id of
     * the parent structure node, plus the process and thread ids
     * of the original node if the parent node is process- or
     * thread-related.
     */
    boolean_t pid_node = (parent_snode->psn_flags & PSN_FLAG_PROCESS) != 0;
    boolean_t thread_node = (parent_snode->psn_flags & PSN_FLAG_THREAD) != 0;

    return_idp->nodeid_base_id = parent_snode->psn_base_node_id;
    return_idp->nodeid_pid = pid_node ? pnp->node_id.nodeid_pid : PRNODE_NO_PID;
    return_idp->nodeid_objectid = thread_node ? pnp->node_id.nodeid_objectid : PRNODE_NO_OBJECTID;
}
