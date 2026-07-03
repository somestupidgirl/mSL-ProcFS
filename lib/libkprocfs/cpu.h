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
