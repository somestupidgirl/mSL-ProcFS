/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_ctl.h
 *
 * Shared wire protocol for the procfs kernel-control bridge. The kext registers
 * a PF_SYSTEM kernel control (CTL_FLAG_PRIVILEGED, root only); the userspace
 * daemon (procfsd) connects and answers requests using libproc's proc_pidinfo()
 * - the data the kext cannot obtain on its own (CPU time, faults, the full
 * proc_taskinfo, per-thread info). Included by both kext/procfs_ctl.c and
 * tools/procfsd.c.
 */
#ifndef _FS_PROCFS_PROCFS_CTL_H_
#define _FS_PROCFS_PROCFS_CTL_H_

#include <stdint.h>

#define PROCFS_CTL_NAME        "com.beako.filesystems.procfs"
#define PROCFS_CTL_MAGIC       0x50524F43u   /* 'PROC' */
#define PROCFS_CTL_MAXPAYLOAD  2048u
#define PROCFS_CTL_NAMEMAX     256u          /* max MIB name in a request */

/* Request types (procfs_ctl_req.type). */
enum {
    PROCFS_REQ_TASKINFO   = 1,  /* payload: struct proc_taskinfo                 */
    PROCFS_REQ_THREADINFO = 2,  /* arg = tid; payload: struct proc_threadinfo    */
    PROCFS_REQ_VMSTAT     = 3,  /* payload: vm_statistics64_data_t (HOST_VM_INFO64) */
    PROCFS_REQ_LOADAVG    = 4,  /* payload: uint32_t[3] (getloadavg, scaled x100) */
    PROCFS_REQ_REGS       = 5,  /* payload: arm_thread_state64_t / x86_thread_state64_t */
    PROCFS_REQ_FPREGS     = 6,  /* payload: arm_neon_state64_t / x86_float_state64_t   */
    PROCFS_REQ_SYSCTL     = 7,  /* name = dotted MIB name; payload: raw value bytes */
    PROCFS_REQ_EXTENSIONS = 8,  /* arg = byte offset; payload: text chunk of the
                                 * loaded-kext listing (macOS/kextstat style).
                                 * The reply is a slice [arg, arg+MAXPAYLOAD); the
                                 * caller keeps requesting until a short chunk. */
    PROCFS_REQ_MODULES    = 9,  /* arg = byte offset; same chunked transfer as
                                 * PROCFS_REQ_EXTENSIONS but Linux /proc/modules
                                 * format (name size refcount deps state address). */
    PROCFS_REQ_FDLIST     = 10, /* pid; payload: struct proc_fdinfo[] (PROC_PIDLISTFDS) */
    PROCFS_REQ_FDINFO     = 11, /* pid + arg=fd; payload: struct vnode_fdinfowithpath */
    PROCFS_REQ_FDSOCKET   = 12, /* pid + arg=fd; payload: struct socket_fdinfo */
    PROCFS_REQ_DEVICES    = 13, /* arg = byte offset; same chunked transfer as
                                 * PROCFS_REQ_MODULES, Linux /proc/devices format
                                 * (char/block major listing derived from /dev). */
    PROCFS_REQ_RUSAGE     = 14, /* pid; payload: uint64_t[2] = { disk read_bytes,
                                 * disk write_bytes } from proc_pid_rusage */
    PROCFS_REQ_ALLOCINFO  = 15, /* arg = byte offset; same chunked transfer as
                                 * PROCFS_REQ_DEVICES, Linux /proc/allocinfo format
                                 * (per-zone allocation stats via mach_zone_info). */
    PROCFS_REQ_APM        = 16, /* payload: struct procfs_apm_info (IOKit power
                                 * sources), for Linux /proc/apm */
    PROCFS_REQ_PCIDEVICES = 17, /* arg = byte offset; same chunked transfer as
                                 * PROCFS_REQ_DEVICES, Linux /proc/bus/pci/devices
                                 * format (PCI devices via IOKit IOPCIDevice). */
    PROCFS_REQ_FBDEVICES  = 18, /* arg = byte offset; same chunked transfer as
                                 * PROCFS_REQ_DEVICES, Linux /proc/fb format
                                 * (framebuffers via IOKit IO[Mobile]Framebuffer). */
    PROCFS_REQ_NFSEXPORTS = 19, /* arg = byte offset; same chunked transfer as
                                 * PROCFS_REQ_DEVICES, the NFS export table for
                                 * /proc/fs/nfs/exports (macOS /etc/exports). */
    PROCFS_REQ_INTERRUPTS = 20, /* arg = byte offset; same chunked transfer as
                                 * PROCFS_REQ_DEVICES, Linux /proc/interrupts
                                 * (IRQ topology via IOKit IOInterruptSpecifiers). */
    PROCFS_REQ_TTYDRIVERS = 21, /* arg = byte offset; same chunked transfer as
                                 * PROCFS_REQ_DEVICES, Linux /proc/tty/drivers
                                 * (tty devices from /dev grouped by major). */
    PROCFS_REQ_CPUSTAT    = 22, /* payload: struct procfs_cpu_stat[] (one per CPU),
                                 * host_processor_info(PROCESSOR_CPU_STAT). Backs
                                 * the softirq/interrupt concept in libkprocfs. */
    PROCFS_REQ_CPUCLUSTERS = 23, /* payload: char[] (one per logical CPU) = the
                                  * device-tree cluster-type 'E'/'P', for the
                                  * per-core /proc/cpuinfo part number. */
    PROCFS_REQ_TTY        = 24, /* pid; payload: controlling-terminal /dev path
                                 * string (proc_pidinfo PROC_PIDTBSDINFO e_tdev ->
                                 * devname). Empty payload = no controlling tty. */
    PROCFS_REQ_CPULOAD    = 25, /* payload: struct procfs_cpu_load[] (one per CPU),
                                 * host_processor_info(PROCESSOR_CPU_LOAD_INFO).
                                 * The per-CPU user/nice/system/idle ticks behind
                                 * /proc/stat's cpu/cpuN lines. */
    PROCFS_REQ_MAPS       = 26, /* pid + arg = resume address; payload: struct
                                 * procfs_map_region[] for the VM regions at or
                                 * above arg (task_for_pid + mach_vm_region). The
                                 * caller re-requests with arg = last region's end
                                 * until an empty reply. EPERM for SIP/hardened
                                 * targets task_for_pid cannot open. Backs
                                 * map/maps/smaps/smaps_rollup/numa_maps. */
    PROCFS_REQ_MEMREAD    = 27, /* pid + arg = virtual address; payload: up to
                                 * MAXPAYLOAD bytes read from the target at arg
                                 * (task_for_pid + mach_vm_read_overwrite, the
                                 * resident prefix - a short/empty reply marks a
                                 * hole). EPERM for protected targets. Backs
                                 * /proc/<pid>/mem. */
    PROCFS_REQ_PROCARGS   = 28, /* pid + arg = byte offset; payload: slice of the
                                 * flattened KERN_PROCARGS2 argument region (argc,
                                 * exec path, argv, env, apple[]). The caller
                                 * re-requests with arg += chunk until a short
                                 * reply, then splits the sections. Backs
                                 * cmdline/environ/auxv. Uses a root sysctl (no
                                 * task_for_pid), so protected targets still work. */
    PROCFS_REQ_VMALLOCINFO = 29, /* arg = byte offset; same chunked transfer as
                                 * PROCFS_REQ_ALLOCINFO, Linux /proc/vmallocinfo
                                 * format (kernel VM allocations by tagged site via
                                 * mach_memory_info; addresses unavailable on
                                 * macOS, so ranges are 0). */
    PROCFS_REQ_SCSI       = 30, /* arg = byte offset; same chunked transfer as
                                 * PROCFS_REQ_ALLOCINFO, Linux /proc/scsi/scsi
                                 * format (attached SCSI peripheral devices via
                                 * IOKit IOSCSIPeripheralDeviceType*). */
    PROCFS_REQ_SYSVIPC_SHM = 31, /* arg = byte offset; chunked Linux
                                  * /proc/sysvipc/shm - SysV shared-memory segments
                                  * enumerated via the kern.sysv.ipcs.shm sysctl. */
    PROCFS_REQ_SYSVIPC_SEM = 32, /* arg = byte offset; chunked /proc/sysvipc/sem
                                  * (semaphore sets, kern.sysv.ipcs.sem). */
    PROCFS_REQ_SYSVIPC_MSG = 33, /* arg = byte offset; chunked /proc/sysvipc/msg
                                  * (message queues, kern.sysv.ipcs.msg). */
    PROCFS_REQ_SLABINFO   = 34, /* arg = byte offset; chunked Linux /proc/slabinfo
                                 * - one row per zone allocator zone (mach_zone_info,
                                 * the data behind zprint), needs host priv port. */
    PROCFS_REQ_KMSG       = 35, /* arg = byte offset; chunked snapshot of the
                                 * kernel message buffer (proc_kmsgbuf, the data
                                 * behind dmesg), needs root. Linux /proc/kmsg. */
    PROCFS_REQ_LAST_KMSG  = 36, /* arg = byte offset; chunked contents of the
                                 * newest kernel panic report - macOS's only
                                 * cross-boot kernel log. Linux /proc/last_kmsg. */
    PROCFS_REQ_KSYMS      = 37, /* arg = byte offset; chunked "addr type name"
                                 * symbol table of the running kernel image
                                 * (arch/SoC-specific Mach-O). Linux /proc/ksyms.
                                 * Real addresses; the kext zeroes them for
                                 * non-root readers (kptr_restrict). */
    PROCFS_REQ_KALLSYMS   = 38, /* arg = byte offset; like PROCFS_REQ_KSYMS but
                                 * also the non-exported symbol names recovered
                                 * from the XNU source (emitted with address 0,
                                 * as the stripped kernel has no address for
                                 * them). Linux /proc/kallsyms. */
    PROCFS_REQ_MISC       = 39, /* arg = byte offset; chunked "<minor> <name>"
                                 * list of macOS's miscellaneous character devices
                                 * (derived from /dev, as XNU has no misc-device
                                 * registry). Linux /proc/misc. */
    PROCFS_REQ_KSYM_LOOKUP = 40, /* arg = a kernel text address in the wchan symbol
                                  * source's layout; payload = the name of the
                                  * nearest preceding symbol (empty if none). Used
                                  * to symbolize a blocked thread's continuation
                                  * for /proc/<pid>/wchan. */
    PROCFS_REQ_KSYM_REF   = 41, /* payload = uint64 address of the reference symbol
                                 * (_proc_pid) in the wchan symbol source, so the
                                 * kext can compute the running kernel's slide. */
    PROCFS_REQ_PAGEMAP    = 42, /* pid + arg = start virtual address; payload =
                                 * a run of Linux /proc/<pid>/pagemap 64-bit page
                                 * entries (present/swap/file/dirty; PFN hidden)
                                 * from mach_vm_page_query. Empty = past the last
                                 * mapped region (EOF) or task_for_pid denied. */
    PROCFS_REQ_NETTCP     = 43, /* arg = byte offset; chunked Linux /proc/net/tcp
                                 * (IPv4 TCP connection table) built from the
                                 * net.inet.tcp.pcblist_n sysctl. */
    PROCFS_REQ_NETTCP6    = 44, /* arg = byte offset; /proc/net/tcp6 (IPv6 TCP),
                                 * same sysctl filtered to INP_IPV6. */
    PROCFS_REQ_NETUDP     = 45, /* arg = byte offset; /proc/net/udp (IPv4 UDP)
                                 * from net.inet.udp.pcblist_n. */
    PROCFS_REQ_NETUDP6    = 46, /* arg = byte offset; /proc/net/udp6 (IPv6 UDP). */
};

/*
 * Per-CPU interrupt-event counters for PROCFS_REQ_CPUSTAT. The daemon returns an
 * array of these - one per online CPU - built from host_processor_info()'s
 * PROCESSOR_CPU_STAT flavor. This is the XNU-side data behind Linux softirqs and
 * the /proc/interrupts summary lines.
 */
struct procfs_cpu_stat {
    uint64_t hwirq;     /* hardware interrupts  (irq_ex_cnt) */
    uint64_t ipi;       /* inter-processor IRQs (ipi_cnt)    */
    uint64_t timer;     /* timer interrupts     (timer_cnt)  */
};

/*
 * Per-CPU tick counters for PROCFS_REQ_CPULOAD. The daemon returns one per
 * online CPU, from host_processor_info()'s PROCESSOR_CPU_LOAD_INFO flavor -
 * already in USER_HZ jiffies. These back /proc/stat's cpu/cpuN lines.
 */
struct procfs_cpu_load {
    uint64_t user;      /* CPU_STATE_USER   */
    uint64_t nice;      /* CPU_STATE_NICE   */
    uint64_t sys;       /* CPU_STATE_SYSTEM */
    uint64_t idle;      /* CPU_STATE_IDLE   */
};

/*
 * One VM region for PROCFS_REQ_MAPS. The daemon fills these from a userspace
 * mach_vm_region() walk of the target task - BASIC_INFO_64 for the protections,
 * offset and wired count, EXTENDED_INFO for the page counts and share mode - so
 * the kext keeps all node formatting (map/maps/smaps/...) and only consumes the
 * raw records.
 */
struct procfs_map_region {
    uint64_t start;          /* region base address                    */
    uint64_t size;           /* region size in bytes                   */
    uint64_t offset;         /* BASIC.offset (into the backing object)  */
    uint32_t prot;           /* BASIC.protection    (current)          */
    uint32_t max_prot;       /* BASIC.max_protection                   */
    uint32_t user_wired;     /* BASIC.user_wired_count                 */
    uint32_t share_mode;     /* EXTENDED.share_mode (SM_*)             */
    uint32_t resident_pages; /* EXTENDED.pages_resident                */
    uint32_t dirty_pages;    /* EXTENDED.pages_dirtied                 */
    uint32_t swapped_pages;  /* EXTENDED.pages_swapped_out             */
    uint32_t external_pager; /* EXTENDED.external_pager (0 => anon)    */
    uint32_t shared;         /* BASIC.shared (bool)                    */
};

/*
 * Power-source snapshot for /proc/apm (PROCFS_REQ_APM). The daemon fills the raw
 * macOS values from IOKit power sources; the kext maps them to the Linux APM
 * status/flag byte encoding and formats the line. Fields use -1 for "unknown".
 */
struct procfs_apm_info {
    int32_t ac_online;          /* 1 = AC, 0 = battery, -1 = unknown */
    int32_t battery_present;    /* 1 = a battery exists, 0 = none */
    int32_t charging;           /* 1 = charging */
    int32_t percentage;         /* 0-100, -1 = unknown */
    int32_t time_minutes;       /* remaining minutes (to empty/full), -1 = unknown */
};

/* kext -> daemon */
struct procfs_ctl_req {
    uint32_t magic;
    uint32_t seq;       /* echoed in the response so the kext can match it */
    uint32_t type;
    int32_t  pid;
    uint64_t arg;       /* tid for thread requests, else 0 */
    char     name[PROCFS_CTL_NAMEMAX];  /* MIB name for PROCFS_REQ_SYSCTL, else "" */
};

/* daemon -> kext, followed by `len` payload bytes */
struct procfs_ctl_resp {
    uint32_t magic;
    uint32_t seq;
    int32_t  error;     /* 0 on success, else an errno */
    uint32_t len;       /* payload bytes following this header (<= MAXPAYLOAD) */
};

#endif /* _FS_PROCFS_PROCFS_CTL_H_ */
