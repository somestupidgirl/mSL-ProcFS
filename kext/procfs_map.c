/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_map.c
 *
 * Process virtual-memory map enumeration and the NetBSD-format node:
 *   /proc/<pid>/map   - NetBSD procfs format (procfs_domap, here)
 *   /proc/<pid>/maps  - Linux format (procfs_domaps, in procfs_linux.c)
 * References:
 *   https://github.com/NetBSD/src/blob/trunk/sys/miscfs/procfs/procfs_map.c
 *   Linux Documentation/filesystems/proc.rst (/proc/<pid>/maps)
 *
 * All map/maps/smaps nodes share the region walk in procfs_map_walk() and differ
 * only in the per-region formatter. macOS exports no region-enumeration KPI a
 * third-party kext may link, and the internal walkers are stripped from the
 * arm64 kernel. Rather than resolve get_task_map()/mach_vm_region() from the
 * kernelcache via libklookup, the enumeration is delegated to the procfsd
 * daemon: it opens the target with task_for_pid() and walks the regions with a
 * userspace mach_vm_region() (PROCFS_REQ_MAPS), returning raw region records.
 * The kext keeps all node formatting here and only consumes the records. The
 * cost is coverage: task_for_pid() is denied for SIP/AMFI-protected and
 * hardened-runtime targets, so those report EPERM (like /proc/<pid>/regs).
 *
 * Backing-file paths (the trailing column on Linux) are not resolved: that
 * needs the region's memory object -> vnode, which is not reachable here.
 */
#include <stdint.h>
#include <libkern/libkern.h>
#include <mach/vm_prot.h>
#include <mach/vm_region.h>
#if defined(__x86_64__)
#include <mach/i386/vm_param.h>
#else
#include <mach/vm_param.h>
#endif
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/proc_internal.h>
#include <sys/sbuf.h>
#include <sys/types.h>
#include <sys/uio.h>

#include <fs/procfs/procfs.h>
#include <fs/procfs/procfs_ctl.h>

/*
 * Fetch the target's VM regions from the procfsd daemon and invoke `cb` once per
 * region. The daemon returns the regions at or above a resume address; we start
 * at 0 and re-request with the end of the last region until an empty reply. On
 * the first request a failure is reported: ENOTSUP when no daemon is connected,
 * else the daemon's errno (EPERM for a protected target, ESRCH if it exited). A
 * failure partway through a walk truncates the result rather than discarding it.
 */
typedef void (*procfs_region_cb_t)(const struct procfs_map_region *r, void *arg);

static int
procfs_map_walk(pid_t pid, procfs_region_cb_t cb, void *arg)
{
    struct procfs_map_region regs[PROCFS_CTL_MAXPAYLOAD / sizeof(struct procfs_map_region)];
    uint64_t  addr  = 0;
    boolean_t first = TRUE;

    for (;;) {
        uint32_t got = 0;
        int rc = procfs_ctl_request(PROCFS_REQ_MAPS, pid, addr,
                                    regs, sizeof(regs), &got);
        if (rc != 0) {
            if (!first) {
                return 0;   /* partial result already emitted */
            }
            if (rc == ENOTCONN || rc == ETIMEDOUT) {
                return ENOTSUP;     /* no daemon connected */
            }
            return rc;              /* EPERM (protected), ESRCH, ... */
        }

        uint32_t n = got / (uint32_t)sizeof(regs[0]);
        if (n == 0) {
            return 0;               /* end of the walk */
        }
        for (uint32_t i = 0; i < n; i++) {
            cb(&regs[i], arg);
        }

        uint64_t last_end = regs[n - 1].start + regs[n - 1].size;
        if (last_end <= addr) {
            return 0;               /* no forward progress - stop */
        }
        addr  = last_end;
        first = FALSE;
    }
}

/*
 * map/maps walk: build a struct procfs_region for each region and hand it to the
 * node's formatter.
 */
struct procfs_map_render_ctx {
    struct sbuf           *sb;
    procfs_region_fmt_fn   fmt;
};

static void
procfs_map_render_cb(const struct procfs_map_region *mr, void *arg)
{
    struct procfs_map_render_ctx *c = arg;
    struct procfs_region r = {
        .start    = mr->start,
        .end      = mr->start + mr->size,
        .offset   = mr->offset,
        .prot     = (int)mr->prot,
        .max_prot = (int)mr->max_prot,
        .shared   = (int)mr->shared,
        .wired    = mr->user_wired,
    };
    c->fmt(c->sb, &r);
}

/*
 * Shared region walk for the map/maps nodes. Enumerates the process's VM regions
 * and invokes `fmt` once per region to append a formatted line.
 */
int
procfs_map_render(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx, procfs_region_fmt_fn fmt)
{
    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p == PROC_NULL) {
        return ESRCH;
    }

    int error = procfs_check_can_access_process(vfs_context_ucred(ctx), p);
    if (error != 0) {
        proc_rele(p);
        return error;
    }

    /* A large address space can have thousands of regions, so grow on demand. */
    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 4096, SBUF_AUTOEXTEND) == NULL) {
        proc_rele(p);
        return ENOMEM;
    }

    struct procfs_map_render_ctx rctx = { &sb, fmt };
    error = procfs_map_walk(pnp->node_id.nodeid_pid, procfs_map_render_cb, &rctx);
    proc_rele(p);
    if (error != 0) {
        sbuf_delete(&sb);
        return error;
    }

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * Virtual and (footprint-approximating) resident size accumulator.
 */
struct procfs_map_sizes_ctx {
    uint64_t vsize;
    uint64_t rsize;
};

static void
procfs_map_sizes_cb(const struct procfs_map_region *mr, void *arg)
{
    struct procfs_map_sizes_ctx *c = arg;
    c->vsize += mr->size;

    /*
     * Approximate proc_pidinfo's pti_resident_size, which is the task's
     * phys_footprint - private memory, excluding pages shared with other tasks
     * (the dyld shared cache, shared libraries). pages_resident counts shared
     * pages too, so only count regions that are not shared.
     */
    switch (mr->share_mode) {
    case SM_SHARED:
    case SM_TRUESHARED:
    case SM_SHARED_ALIASED:
        break;      /* shared with other tasks - not part of footprint */
    default:
        c->rsize += (uint64_t)mr->resident_pages * PAGE_SIZE;
        break;
    }
}

/*
 * Sum the task's virtual size and resident size from its VM regions. This is the
 * offset-free way to obtain proc_taskinfo's pti_virtual_size / pti_resident_size
 * on arm64, where fill_taskprocinfo, task_info and pmap_resident_count are all
 * stripped. Caller holds the proc_find() reference.
 */
int
procfs_task_vm_sizes(proc_t p, uint64_t *vsize, uint64_t *rsize)
{
    *vsize = 0;
    *rsize = 0;

    struct procfs_map_sizes_ctx c = { 0, 0 };
    int error = procfs_map_walk(proc_pid(p), procfs_map_sizes_cb, &c);
    if (error != 0) {
        return error;
    }
    *vsize = c.vsize;
    *rsize = c.rsize;
    return 0;
}

/*
 * smaps-family walk: build a struct procfs_ext_region (extended per-region page
 * counts and share mode) for each region and hand it to the caller's callback.
 */
struct procfs_map_ext_ctx {
    procfs_ext_region_fn cb;
    void                *arg;
};

static void
procfs_map_ext_cb(const struct procfs_map_region *mr, void *arg)
{
    struct procfs_map_ext_ctx *c = arg;
    struct procfs_ext_region r = {
        .start          = mr->start,
        .end            = mr->start + mr->size,
        .prot           = (int)mr->prot,
        .resident_pages = mr->resident_pages,
        .dirty_pages    = mr->dirty_pages,
        .swapped_pages  = mr->swapped_pages,
        .shared         = (mr->share_mode == SM_SHARED ||
                           mr->share_mode == SM_TRUESHARED ||
                           mr->share_mode == SM_SHARED_ALIASED),
        .anonymous      = (mr->external_pager == 0),
    };
    c->cb(&r, c->arg);
}

/*
 * Shared VM-region walk exposing the extended per-region page counts and share
 * mode. The Linux smaps-family nodes (smaps / smaps_rollup / numa_maps) in
 * procfs_linux.c build their text from this; it produces no output of its own.
 * Returns 0 or an errno.
 */
int
procfs_map_foreach_ext(pfsnode_t *pnp, vfs_context_t ctx,
    procfs_ext_region_fn cb, void *arg)
{
    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p == PROC_NULL) {
        return ESRCH;
    }

    int error = procfs_check_can_access_process(vfs_context_ucred(ctx), p);
    if (error != 0) {
        proc_rele(p);
        return error;
    }

    struct procfs_map_ext_ctx ectx = { cb, arg };
    error = procfs_map_walk(pnp->node_id.nodeid_pid, procfs_map_ext_cb, &ectx);
    proc_rele(p);
    return error;
}

/*
 * NetBSD-style formatter: start-end curprot maxprot sharing wired.
 */
static void
procfs_map_fmt_netbsd(struct sbuf *sb, const struct procfs_region *r)
{
    sbuf_printf(sb, "%#018llx %#018llx %c%c%c %c%c%c %s %u\n",
        (unsigned long long)r->start, (unsigned long long)r->end,
        (r->prot & VM_PROT_READ)        ? 'r' : '-',
        (r->prot & VM_PROT_WRITE)       ? 'w' : '-',
        (r->prot & VM_PROT_EXECUTE)     ? 'x' : '-',
        (r->max_prot & VM_PROT_READ)    ? 'r' : '-',
        (r->max_prot & VM_PROT_WRITE)   ? 'w' : '-',
        (r->max_prot & VM_PROT_EXECUTE) ? 'x' : '-',
        r->shared ? "share" : "priv", r->wired);
}

/*
 * "map" node - NetBSD procfs format.
 */
int
procfs_domap(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx)
{
    return procfs_map_render(pnp, uio, ctx, procfs_map_fmt_netbsd);
}
