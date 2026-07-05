#ifndef cpu_h
#define cpu_h

#include <stdint.h>

#define AMD_UCODE_MAGIC             0x00414d44
#define MSR_UCODE_AMD_PATCHLEVEL    0x0000008b
#define MSR_UCODE_AMD_PATCHLOADER   0xc0010020

#pragma mark -
#pragma mark Common API (all architectures)

extern struct loadavg averunnable;
extern fixpt_t cexp[3];

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

/*
 * The XNU-side analog of the Linux softirq concept. XNU has no softirqs, but it
 * maintains per-CPU interrupt-event counters - hardware IRQs, inter-processor
 * interrupts and timer interrupts - reachable through
 * processor_info(PROCESSOR_CPU_STAT). This surfaces those counters as the
 * per-CPU interrupt/softirq information that /proc/interrupts and /proc/softirqs
 * (and anything else built on the concept) need, replacing the all-zero
 * placeholders with real numbers where the platform provides them.
 */

/* Raw per-CPU interrupt-event counters. */
struct procfs_cpu_irq {
    uint64_t hwirq;   /* hardware interrupts  (irq_ex_cnt) */
    uint64_t ipi;     /* inter-processor IRQs (ipi_cnt)    */
    uint64_t timer;   /* timer interrupts     (timer_cnt)  */
};

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
 * Read the raw per-CPU interrupt counters for logical CPU `cpu`. Returns 0 on
 * success, or an errno - ENOTSUP when cpu_to_processor (libklookup) or the
 * PROCESSOR_CPU_STAT processor_info() flavor are unavailable, in which case
 * *out is zeroed.
 */
extern int procfs_cpu_irq_counts(int cpu, struct procfs_cpu_irq *out);

/*
 * Fill Linux-style per-CPU softirq counts for `cpu` by mapping the XNU per-CPU
 * event counters onto the softirq vectors (TIMER/HRTIMER from the timer
 * interrupt, SCHED from reschedule IPIs; vectors with no XNU counter stay 0).
 * Returns 0 on success, else errno (counts[] zeroed on failure).
 */
extern int procfs_cpu_softirq_counts(int cpu, uint64_t counts[PROCFS_NR_SOFTIRQ]);

#if defined(__x86_64__)
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

/* CPU part hex string derived from hw.cpufamily, e.g. "0x22" for M1 */
extern const char  *arm64_cpu_part(void);

/* CPU revision decimal string, e.g. "0" */
extern const char  *arm64_cpu_revision(void);

/* Human-readable chip name derived from hw.cpufamily, e.g. "Apple Firestorm/Icestorm (M1)" */
extern const char  *arm64_cpu_name(void);

/* BogoMIPS string computed from hw.tbfrequency, e.g. "48.00" */
extern const char  *arm64_bogomips(void);

extern arm_cpu_info_t *cpuid_info(void);

#endif /* __arm64__ || __aarch64__ */

#endif /* cpu_h */
