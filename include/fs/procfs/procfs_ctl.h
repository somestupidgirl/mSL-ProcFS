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
