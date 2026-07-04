/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_sysctl.c
 *
 * Dynamic /proc/sys tree: a live mirror of the kernel's sysctl MIB, the macOS
 * counterpart to Linux's /proc/sys. Rather than static structure nodes, every
 * /proc/sys vnode is an instance of the single PFSsysctl structure node,
 * distinguished by its node id's objectid, which holds the kernel address of the
 * corresponding `struct sysctl_oid` (objectid 0 == the tree root,
 * `sysctl__children`). A NODE oid is a directory; a leaf is a file whose read
 * builds the dotted MIB name and fetches the value with sysctlbyname().
 *
 * This file owns all knowledge of the sysctl_oid internals; the vnops layer
 * drives it through the small interface declared in procfs.h.
 */
#include <stdint.h>
#include <string.h>

#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <libkern/libkern.h>

#include <fs/procfs/procfs.h>
#include <fs/procfs/procfs_ctl.h>

/* In-kernel sysctl-by-name (not prototyped in the kext's sysctl.h view). */
extern int sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
    void *newp, size_t newlen);

/* The children list for a directory objectid: the tree root (objectid 0) maps to
 * sysctl__children; a NODE oid maps to its child list (oid_arg1); anything else
 * (a leaf) has no children. */
static struct sysctl_oid_list *
procfs_sysctl_children(uint64_t objectid)
{
    if (objectid == 0) {
        return &sysctl__children;
    }
    struct sysctl_oid *oid = (struct sysctl_oid *)objectid;
    if ((oid->oid_kind & CTLTYPE) != CTLTYPE_NODE) {
        return NULL;
    }
    return (struct sysctl_oid_list *)oid->oid_arg1;
}

/* Internal oids not shown in the listing: nameless entries, deprecated/masked
 * variables, and the mutable-anchor markers (__anchor__(...)). */
static boolean_t
procfs_sysctl_hidden(struct sysctl_oid *oid)
{
    return oid->oid_name == NULL
        || (oid->oid_kind & CTLFLAG_MASKED) != 0
        || oid->oid_number == OID_MUTABLE_ANCHOR;
}

/* True if the node is a directory (the tree root, or a CTLTYPE_NODE oid). */
boolean_t
procfs_sysctl_is_node(uint64_t objectid)
{
    if (objectid == 0) {
        return TRUE;
    }
    struct sysctl_oid *oid = (struct sysctl_oid *)objectid;
    return (oid->oid_kind & CTLTYPE) == CTLTYPE_NODE;
}

/* Find a named child of a directory objectid; sets *out_objectid on success. */
boolean_t
procfs_sysctl_find(uint64_t dir_objectid, const char *name, uint64_t *out_objectid)
{
    struct sysctl_oid_list *list = procfs_sysctl_children(dir_objectid);
    if (list == NULL) {
        return FALSE;
    }
    struct sysctl_oid *oid;
    SLIST_FOREACH(oid, list, oid_link) {
        if (!procfs_sysctl_hidden(oid) && strcmp(oid->oid_name, name) == 0) {
            *out_objectid = (uint64_t)oid;
            return TRUE;
        }
    }
    return FALSE;
}

/*
 * Enumerate a directory's children by index (for readdir): fills *name,
 * *is_node and *objectid for the child at `index`. Returns 1 if present, 0 if
 * the index is past the end. The SLIST order is stable except across kext
 * load/unload, matching readdir's "content may change between calls" contract.
 */
int
procfs_sysctl_child_at(uint64_t dir_objectid, int index,
    const char **name, boolean_t *is_node, uint64_t *objectid)
{
    struct sysctl_oid_list *list = procfs_sysctl_children(dir_objectid);
    if (list == NULL) {
        return 0;
    }
    int i = 0;
    struct sysctl_oid *oid;
    SLIST_FOREACH(oid, list, oid_link) {
        if (procfs_sysctl_hidden(oid)) {
            continue;
        }
        if (i == index) {
            *name     = oid->oid_name;
            *is_node  = (oid->oid_kind & CTLTYPE) == CTLTYPE_NODE;
            *objectid = (uint64_t)oid;
            return 1;
        }
        i++;
    }
    return 0;
}

/* Recover the full dotted MIB name of `target` by DFS from the tree root, since
 * a sysctl_oid has no back-pointer to its parent oid. The tree is shallow, so
 * this is cheap enough for the (cold) read path. */
static boolean_t
procfs_sysctl_build_name(struct sysctl_oid_list *list, const char *prefix,
    uint64_t target, char *out, size_t outsz)
{
    struct sysctl_oid *oid;
    SLIST_FOREACH(oid, list, oid_link) {
        if (oid->oid_name == NULL) {
            continue;
        }
        char path[256];
        if (prefix[0] != '\0') {
            snprintf(path, sizeof(path), "%s.%s", prefix, oid->oid_name);
        } else {
            strlcpy(path, oid->oid_name, sizeof(path));
        }
        if ((uint64_t)oid == target) {
            strlcpy(out, path, outsz);
            return TRUE;
        }
        if ((oid->oid_kind & CTLTYPE) == CTLTYPE_NODE && oid->oid_arg1 != NULL) {
            if (procfs_sysctl_build_name((struct sysctl_oid_list *)oid->oid_arg1,
                    path, target, out, outsz)) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* DFS helper: find `target`'s parent oid, carrying the current parent id down
 * the recursion (0 for the tree root == /proc/sys). */
static boolean_t
procfs_sysctl_find_parent(struct sysctl_oid_list *list, uint64_t parent_id,
    uint64_t target, uint64_t *out_parent)
{
    struct sysctl_oid *oid;
    SLIST_FOREACH(oid, list, oid_link) {
        if ((uint64_t)oid == target) {
            *out_parent = parent_id;
            return TRUE;
        }
        if ((oid->oid_kind & CTLTYPE) == CTLTYPE_NODE && oid->oid_arg1 != NULL) {
            if (procfs_sysctl_find_parent((struct sysctl_oid_list *)oid->oid_arg1,
                    (uint64_t)oid, target, out_parent)) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

/*
 * Resolve the parent directory objectid of `objectid` for ".." traversal. Sets
 * *parent_objectid to the enclosing oid, or 0 when the parent is the /proc/sys
 * root (a top-level oid). Returns FALSE for the root itself (no sysctl parent),
 * leaving ".." to fall through to the /proc root.
 */
boolean_t
procfs_sysctl_parent(uint64_t objectid, uint64_t *parent_objectid)
{
    if (objectid == 0) {
        *parent_objectid = 0;
        return FALSE;
    }
    return procfs_sysctl_find_parent(&sysctl__children, 0, objectid, parent_objectid);
}

/*
 * Read a leaf's value as Linux-style text. Builds the MIB name, fetches the raw
 * value, and formats it by the oid's declared type (int / quad / string).
 * Opaque/struct sysctls have no text rendering and read empty. A directory
 * objectid returns EISDIR.
 *
 * The raw value comes from the procfsd bridge (userspace sysctlbyname, which
 * serves every oid), falling back to in-kernel sysctlbyname() when no daemon is
 * connected. The kernel path only serves oids marked CTLFLAG_KERN; others (e.g.
 * kern.hostname, kern.maxproc) read empty without the daemon. Invoking the oid
 * handler in-kernel to bypass that gate is unsafe — the custom handlers assume
 * the sysctl lock is held and panic — so the daemon is the path to full coverage.
 */
int
procfs_sysctl_read(uint64_t objectid, uio_t uio)
{
    if (procfs_sysctl_is_node(objectid)) {
        return EISDIR;
    }
    struct sysctl_oid *oid = (struct sysctl_oid *)objectid;

    char name[PROCFS_CTL_NAMEMAX];
    name[0] = '\0';
    if (!procfs_sysctl_build_name(&sysctl__children, "", objectid, name, sizeof(name))) {
        return ENOENT;
    }

    /*
     * When a Linux kernel version is being spoofed, the OS-identity sysctls
     * report a Linux identity so /proc/sys/kernel/{ostype,osrelease,version}
     * agree with /proc/version.
     */
    const char *rel = procfs_spoofed_release();
    if (rel != NULL) {
        char sbuf[512];
        if (strcmp(name, "kern.ostype") == 0) {
            return procfs_copy_data("Linux\n", 6, uio);
        }
        if (strcmp(name, "kern.osrelease") == 0) {
            int n = snprintf(sbuf, sizeof(sbuf), "%s\n", rel);
            return procfs_copy_data(sbuf, n, uio);
        }
        if (strcmp(name, "kern.version") == 0) {
            int n = procfs_build_linux_version(sbuf, sizeof(sbuf));
            return procfs_copy_data(sbuf, n, uio);
        }
    }

    uint8_t raw[1024];
    size_t  rawlen = 0;

    /* Prefer the daemon (covers all oids); fall back to the in-kernel path. */
    uint32_t dlen = 0;
    if (procfs_ctl_request_named(PROCFS_REQ_SYSCTL, 0, 0, name,
            raw, sizeof(raw), &dlen) == 0) {
        rawlen = dlen;
    } else {
        rawlen = sizeof(raw);
        if (sysctlbyname(name, raw, &rawlen, NULL, 0) != 0) {
            rawlen = 0;
        }
    }
    if (rawlen == 0) {
        return procfs_copy_data("", 0, uio);       /* unreadable/non-KERN/empty */
    }

    char out[1100];
    int  len  = 0;
    int  type = oid->oid_kind & CTLTYPE;
    const char *fmt = (oid->oid_fmt != NULL) ? oid->oid_fmt : "";

    switch (type) {
    case CTLTYPE_STRING: {
        if (raw[rawlen - 1] == '\0') {
            rawlen--;                              /* drop the trailing NUL */
        }
        len = snprintf(out, sizeof(out), "%.*s\n", (int)rawlen, (char *)raw);
        break;
    }
    case CTLTYPE_INT: {
        if (rawlen >= sizeof(int32_t)) {
            int32_t v;
            memcpy(&v, raw, sizeof(v));
            len = (fmt[0] == 'I' && fmt[1] == 'U')
                ? snprintf(out, sizeof(out), "%u\n", (uint32_t)v)
                : snprintf(out, sizeof(out), "%d\n", v);
        }
        break;
    }
    case CTLTYPE_QUAD: {
        if (rawlen >= sizeof(int64_t)) {
            int64_t v;
            memcpy(&v, raw, sizeof(v));
            len = (fmt[0] == 'Q' && fmt[1] == 'U')
                ? snprintf(out, sizeof(out), "%llu\n", (unsigned long long)v)
                : snprintf(out, sizeof(out), "%lld\n", (long long)v);
        }
        break;
    }
    default:
        return procfs_copy_data("", 0, uio);        /* opaque/struct: no text */
    }

    return procfs_copy_data(out, len, uio);
}
