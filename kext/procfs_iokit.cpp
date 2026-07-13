/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_iokit.cpp
 *
 * IOKit block-device enumeration for the partitions node. Linux's
 * /proc/partitions lists every block device (whole disks and partitions,
 * mounted or not); on macOS that information lives in the IORegistry, reachable
 * only through the C++ IOKit runtime (the C IOKit KPI exposes no registry
 * matching). This is the kext's one C++ translation unit; it exposes a single
 * C-linkage entry point that the C partitions node calls.
 *
 * We match the "IOMedia" class by name and read properties off the base
 * IORegistryEntry, so no dependency on IOStorageFamily's IOMedia C++ class (and
 * its metaclass) is needed - only the base IOKit and libkern KPIs, both already
 * declared in Info.plist.
 */
#include <IOKit/IOService.h>

#include <libkern/c++/OSObject.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/libkern.h>

#include <sys/errno.h>

#include <fs/procfs/procfs_iokit.h>

/* IORegistry property keys (stable IOKit strings; hardcoded to avoid pulling in
 * IOStorageFamily / IOBSD headers). */
#define PROCFS_IOMEDIA_CLASS    "IOMedia"
#define PROCFS_KEY_BSD_NAME     "BSD Name"
#define PROCFS_KEY_BSD_MAJOR    "BSD Major"
#define PROCFS_KEY_BSD_MINOR    "BSD Minor"
#define PROCFS_KEY_SIZE         "Size"
#define PROCFS_KEY_WHOLE        "Whole"

/* IOBlockStorageDriver "Statistics" sub-dictionary keys (IOBlockStorageDriver.h;
 * hardcoded for the same reason). Times are in nanoseconds. */
#define PROCFS_KEY_STATISTICS   "Statistics"
#define PROCFS_STAT_READS       "Operations (Read)"
#define PROCFS_STAT_WRITES      "Operations (Write)"
#define PROCFS_STAT_BYTES_READ  "Bytes (Read)"
#define PROCFS_STAT_BYTES_WRITE "Bytes (Write)"
#define PROCFS_STAT_TIME_READ   "Total Time (Read)"
#define PROCFS_STAT_TIME_WRITE  "Total Time (Write)"

extern "C" int
procfs_iokit_get_partitions(struct procfs_partition *out, int max, int *count)
{
    if (out == nullptr || count == nullptr || max <= 0) {
        return EINVAL;
    }
    *count = 0;

    OSDictionary *match = IOService::serviceMatching(PROCFS_IOMEDIA_CLASS);
    if (match == nullptr) {
        return ENOMEM;
    }

    /* getMatchingServices consumes no reference to `match`; release it after. */
    OSIterator *it = IOService::getMatchingServices(match);
    match->release();
    if (it == nullptr) {
        return EIO;
    }

    int n = 0;
    OSObject *obj;
    while (n < max && (obj = it->getNextObject()) != nullptr) {
        IORegistryEntry *entry = OSDynamicCast(IORegistryEntry, obj);
        if (entry == nullptr) {
            continue;
        }

        OSString *name = OSDynamicCast(OSString, entry->getProperty(PROCFS_KEY_BSD_NAME));
        if (name == nullptr) {
            continue;                       /* no BSD device - skip */
        }

        OSNumber *major = OSDynamicCast(OSNumber, entry->getProperty(PROCFS_KEY_BSD_MAJOR));
        OSNumber *minor = OSDynamicCast(OSNumber, entry->getProperty(PROCFS_KEY_BSD_MINOR));
        OSNumber *size  = OSDynamicCast(OSNumber, entry->getProperty(PROCFS_KEY_SIZE));

        struct procfs_partition *p = &out[n];
        strlcpy(p->name, name->getCStringNoCopy(), sizeof(p->name));
        p->major = major != nullptr ? major->unsigned32BitValue() : 0;
        p->minor = minor != nullptr ? minor->unsigned32BitValue() : 0;
        p->size  = size  != nullptr ? size->unsigned64BitValue()  : 0;
        n++;
    }
    it->release();

    *count = n;
    return 0;
}

/* Read a uint64 value out of an OSDictionary by key, or 0 if absent. */
static uint64_t
procfs_stat_u64(OSDictionary *stats, const char *key)
{
    OSNumber *n = OSDynamicCast(OSNumber, stats->getObject(key));

    return n != nullptr ? n->unsigned64BitValue() : 0;
}

extern "C" int
procfs_iokit_get_diskstats(struct procfs_diskstat *out, int max, int *count)
{
    int n = 0;

    if (out == nullptr || count == nullptr || max <= 0) {
        return EINVAL;
    }

    *count = 0;

    OSDictionary *match = IOService::serviceMatching(PROCFS_IOMEDIA_CLASS);
    if (match == nullptr) {
        return ENOMEM;
    }
    OSIterator *it = IOService::getMatchingServices(match);
    match->release();
    if (it == nullptr) {
        return EIO;
    }

    OSObject *obj;
    while (n < max && (obj = it->getNextObject()) != nullptr) {
        IOService *media = OSDynamicCast(IOService, obj);
        if (media == nullptr) {
            continue;
        }

        /* diskstats is per whole disk (disk0), not per partition. */
        OSBoolean *whole = OSDynamicCast(OSBoolean, media->getProperty(PROCFS_KEY_WHOLE));
        if (whole == nullptr || !whole->isTrue()) {
            continue;
        }

        OSString *name = OSDynamicCast(OSString, media->getProperty(PROCFS_KEY_BSD_NAME));
        if (name == nullptr) {
            continue;
        }

        /* The whole-disk IOMedia's provider is the IOBlockStorageDriver, which
         * publishes the cumulative I/O "Statistics" dictionary. */
        IOService *drv = media->getProvider();
        OSDictionary *stats = (drv != nullptr)
            ? OSDynamicCast(OSDictionary, drv->getProperty(PROCFS_KEY_STATISTICS))
            : nullptr;

        if (stats == nullptr) {
            /* no stats (e.g. synthesized media) */
            continue;
        }

        OSNumber *major = OSDynamicCast(OSNumber, media->getProperty(PROCFS_KEY_BSD_MAJOR));
        OSNumber *minor = OSDynamicCast(OSNumber, media->getProperty(PROCFS_KEY_BSD_MINOR));

        struct procfs_diskstat *d = &out[n];
        strlcpy(d->name, name->getCStringNoCopy(), sizeof(d->name));
        d->major = major != nullptr ? major->unsigned32BitValue() : 0;
        d->minor = minor != nullptr ? minor->unsigned32BitValue() : 0;
        d->reads          = procfs_stat_u64(stats, PROCFS_STAT_READS);
        d->writes         = procfs_stat_u64(stats, PROCFS_STAT_WRITES);
        d->read_sectors   = procfs_stat_u64(stats, PROCFS_STAT_BYTES_READ) / 512;
        d->write_sectors  = procfs_stat_u64(stats, PROCFS_STAT_BYTES_WRITE) / 512;
        d->read_ticks_ms  = procfs_stat_u64(stats, PROCFS_STAT_TIME_READ) / 1000000;
        d->write_ticks_ms = procfs_stat_u64(stats, PROCFS_STAT_TIME_WRITE) / 1000000;
        n++;
    }
    it->release();

    *count = n;

    return 0;
}
