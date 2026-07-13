/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_iokit.h
 *
 * Bridge between the C VFS code and the C++ IOKit helper (procfs_iokit.cpp),
 * which enumerates block devices for the partitions node. Included from both
 * languages.
 */
#ifndef _FS_PROCFS_PROCFS_IOKIT_H_
#define _FS_PROCFS_PROCFS_IOKIT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * One block device (IOMedia): a whole disk or a partition.
 */
struct procfs_partition {
    uint32_t major;         /* BSD major device number */
    uint32_t minor;         /* BSD minor device number */
    uint64_t size;          /* size in bytes */
    char     name[64];      /* BSD name, e.g. "disk0" / "disk0s1" */
};

/*
 * Enumerate block devices via IOKit (matching the "IOMedia" class), filling up
 * to `max` entries into `out` and setting *count. Returns 0 on success or an
 * errno on failure (the caller then falls back to the mounted-filesystem list).
 */
int procfs_iokit_get_partitions(struct procfs_partition *out, int max, int *count);

/*
 * Per-whole-disk I/O statistics (from IOBlockStorageDriver's "Statistics"),
 * for the Linux /proc/diskstats node. Times are in milliseconds; sector counts
 * are 512-byte sectors (byte totals / 512). Merged/in-flight/queue counters have
 * no macOS source and are reported as 0 by the formatter.
 */
struct procfs_diskstat {
    uint32_t major;
    uint32_t minor;
    char     name[64];      /* BSD name, e.g. "disk0" */
    uint64_t reads;         /* completed read operations */
    uint64_t read_sectors;  /* 512-byte sectors read */
    uint64_t read_ticks_ms; /* total time spent reading (ms) */
    uint64_t writes;        /* completed write operations */
    uint64_t write_sectors; /* 512-byte sectors written */
    uint64_t write_ticks_ms;/* total time spent writing (ms) */
};

/*
 * Enumerate whole-disk I/O statistics via IOKit (whole-disk "IOMedia" entries
 * and their IOBlockStorageDriver provider's "Statistics"), filling up to `max`
 * entries and setting *count. Returns 0 on success or an errno on failure.
 */
int procfs_iokit_get_diskstats(struct procfs_diskstat *out, int max, int *count);

#ifdef __cplusplus
}
#endif

#endif /* _FS_PROCFS_PROCFS_IOKIT_H_ */
