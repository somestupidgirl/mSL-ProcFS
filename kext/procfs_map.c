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
 * Both nodes share the region walk in procfs_map_render() and differ only in
 * the per-region formatter. The walk uses mach_vm_region(), resolved from the
 * on-disk kernel collection via libklookup along with get_task_map(): macOS
 * exports no region-enumeration KPI a third-party kext may link, and the
 * internal walkers (vm_map_region/vm_map_lookup_entry) are stripped from the
 * arm64 kernel. mach_vm_region() takes the map's read lock internally, so this
 * needs no struct-walking or manual locking. For VM_REGION_BASIC_INFO_64 the
 * call sets object_name to IP_NULL, so there is no port reference to release.
 *
 * Backing-file paths (the trailing column on Linux) are not resolved: that
 * needs the region's memory object -> vnode, which is not reachable here.
 */
#include <stdint.h>
#include <string.h>
#include <libkern/libkern.h>
#include <mach/kern_return.h>
#include <mach/mach_types.h>
#include <mach/vm_types.h>
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
#include <sys/vnode.h>
#include <ptrauth.h>

#include <bsdcompat/sys/malloc.h>

#include <fs/procfs/procfs.h>

#include <libkprocfs/symbols.h>

/*
 * Resolved-symbol function-pointer types. The mach_vm_region() prototype uses
 * the "user typed" (_ut) wrappers, but those are transparent_unions that are
 * ABI-identical to the plain mach_vm_offset_t/mach_vm_size_t, so a plain-typed
 * pointer is calling-convention correct. object_name is taken as void * so the
 * kernel's 8-byte IP_NULL write lands in a pointer-sized slot regardless of how
 * mach_port_t is typedef'd here.
 */
typedef vm_map_t (*procfs_get_task_map_fn)(task_t task);
typedef kern_return_t (*procfs_mach_vm_region_fn)(vm_map_t map,
    mach_vm_offset_t *address, mach_vm_size_t *size, int flavor,
    int *info, mach_msg_type_number_t *count, void *object_name);

/* Guard against a pathological walk (mach_vm_region terminates with a non-
 * success return at the top of the address space anyway). */
#define PROCFS_MAP_MAX_REGIONS 1000000

/*
 * Shared region walk for the map/maps nodes. Enumerates the process's VM
 * regions and invokes `fmt` once per region to append a formatted line.
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

    if (procfs_kl_get_task_map == NULL || procfs_kl_mach_vm_region == NULL) {
        proc_rele(p);
        return ENOTSUP;     /* libklookup did not resolve the VM symbols */
    }

    /* Sign the resolved addresses for the arm64e ABI (disc 0; the assignment
     * resigns to each call's type discriminator). */
    procfs_get_task_map_fn   get_task_map   =
        ptrauth_sign_unauthenticated(procfs_kl_get_task_map, ptrauth_key_function_pointer, 0);
    procfs_mach_vm_region_fn mach_vm_region =
        ptrauth_sign_unauthenticated(procfs_kl_mach_vm_region, ptrauth_key_function_pointer, 0);

    task_t   task = proc_task(p);
    vm_map_t map  = (task != TASK_NULL) ? get_task_map(task) : NULL;
    if (map == NULL) {
        proc_rele(p);
        return EIO;
    }

    /* A large address space can have thousands of regions, so grow on demand. */
    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 4096, SBUF_AUTOEXTEND) == NULL) {
        proc_rele(p);
        return ENOMEM;
    }

    mach_vm_offset_t addr = 0;
    for (int n = 0; n < PROCFS_MAP_MAX_REGIONS; n++) {
        mach_vm_size_t                 size  = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t         count = VM_REGION_BASIC_INFO_COUNT_64;
        void                          *object_name = NULL;  /* 8-byte slot; gets IP_NULL */

        kern_return_t kr = mach_vm_region(map, &addr, &size, VM_REGION_BASIC_INFO_64,
            (int *)&info, &count, &object_name);
        if (kr != KERN_SUCCESS) {
            break;          /* no more regions at or above addr */
        }

        struct procfs_region r = {
            .start    = (uint64_t)addr,
            .end      = (uint64_t)(addr + size),
            .offset   = (uint64_t)info.offset,
            .prot     = info.protection,
            .max_prot = info.max_protection,
            .shared   = info.shared,
            .wired    = info.user_wired_count,
        };
        fmt(&sb, &r);

        mach_vm_offset_t next = addr + size;
        if (next <= addr) {
            break;          /* wrapped at the top of the address space */
        }
        addr = next;
    }

    proc_rele(p);

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * Sum the task's virtual size and resident size by walking its VM regions with
 * VM_REGION_EXTENDED_INFO (which carries pages_resident per region). This is the
 * offset-free way to obtain proc_taskinfo's pti_virtual_size / pti_resident_size
 * on arm64, where fill_taskprocinfo, task_info and pmap_resident_count are all
 * stripped. Caller holds the proc_find() reference.
 */
int
procfs_task_vm_sizes(proc_t p, uint64_t *vsize, uint64_t *rsize)
{
    *vsize = 0;
    *rsize = 0;

    if (procfs_kl_get_task_map == NULL || procfs_kl_mach_vm_region == NULL) {
        return ENOTSUP;
    }

    procfs_get_task_map_fn   get_task_map   =
        ptrauth_sign_unauthenticated(procfs_kl_get_task_map, ptrauth_key_function_pointer, 0);
    procfs_mach_vm_region_fn mach_vm_region =
        ptrauth_sign_unauthenticated(procfs_kl_mach_vm_region, ptrauth_key_function_pointer, 0);

    task_t   task = proc_task(p);
    if (task == TASK_NULL) {
        return ESRCH;
    }
    vm_map_t map = get_task_map(task);
    if (map == NULL) {
        return EIO;
    }

    mach_vm_offset_t addr = 0;
    for (int n = 0; n < PROCFS_MAP_MAX_REGIONS; n++) {
        mach_vm_size_t                    size  = 0;
        vm_region_extended_info_data_t    info;
        mach_msg_type_number_t            count = VM_REGION_EXTENDED_INFO_COUNT;
        void                             *object_name = NULL;  /* gets IP_NULL */

        kern_return_t kr = mach_vm_region(map, &addr, &size, VM_REGION_EXTENDED_INFO,
            (int *)&info, &count, &object_name);
        if (kr != KERN_SUCCESS) {
            break;
        }

        *vsize += (uint64_t)size;

        /*
         * Approximate proc_pidinfo's pti_resident_size, which is the task's
         * phys_footprint (ledger phys_mem) - private memory, excluding pages
         * shared with other tasks (the dyld shared cache, shared libraries).
         * EXTENDED_INFO's pages_resident counts shared pages too, which would
         * over-report by ~10x, so only count regions that are not shared. The
         * exact ledger value is unreachable (ledger_get_balance is stripped).
         */
        switch (info.share_mode) {
        case SM_SHARED:
        case SM_TRUESHARED:
        case SM_SHARED_ALIASED:
            break;      /* shared with other tasks - not part of footprint */
        default:
            *rsize += (uint64_t)info.pages_resident * PAGE_SIZE;
            break;
        }

        mach_vm_offset_t next = addr + size;
        if (next <= addr) {
            break;
        }
        addr = next;
    }

    return 0;
}

/*
 * "smaps" node - Linux /proc/<pid>/smaps. For each VM region, the maps header
 * line followed by per-region memory detail. Uses VM_REGION_EXTENDED_INFO for
 * resident / dirtied / swapped page counts and the region's share mode.
 *
 * Fields with no macOS source are approximated: Pss and Referenced track Rss
 * (true proportional-set-size and reference-bit data are unavailable), the
 * region's share_mode classifies its whole Rss as shared or private, and the
 * file offset is 0 (VM_REGION_EXTENDED_INFO does not carry it). This is a
 * best-effort smaps, consistent with the rest of the VM nodes on Apple Silicon.
 */
int
procfs_dosmaps(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx)
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

    if (procfs_kl_get_task_map == NULL || procfs_kl_mach_vm_region == NULL) {
        proc_rele(p);
        return ENOTSUP;
    }
    procfs_get_task_map_fn   get_task_map   =
        ptrauth_sign_unauthenticated(procfs_kl_get_task_map, ptrauth_key_function_pointer, 0);
    procfs_mach_vm_region_fn mach_vm_region =
        ptrauth_sign_unauthenticated(procfs_kl_mach_vm_region, ptrauth_key_function_pointer, 0);

    task_t   task = proc_task(p);
    vm_map_t map  = (task != TASK_NULL) ? get_task_map(task) : NULL;
    if (map == NULL) {
        proc_rele(p);
        return EIO;
    }

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 8192, SBUF_AUTOEXTEND) == NULL) {
        proc_rele(p);
        return ENOMEM;
    }

    const uint64_t pgkb = (uint64_t)PAGE_SIZE / 1024;   /* page size in kB */

    mach_vm_offset_t addr = 0;
    for (int n = 0; n < PROCFS_MAP_MAX_REGIONS; n++) {
        mach_vm_size_t                 size  = 0;
        vm_region_extended_info_data_t info;
        mach_msg_type_number_t         count = VM_REGION_EXTENDED_INFO_COUNT;
        void                          *object_name = NULL;

        kern_return_t kr = mach_vm_region(map, &addr, &size, VM_REGION_EXTENDED_INFO,
            (int *)&info, &count, &object_name);
        if (kr != KERN_SUCCESS) {
            break;
        }

        boolean_t shared = (info.share_mode == SM_SHARED ||
                            info.share_mode == SM_TRUESHARED ||
                            info.share_mode == SM_SHARED_ALIASED);

        uint64_t size_kb  = (uint64_t)size / 1024;
        uint64_t rss_kb   = (uint64_t)info.pages_resident    * pgkb;
        uint64_t dirty_kb = (uint64_t)info.pages_dirtied     * pgkb;
        uint64_t swap_kb  = (uint64_t)info.pages_swapped_out * pgkb;
        if (dirty_kb > rss_kb) {
            dirty_kb = rss_kb;
        }
        uint64_t clean_kb = rss_kb - dirty_kb;
        /* No external pager => anonymous memory. */
        uint64_t anon_kb  = (info.external_pager == 0) ? rss_kb : 0;

        /* maps-format header line (offset unavailable from extended info). */
        sbuf_printf(&sb, "%016llx-%016llx %c%c%c%c %016llx 00:00 0 \n",
            (unsigned long long)(uint64_t)addr,
            (unsigned long long)(uint64_t)(addr + size),
            (info.protection & VM_PROT_READ)    ? 'r' : '-',
            (info.protection & VM_PROT_WRITE)   ? 'w' : '-',
            (info.protection & VM_PROT_EXECUTE) ? 'x' : '-',
            shared ? 's' : 'p',
            0ULL);

        sbuf_printf(&sb,
            "Size:           %8llu kB\n"
            "KernelPageSize: %8llu kB\n"
            "MMUPageSize:    %8llu kB\n"
            "Rss:            %8llu kB\n"
            "Pss:            %8llu kB\n"
            "Shared_Clean:   %8llu kB\n"
            "Shared_Dirty:   %8llu kB\n"
            "Private_Clean:  %8llu kB\n"
            "Private_Dirty:  %8llu kB\n"
            "Referenced:     %8llu kB\n"
            "Anonymous:      %8llu kB\n"
            "Swap:           %8llu kB\n",
            (unsigned long long)size_kb,
            (unsigned long long)pgkb, (unsigned long long)pgkb,
            (unsigned long long)rss_kb, (unsigned long long)rss_kb,
            (unsigned long long)(shared ? clean_kb : 0),
            (unsigned long long)(shared ? dirty_kb : 0),
            (unsigned long long)(shared ? 0 : clean_kb),
            (unsigned long long)(shared ? 0 : dirty_kb),
            (unsigned long long)rss_kb,
            (unsigned long long)anon_kb,
            (unsigned long long)swap_kb);

        sbuf_printf(&sb, "VmFlags:%s%s%s%s\n",
            (info.protection & VM_PROT_READ)    ? " rd" : "",
            (info.protection & VM_PROT_WRITE)   ? " wr" : "",
            (info.protection & VM_PROT_EXECUTE) ? " ex" : "",
            shared ? " sh" : "");

        mach_vm_offset_t next = addr + size;
        if (next <= addr) {
            break;
        }
        addr = next;
    }

    proc_rele(p);

    sbuf_finish(&sb);
    error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

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
