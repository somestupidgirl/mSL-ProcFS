#ifndef cpu_h
#define cpu_h

#include <stdint.h>

#define AMD_UCODE_MAGIC             0x00414d44
#define MSR_UCODE_AMD_PATCHLEVEL    0x0000008b
#define MSR_UCODE_AMD_PATCHLOADER   0xc0010020

#pragma mark -
#pragma mark Common API (all architectures)

/*
 * sysctlbyname() is an exported kernel KPI symbol, but the latest
 * <sys/sysctl.h> only prototypes it in the !KERNEL branch; declare it here for
 * this kernel-private build (both cpu.c and procfs_linux.c call it).
 */
extern int sysctlbyname(const char *name, void *oldp, size_t *oldlenp,
                        void *newp, size_t newlen);

extern boolean_t    is_amd_cpu(void);
extern boolean_t    is_intel_cpu(void);

extern uint32_t     extract_bitfield(uint32_t infield, uint32_t width, uint32_t offset);
extern uint32_t     get_bitfield_width(uint32_t number);

extern uint32_t     disable_write_protection(void);
extern uint32_t     enable_write_protection(void);

extern uint32_t     set_microcode_version(void);
extern uint32_t     get_microcode_version(void);

extern char        *get_cpu_flags(void);
extern char        *get_cpu_ext_flags(void);
extern char        *get_leaf7_flags(void);
extern char        *get_leaf7_ext_flags(void);

#pragma mark -
#pragma mark Per-CPU interrupt / softirq accounting

#include <fs/procfs/procfs_ctl.h>   /* struct procfs_cpu_stat, PROCFS_REQ_CPUSTAT */

/*
 * The XNU-side analog of the Linux softirq concept. XNU has no softirqs, but it
 * maintains per-CPU interrupt-event counters - hardware IRQs, inter-processor
 * interrupts and timer interrupts - which host_processor_info(PROCESSOR_CPU_STAT)
 * exposes. In-kernel that flavor reads zero for a kext, so the counters are
 * fetched from the procfsd daemon (userspace host_processor_info) and surfaced as
 * the per-CPU interrupt/softirq information that /proc/interrupts and
 * /proc/softirqs (and anything else built on the concept) need. Without a daemon
 * the counters are zero.
 */

/* Linux softirq vectors, in /proc/softirqs order. */
enum procfs_softirq {
    PROCFS_SOFTIRQ_HI = 0,
    PROCFS_SOFTIRQ_TIMER,
    PROCFS_SOFTIRQ_NET_TX,
    PROCFS_SOFTIRQ_NET_RX,
    PROCFS_SOFTIRQ_BLOCK,
    PROCFS_SOFTIRQ_IRQ_POLL,
    PROCFS_SOFTIRQ_TASKLET,
    PROCFS_SOFTIRQ_SCHED,
    PROCFS_SOFTIRQ_HRTIMER,
    PROCFS_SOFTIRQ_RCU,
    PROCFS_NR_SOFTIRQ
};

/* Softirq vector names, indexed by enum procfs_softirq (for /proc/softirqs). */
extern const char *const procfs_softirq_names[PROCFS_NR_SOFTIRQ];

/*
 * Fetch per-CPU interrupt counters for all online CPUs from the procfsd daemon
 * (one PROCFS_REQ_CPUSTAT request). Fills out[0..ncpu); entries the daemon does
 * not report (or all of them, with no daemon) are left zeroed. Returns 0 on
 * success, else an errno.
 */
extern int procfs_cpu_stat_all(struct procfs_cpu_stat *out, uint32_t ncpu);

/*
 * Map one CPU's raw counters onto the Linux softirq vectors (TIMER/HRTIMER from
 * the timer interrupt, SCHED from reschedule IPIs; vectors with no XNU counter
 * stay 0). Pure - no I/O.
 */
extern void procfs_cpu_softirq_map(const struct procfs_cpu_stat *st,
                                   uint64_t counts[PROCFS_NR_SOFTIRQ]);

/*
 * Fetch per-logical-CPU cluster types from the procfsd daemon (the device-tree
 * 'E'/'P', PROCFS_REQ_CPUCLUSTERS): the authoritative source for which cores are
 * efficiency vs performance, used for the per-core /proc/cpuinfo part number.
 * Fills out[0..ncpu); unreported entries (or all, with no daemon) are '?'.
 * Returns 0 on success, else an errno.
 */
extern int procfs_cpu_clusters(char *out, uint32_t ncpu);

#if defined(__x86_64__)
#include <i386/cpuid.h>

/*
 * Forward-ported cpuid_info(): fills an i386_cpu_info_t from the cpuid
 * instruction (do_cpuid) plus a couple of public sysctls, in place of the
 * kernel's private cpuid_info() (not linkable by a third-party kext). Used
 * through the cpuid_info() macro by cpu.c and procfs_linux.c's x86 /proc/cpuinfo.
 */
extern i386_cpu_info_t *procfs_cpuid_info(void);
#define cpuid_info() procfs_cpuid_info()

/* x86-only: power-management line (CPUID 0x80000007) and CPU bug classes
 * (IA32_ARCH_CAPABILITIES + vendor/family). */
extern char        *get_pm_flags(void);
extern char        *get_cpu_bugs(void);

/* AMD-only extended feature flags (CPUID 0x80000001 EDX/ECX); empty on Intel. */
extern char        *get_amd_feature_flags(void);
extern char        *get_amd_feature2_flags(void);
#endif

#pragma mark -
#pragma mark ARM64 API

#if defined(__arm64__) || defined(__aarch64__)

#include <arm/cpuid.h>

/*
 * These functions provide the data needed to populate
 * /proc/cpuinfo entries in Linux ARM64 format:
 *
 *   processor       : <N>
 *   BogoMIPS        : <arm64_bogomips()>
 *   Features        : <get_cpu_flags()>
 *   CPU implementer : <arm64_cpu_implementer()>
 *   CPU architecture: <arm64_cpu_architecture()>
 *   CPU variant     : <arm64_cpu_variant()>
 *   CPU part        : <arm64_cpu_part()>
 *   CPU revision    : <arm64_cpu_revision()>
 */

/* ARM implementer code string, e.g. "0x61" for Apple */
extern const char  *arm64_cpu_implementer(void);

/* ARM architecture version string, always "8" for AArch64 */
extern const char  *arm64_cpu_architecture(void);

/* CPU variant hex string, e.g. "0x0" */
extern const char  *arm64_cpu_variant(void);

/* CPU part hex string for the performance core, e.g. "0x022" for M1 Firestorm */
extern const char  *arm64_cpu_part(void);

/* CPU part hex string for the efficiency core, e.g. "0x023" for M1 Icestorm */
extern const char  *arm64_cpu_part_ecore(void);

/* CPU revision decimal string, e.g. "0" */
extern const char  *arm64_cpu_revision(void);

/* Human-readable chip name derived from hw.cpufamily, e.g. "Apple Firestorm/Icestorm (M1)" */
extern const char  *arm64_cpu_name(void);

/* BogoMIPS string computed from hw.tbfrequency, e.g. "48.00" */
extern const char  *arm64_bogomips(void);

extern arm_cpu_info_t *cpuid_info(void);

#endif /* __arm64__ || __aarch64__ */

#endif /* cpu_h */
