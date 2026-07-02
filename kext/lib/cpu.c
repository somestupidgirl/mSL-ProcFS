/*
 * Copyright (c) 2014 squiffypwn. All rights reserved.
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * cpuinfo.c
 *
 * CPU kext library containing various odds and ends
 * for various CPU-related things and with AMD support
 * in mind.
 *
 * Certain functions have been pulled from the AMD
 * patches for XNU authored by Shaneee, AlGrey and
 * others. Specifically the following functions:
 *
 *      is_amd_cpu()
 *      is_intel_cpu()
 *      extract_bitfield()
 *      get_bitfield_width()
 *
 * ARM64 support added by Sunneva Jonsdottir (2024).
 * ARM64 CPU identification uses sysctlbyname() in place
 * of CPUID, and produces output compatible with the
 * Linux /proc/cpuinfo format for ARM64.
 */

#include <stddef.h>
#include <string.h>
#include <libkext/libkext.h>
#include <mach/machine.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/ioccom.h>
#include <sys/malloc.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/types.h>

#include "cpu.h"
#include "symbols.h"

/* ============================================================
 * x86_64 implementation
 * ============================================================ */
#if defined(__x86_64__)

#include <i386/cpuid.h>
#include <i386/pmCPU.h>
#include <i386/tsc.h>
#include <i386/proc_reg.h>

/*
 * Multipliers used to encode 1*K .. 64*M in a 16 bit size field
 */
#define K               (1)
#define M               (1024)

#define min(a, b) ((a) < (b) ? (a) : (b))
#define quad(hi, lo)    (((uint64_t)(hi)) << 32 | (lo))

/*
 * The "bugs" line reads IA32_ARCH_CAPABILITIES (MSR_IA32_ARCH_CAPABILITIES* are
 * defined by <i386/proc_reg.h>); each *_NO bit means the CPU declares itself NOT
 * vulnerable to that class. The MSR is enumerated by CPUID.(EAX=7,ECX=0).EDX[29].
 */
#define CPUID7_EDX_ARCH_CAPABILITIES (1U << 29)

/*
 * Returns TRUE if AMD CPU.
 */
boolean_t
is_amd_cpu(void)
{
    uint32_t ourcpuid[4];

    do_cpuid(0, ourcpuid);
    if (ourcpuid[ebx] == 0x68747541 &&
        ourcpuid[ecx] == 0x444D4163 &&
        ourcpuid[edx] == 0x69746E65) {
        return TRUE;
    }

    return FALSE;
};

/*
 * Returns TRUE if Intel CPU.
 */
boolean_t
is_intel_cpu(void)
{
    uint32_t ourcpuid[4];

    do_cpuid(0, ourcpuid);
    if (ourcpuid[ebx] == 0x756E6547 &&
        ourcpuid[ecx] == 0x6C65746E &&
        ourcpuid[edx] == 0x49656E69) {
        return TRUE;
    }

    if (!is_amd_cpu()) {
        return TRUE;
    }

    return FALSE;
}

/*
 * Extract bitfield function.
 */
uint32_t
extract_bitfield(uint32_t infield, uint32_t width, uint32_t offset)
{
    uint32_t bitmask;
    uint32_t outfield;

    if ((offset+width) == 32) {
        bitmask = (0xFFFFFFFF<<offset);
    } else {
        bitmask = (0xFFFFFFFF<<offset) ^ (0xFFFFFFFF<<(offset+width));
    }

    outfield = (infield & bitmask) >> offset;
    return outfield;
}

/*
 * Get bitfield width function.
 */
uint32_t
get_bitfield_width(uint32_t number)
{
    uint32_t fieldwidth;

    number--;
    if (number == 0) {
        return 0;
    }

    __asm__ volatile ( "bsr %%eax, %%ecx\n\t"
                      : "=c" (fieldwidth)
                      : "a"(number));

    return fieldwidth+1;  /* bsr returns the position, we want the width */
}

/*
 * Disable CPU write protection.
 * From squiffypwn's libmasochist.
 */
uint32_t
disable_write_protection() {
    uintptr_t cr0 = get_cr0();
    cr0 &= ~CR0_WP;
    set_cr0(cr0);

    return 0;
}

/*
 * Enable CPU write protection.
 * From squiffypwn's libmasochist.
 */
uint32_t
enable_write_protection() {
    uintptr_t cr0 = get_cr0();
    cr0 |= CR0_WP;
    set_cr0(cr0);

    return 0;
}

/*
 * This function returns the actual microcode version
 * number for both AMD and Intel CPUs without modify-
 * ing the cpuid_microcode_version field in struct
 * i386_cpu_info.
 */
uint32_t
get_microcode_version(void)
{
    uint32_t version = 0;

    if (is_amd_cpu()) {
        version = (uint32_t)rdmsr64(MSR_UCODE_AMD_PATCHLEVEL);
    } else if (is_intel_cpu()) {
        version = (uint32_t)(rdmsr64(MSR_IA32_BIOS_SIGN_ID) >> 32);
    }

    return version;
}

/*
 * WARNING! This is currently for R&D purposes only.
 *
 * In the AMD kernel the cpuid_microcode_version
 * field in struct i386_cpu_info is set to 186.
 * This is obviously a dummy version number, but
 * setting it to its actual version number will
 * result in a kernel panic for some unknown rea-
 * son. Needs further research.
 */
uint32_t
set_microcode_version(void)
{
    i386_cpu_info_t *info_p = NULL;
    uint32_t reg[4];

    /*
     * Get processor signature and decode
     * and bracket this with the approved procedure for reading the
     * the microcode version number a.k.a. signature a.k.a. BIOS ID
     */
    if (is_intel_cpu()) {
        wrmsr64(MSR_IA32_BIOS_SIGN_ID, 0);
        do_cpuid(1, reg);
        info_p->cpuid_microcode_version = (uint32_t)(rdmsr64(MSR_IA32_BIOS_SIGN_ID) >> 32);
    } else if (is_amd_cpu()) {
        wrmsr64(MSR_UCODE_AMD_PATCHLOADER, 0);
        do_cpuid(1, reg);
        info_p->cpuid_microcode_version = (uint32_t)(rdmsr64(MSR_UCODE_AMD_PATCHLEVEL));
    }

    return 0;
}

/*
 * CPU features and flags.
 */

uint64_t feature_list[] = {
    /* 0 */  CPUID_FEATURE_FPU,
    /* 1 */  CPUID_FEATURE_VME,
    /* 2 */  CPUID_FEATURE_DE,
    /* 3 */  CPUID_FEATURE_PSE,
    /* 4 */  CPUID_FEATURE_TSC,
    /* 5 */  CPUID_FEATURE_MSR,
    /* 6 */  CPUID_FEATURE_PAE,
    /* 7 */  CPUID_FEATURE_MCE,
    /* 8 */  CPUID_FEATURE_CX8,
    /* 9 */  CPUID_FEATURE_APIC,
    /* 10 */ CPUID_FEATURE_SEP,
    /* 11 */ CPUID_FEATURE_MTRR,
    /* 12 */ CPUID_FEATURE_PGE,
    /* 13 */ CPUID_FEATURE_MCA,
    /* 14 */ CPUID_FEATURE_CMOV,
    /* 15 */ CPUID_FEATURE_PAT,
    /* 16 */ CPUID_FEATURE_PSE36,
    /* 17 */ CPUID_FEATURE_PSN,
    /* 18 */ CPUID_FEATURE_CLFSH,
    /* 19 */ CPUID_FEATURE_DS,
    /* 20 */ CPUID_FEATURE_ACPI,
    /* 21 */ CPUID_FEATURE_MMX,
    /* 22 */ CPUID_FEATURE_FXSR,
    /* 23 */ CPUID_FEATURE_SSE,
    /* 24 */ CPUID_FEATURE_SSE2,
    /* 25 */ CPUID_FEATURE_SS,
    /* 26 */ CPUID_FEATURE_HTT,
    /* 27 */ CPUID_FEATURE_TM,
    /* 28 */ CPUID_FEATURE_PBE,
    /* 29 */ CPUID_FEATURE_SSE3,
    /* 30 */ CPUID_FEATURE_PCLMULQDQ,
    /* 31 */ CPUID_FEATURE_DTES64,
    /* 32 */ CPUID_FEATURE_MONITOR,
    /* 33 */ CPUID_FEATURE_DSCPL,
    /* 34 */ CPUID_FEATURE_VMX,
    /* 35 */ CPUID_FEATURE_SMX,
    /* 36 */ CPUID_FEATURE_EST,
    /* 37 */ CPUID_FEATURE_TM2,
    /* 38 */ CPUID_FEATURE_SSSE3,
    /* 39 */ CPUID_FEATURE_CID,
    /* 40 */ CPUID_FEATURE_SEGLIM64,
    /* 41 */ CPUID_FEATURE_FMA,
    /* 42 */ CPUID_FEATURE_CX16,
    /* 43 */ CPUID_FEATURE_xTPR,
    /* 44 */ CPUID_FEATURE_PDCM,
    /* 45 */ CPUID_FEATURE_PCID,
    /* 46 */ CPUID_FEATURE_DCA,
    /* 47 */ CPUID_FEATURE_SSE4_1,
    /* 48 */ CPUID_FEATURE_SSE4_2,
    /* 49 */ CPUID_FEATURE_x2APIC,
    /* 50 */ CPUID_FEATURE_MOVBE,
    /* 51 */ CPUID_FEATURE_POPCNT,
    /* 52 */ CPUID_FEATURE_TSCTMR,
    /* 53 */ CPUID_FEATURE_AES,
    /* 54 */ CPUID_FEATURE_XSAVE,
    /* 55 */ CPUID_FEATURE_OSXSAVE,
    /* 56 */ CPUID_FEATURE_AVX1_0,
    /* 57 */ CPUID_FEATURE_F16C,
    /* 58 */ CPUID_FEATURE_RDRAND,
    /* 59 */ CPUID_FEATURE_VMM,
};

const char *
feature_flags[] = {
    /* 0 */  "fpu",
    /* 1 */  "vme",
    /* 2 */  "de",
    /* 3 */  "pse",
    /* 4 */  "tsc",
    /* 5 */  "msr",
    /* 6 */  "pae",
    /* 7 */  "mce",
    /* 8 */  "cx8",
    /* 9 */  "apic",
    /* 10 */ "sep",
    /* 11 */ "mtrr",
    /* 12 */ "pge",
    /* 13 */ "mca",
    /* 14 */ "cmov",
    /* 15 */ "pat",
    /* 16 */ "pse36",
    /* 17 */ "psn",
    /* 18 */ "clfsh",
    /* 19 */ "ds",
    /* 20 */ "acpi",
    /* 21 */ "mmx",
    /* 22 */ "fxsr",
    /* 23 */ "sse",
    /* 24 */ "sse2",
    /* 25 */ "ss",
    /* 26 */ "htt",
    /* 27 */ "tm",
    /* 28 */ "pbe",
    /* 39 */ "sse3",
    /* 30 */ "pclmulqdq",
    /* 31 */ "dtes64",
    /* 32 */ "mon",
    /* 33 */ "dscpl",
    /* 34 */ "vmx",
    /* 35 */ "smx",
    /* 36 */ "est",
    /* 37 */ "tm2",
    /* 38 */ "ssse3",
    /* 39 */ "cid",
    /* 40 */ "seglim64",
    /* 41 */ "fma",
    /* 42 */ "cx16",
    /* 43 */ "tpr",
    /* 44 */ "pdcm",
    /* 45 */ "pcid",
    /* 46 */ "dca",
    /* 47 */ "sse4.1",
    /* 48 */ "sse4.2",
    /* 49 */ "x2apic",
    /* 50 */ "movbe",
    /* 51 */ "popcnt",
    /* 52 */ "tsctmr",
    /* 53 */ "aes",
    /* 54 */ "xsave",
    /* 55 */ "osxsave",
    /* 56 */ "avx",
    /* 57 */ "f16c",
    /* 58 */ "rdrand",
    /* 59 */ "vmm",
};

char *
get_cpu_flags(void)
{
    /* Static buffer (mirrors the arm64 getters): a real char array so sizeof()
     * is the byte capacity, initialised before use, and valid after return -
     * unlike the former VLA-of-pointers which was uninitialised and dangling. */
    static char flags[512];
    int i = 0;

    flags[0] = '\0';
    if (cpuid_info()->cpuid_features) {
        while (i < (int)ARRAY_COUNT(feature_flags)) {
            /* If the CPU supports a feature in feature_list[], append its flag. */
            if (cpuid_info()->cpuid_features & feature_list[i]) {
                strlcat(flags, feature_flags[i], sizeof(flags));
                strlcat(flags, " ", sizeof(flags));
            }
            i++;
        }
    }
    return flags;
}

/*
 * An array of extended features as defined in
 * osfmk/i386/cpuid.h. This should always be
 * in the same order as its corresponding
 * character array below.
 */
uint64_t feature_ext_list[] = {
    /* 0 */ CPUID_EXTFEATURE_SYSCALL,
    /* 1 */ CPUID_EXTFEATURE_XD,
    /* 2 */ CPUID_EXTFEATURE_1GBPAGE,
    /* 3 */ CPUID_EXTFEATURE_RDTSCP,
    /* 4 */ CPUID_EXTFEATURE_EM64T,
    /* 5 */ CPUID_EXTFEATURE_LAHF,
    /* 6 */ CPUID_EXTFEATURE_LZCNT,
    /* 7 */ CPUID_EXTFEATURE_PREFETCHW
};

const char *
feature_ext_flags[] = {
    /* 0 */ "syscall",
    /* 1 */ "xd",
    /* 2 */ "1gbpage",
    /* 3 */ "rdtscp",
    /* 4 */ "em64t",
    /* 5 */ "lahf",
    /* 6 */ "lzcnt",
    /* 7 */ "prefetchcw"
};

char *
get_cpu_ext_flags(void)
{
    static char flags[512];
    int i = 0;

    flags[0] = '\0';
    if (cpuid_info()->cpuid_extfeatures) {
        while (i < (int)ARRAY_COUNT(feature_ext_flags)) {
            if (cpuid_info()->cpuid_extfeatures & feature_ext_list[i]) {
                strlcat(flags, feature_ext_flags[i], sizeof(flags));
                strlcat(flags, " ", sizeof(flags));
            }
            i++;
        }
    }
    return flags;
}

uint64_t leaf7_feature_list[] = {
    /* 0 */  CPUID_LEAF7_FEATURE_RDWRFSGS,
    /* 1 */  CPUID_LEAF7_FEATURE_TSCOFF,
    /* 2 */  CPUID_LEAF7_FEATURE_SGX,
    /* 3 */  CPUID_LEAF7_FEATURE_BMI1,
    /* 4 */  CPUID_LEAF7_FEATURE_HLE,
    /* 5 */  CPUID_LEAF7_FEATURE_AVX2,
    /* 6 */  CPUID_LEAF7_FEATURE_FDPEO,
    /* 7 */  CPUID_LEAF7_FEATURE_SMEP,
    /* 8 */  CPUID_LEAF7_FEATURE_BMI2,
    /* 9 */  CPUID_LEAF7_FEATURE_ERMS,
    /* 10 */ CPUID_LEAF7_FEATURE_INVPCID,
    /* 11 */ CPUID_LEAF7_FEATURE_RTM,
    /* 12 */ CPUID_LEAF7_FEATURE_PQM,
    /* 13 */ CPUID_LEAF7_FEATURE_FPU_CSDS,
    /* 14 */ CPUID_LEAF7_FEATURE_MPX,
    /* 15 */ CPUID_LEAF7_FEATURE_PQE,
    /* 16 */ CPUID_LEAF7_FEATURE_AVX512F,
    /* 17 */ CPUID_LEAF7_FEATURE_AVX512DQ,
    /* 18 */ CPUID_LEAF7_FEATURE_RDSEED,
    /* 19 */ CPUID_LEAF7_FEATURE_ADX,
    /* 20 */ CPUID_LEAF7_FEATURE_SMAP,
    /* 21 */ CPUID_LEAF7_FEATURE_AVX512IFMA,
    /* 22 */ CPUID_LEAF7_FEATURE_CLFSOPT,
    /* 23 */ CPUID_LEAF7_FEATURE_CLWB,
    /* 24 */ CPUID_LEAF7_FEATURE_IPT,
    /* 25 */ CPUID_LEAF7_FEATURE_AVX512CD,
    /* 26 */ CPUID_LEAF7_FEATURE_SHA,
    /* 27 */ CPUID_LEAF7_FEATURE_AVX512BW,
    /* 28 */ CPUID_LEAF7_FEATURE_AVX512VL,
    /* 29 */ CPUID_LEAF7_FEATURE_PREFETCHWT1,
    /* 30 */ CPUID_LEAF7_FEATURE_AVX512VBMI,
    /* 31 */ CPUID_LEAF7_FEATURE_UMIP,
    /* 32 */ CPUID_LEAF7_FEATURE_PKU,
    /* 33 */ CPUID_LEAF7_FEATURE_OSPKE,
    /* 34 */ CPUID_LEAF7_FEATURE_WAITPKG,
    /* 35 */ CPUID_LEAF7_FEATURE_GFNI,
    /* 36 */ CPUID_LEAF7_FEATURE_VAES,
    /* 37 */ CPUID_LEAF7_FEATURE_VPCLMULQDQ,
    /* 38 */ CPUID_LEAF7_FEATURE_AVX512VNNI,
    /* 39 */ CPUID_LEAF7_FEATURE_AVX512BITALG,
    /* 40 */ CPUID_LEAF7_FEATURE_AVX512VPCDQ,
    /* 41 */ CPUID_LEAF7_FEATURE_RDPID,
    /* 42 */ CPUID_LEAF7_FEATURE_CLDEMOTE,
    /* 43 */ CPUID_LEAF7_FEATURE_MOVDIRI,
    /* 44 */ CPUID_LEAF7_FEATURE_MOVDIRI64B,
    /* 45 */ CPUID_LEAF7_FEATURE_SGXLC
};

const char *
leaf7_feature_flags[] = {
    /* 0 */  "rdwrfsgs",
    /* 1 */  "tsc_thread_offset",
    /* 2 */  "sgx",
    /* 3 */  "bmi1",
    /* 4 */  "hle",
    /* 5 */  "avx2",
    /* 6 */  "fdpeo",
    /* 7 */  "smep",
    /* 8 */  "bmi2",
    /* 9 */  "erms",
    /* 10 */ "invpcid",
    /* 11 */ "rtm",
    /* 12 */ "pqm",
    /* 13 */ "fpu_csds",
    /* 14 */ "mpx",
    /* 15 */ "pqe",
    /* 16 */ "avx512f",
    /* 17 */ "avx512dq",
    /* 18 */ "rdseed",
    /* 19 */ "adx",
    /* 20 */ "smap",
    /* 21 */ "avx512ifma",
    /* 22 */ "clfsopt",
    /* 23 */ "clwb",
    /* 24 */ "ipt",
    /* 25 */ "avx512cd",
    /* 26 */ "sha",
    /* 27 */ "avx512bw",
    /* 28 */ "avx512vl",
    /* 29 */ "prefetchwt1",
    /* 30 */ "avx512vbmi",
    /* 31 */ "umip",
    /* 32 */ "pku",
    /* 33 */ "ospke",
    /* 34 */ "waitpkg",
    /* 35 */ "gfni",
    /* 36 */ "vaes",
    /* 37 */ "vpclmulqdq",
    /* 38 */ "avx512vnni",
    /* 39 */ "avx512bitalg",
    /* 40 */ "avx512vpopcntdq",
    /* 41 */ "rdpid",
    /* 42 */ "cldemote",
    /* 43 */ "movdiri",
    /* 44 */ "movdiri64b",
    /* 45 */ "sgxlc"
};

char*
get_leaf7_flags(void)
{
    int i = 0;

    /*
     * Enable reading the cpuid_leaf7_features on AMD chipsets.
     */
    uint32_t reg[4];
    if (is_amd_cpu() && cpuid_info()->cpuid_family >= 23){
        do_cpuid(0x7, reg);
        cpuid_info()->cpuid_leaf7_features = quad(reg[ecx], reg[ebx]) & ~CPUID_LEAF7_FEATURE_SMAP;
    }

    /*
     * Main loop.
     */
    static char flags[1024];
    flags[0] = '\0';
    if (cpuid_info()->cpuid_leaf7_features) {
        while (i < (int)ARRAY_COUNT(leaf7_feature_flags)) {
            /* If the CPU supports a feature in leaf7_feature_list[], append it. */
            if (cpuid_info()->cpuid_leaf7_features & leaf7_feature_list[i]) {
                strlcat(flags, leaf7_feature_flags[i], sizeof(flags));
                strlcat(flags, " ", sizeof(flags));
            }
            i++;
        }
    }
    return flags;
}

uint64_t leaf7_feature_ext_list[] = {
    /* 0 */  CPUID_LEAF7_EXTFEATURE_AVX5124VNNIW,
    /* 1 */  CPUID_LEAF7_EXTFEATURE_AVX5124FMAPS,
    /* 2 */  CPUID_LEAF7_EXTFEATURE_FSREPMOV,
    /* 3 */  CPUID_LEAF7_EXTFEATURE_SRBDS_CTRL,
    /* 4 */  CPUID_LEAF7_EXTFEATURE_MDCLEAR,
    /* 5 */  CPUID_LEAF7_EXTFEATURE_TSXFA,
    /* 6 */  CPUID_LEAF7_EXTFEATURE_IBRS,
    /* 7 */  CPUID_LEAF7_EXTFEATURE_STIBP,
    /* 8 */  CPUID_LEAF7_EXTFEATURE_L1DF,
    /* 9 */  CPUID_LEAF7_EXTFEATURE_ACAPMSR,
    /* 10 */ CPUID_LEAF7_EXTFEATURE_CCAPMSR,
    /* 11 */ CPUID_LEAF7_EXTFEATURE_SSBD
};

const char *
leaf7_feature_ext_flags[] = {
    /* 0 */  "avx5124vnniw",
    /* 1 */  "avx5124fmaps",
    /* 2 */  "fsrepmov",
    /* 3 */  "srbds_ctlr",
    /* 4 */  "mdclear",
    /* 5 */  "tsxfa",
    /* 6 */  "ibrs",
    /* 7 */  "stibp",
    /* 8 */  "l1d",
    /* 9 */  "acapmsr",
    /* 10 */ "ccapmsr",
    /* 11 */ "ssbd"
};

char *
get_leaf7_ext_flags(void)
{
    int i = 0;

    /*
     * FIXME: Enable reading the cpuid_leaf7_extfeatures on AMD chipsets.
     */
#if 0
    uint32_t reg[4];
    if (is_amd_cpu() && cpuid_info()->cpuid_family >= 23){
        do_cpuid(0x7, reg);
        cpuid_info()->cpuid_leaf7_extfeatures = reg[ebx];
    }
#endif

    /*
     * Main loop.
     */
    static char flags[512];
    flags[0] = '\0';
    if (cpuid_info()->cpuid_leaf7_extfeatures) {
        while (i < (int)ARRAY_COUNT(leaf7_feature_ext_flags)) {
            /* If the CPU supports a feature in leaf7_feature_ext_list[], append. */
            if (cpuid_info()->cpuid_leaf7_extfeatures & leaf7_feature_ext_list[i]) {
                strlcat(flags, leaf7_feature_ext_flags[i], sizeof(flags));
                strlcat(flags, " ", sizeof(flags));
            }
            i++;
        }
    }
    return flags;
}

/*
 * Power-management flags (the trailing "power management:" line of a Linux
 * /proc/cpuinfo block), decoded from CPUID.(EAX=0x80000007).EDX. The bit->name
 * mapping matches Linux's x86_power_flags[] (arch/x86/kernel/cpu/powerflags.c);
 * bit 8 (invariant TSC) is surfaced as a normal flag ("constant_tsc") rather
 * than left blank as Linux does, since we do not fold it into the flags line.
 */
static const char *pm_flags_tbl[] = {
    /*  0 */ "ts",           /* temperature sensor */
    /*  1 */ "fid",          /* frequency id control */
    /*  2 */ "vid",          /* voltage id control */
    /*  3 */ "ttp",          /* thermal trip */
    /*  4 */ "tm",           /* hardware thermal control */
    /*  5 */ "stc",          /* software thermal control */
    /*  6 */ "100mhzsteps",
    /*  7 */ "hwpstate",
    /*  8 */ "constant_tsc", /* invariant TSC */
    /*  9 */ "cpb",          /* core performance boost */
    /* 10 */ "eff_freq_ro",  /* readonly aperf/mperf */
    /* 11 */ "proc_feedback",
    /* 12 */ "acc_power",    /* accumulated power mechanism */
};

char *
get_pm_flags(void)
{
    static char flags[256];
    uint32_t reg[4];

    flags[0] = '\0';

    /* Guard: the power-management leaf must be within the max extended leaf. */
    do_cpuid(0x80000000, reg);
    if (reg[eax] < 0x80000007) {
        return flags;
    }
    do_cpuid(0x80000007, reg);

    for (int b = 0; b < (int)ARRAY_COUNT(pm_flags_tbl); b++) {
        if ((reg[edx] & (1U << b)) && pm_flags_tbl[b][0] != '\0') {
            strlcat(flags, pm_flags_tbl[b], sizeof(flags));
            strlcat(flags, " ", sizeof(flags));
        }
    }
    return flags;
}

/*
 * The "bugs" line: known speculative-execution and errata classes affecting the
 * CPU. This mirrors the core of Linux's cpu_set_bug_bits() - using the
 * IA32_ARCH_CAPABILITIES *_NO bits and CPU vendor/family to decide - but does
 * not reproduce Linux's full per-model whitelist tables, so it reports the
 * common classes rather than every model-specific exemption.
 */
char *
get_cpu_bugs(void)
{
    static char bugs[512];
    uint32_t reg[4];
    uint64_t arch_cap = 0;

    bugs[0] = '\0';

    /* IA32_ARCH_CAPABILITIES is enumerated by CPUID.(7,0).EDX[29]. */
    do_cpuid(0x7, reg);
    if (reg[edx] & CPUID7_EDX_ARCH_CAPABILITIES) {
        arch_cap = rdmsr64(MSR_IA32_ARCH_CAPABILITIES);
    }

    if (is_intel_cpu()) {
        if (!(arch_cap & MSR_IA32_ARCH_CAPABILITIES_RDCL_NO)) {
            strlcat(bugs, "cpu_meltdown ", sizeof(bugs));
            strlcat(bugs, "l1tf ", sizeof(bugs));
        }
        strlcat(bugs, "spectre_v1 spectre_v2 swapgs ", sizeof(bugs));
        if (!(arch_cap & MSR_IA32_ARCH_CAPABILITIES_SSB_NO)) {
            strlcat(bugs, "spec_store_bypass ", sizeof(bugs));
        }
        if (!(arch_cap & MSR_IA32_ARCH_CAPABILITIES_MDS_NO)) {
            strlcat(bugs, "mds ", sizeof(bugs));
        }
        /* SRBDS: only when the SRBDS mitigation control is enumerated. */
        if (cpuid_info()->cpuid_leaf7_extfeatures & CPUID_LEAF7_EXTFEATURE_SRBDS_CTRL) {
            strlcat(bugs, "srbds ", sizeof(bugs));
        }
    } else if (is_amd_cpu()) {
        strlcat(bugs, "spectre_v1 spectre_v2 ", sizeof(bugs));
        if (!(arch_cap & MSR_IA32_ARCH_CAPABILITIES_SSB_NO)) {
            strlcat(bugs, "spec_store_bypass ", sizeof(bugs));
        }
        /* Pre-Zen SYSRET SS-attributes errata; Zen1/Zen2 retbleed. */
        if (cpuid_info()->cpuid_family < 0x17) {
            strlcat(bugs, "sysret_ss_attrs ", sizeof(bugs));
        } else if (cpuid_info()->cpuid_family == 0x17) {
            strlcat(bugs, "retbleed ", sizeof(bugs));
        }
    }
    return bugs;
}

/*
 * TODO
 */

const char *
amd_feature_flags[] = {
    /* 0 */ "syscall",
    /* 1 */ "mp",
    /* 2 */ "nx",
    /* 3 */ "mmxext",
    /* 4 */ "fxsr_opt",
    /* 5 */ "pdpe1gb",
    /* 6 */ "rdtscp",
    /* 7 */ "lm",
    /* 8 */ "3dnowext",
    /* 9 */ "3dnow",
};

static char *
amd_feature2_flags[] = {
    /* 0 */  "lahf_lm",
    /* 1 */  "cmp_legacy",
    /* 2 */  "svm",
    /* 3 */  "extapic",
    /* 4 */  "cr8_legacy",
    /* 5 */  "abm",
    /* 6 */  "sse4a",
    /* 7 */  "misalignsse",
    /* 8 */  "3dnowprefetch",
    /* 9 */  "osvw",
    /* 10 */ "ibs",
    /* 11 */ "xop",
    /* 12 */ "skinit",
    /* 13 */ "wdt",
    /* 14 */ "lwp",
    /* 15 */ "fma4",
    /* 16 */ "tce",
    /* 17 */ "nodeid_msr",
    /* 18 */ "tbm",
    /* 19 */ "topoext",
    /* 20 */ "perfctr_core",
    /* 21 */ "perfctr_nb",
    /* 22 */ "bpext",
    /* 23 */ "ptsc",
    /* 24 */ "perfctr_llc",
    /* 25 */ "mwaitx",
};

const char *
amd_power_flags[] = {
    /* 0 */  "ts",                  /* temperature sensor */
    /* 1 */  "fid",                 /* frequency id control */
    /* 2 */  "vid",                 /* voltage id control */
    /* 3 */  "ttp",                 /* thermal trip */
    /* 4 */  "tm",                  /* hardware thermal control */
    /* 5 */  "stc",                 /* software thermal control */
    /* 6 */  "100mhzsteps",         /* 100 MHz multiplier control */
    /* 7 */  "hwpstate",            /* hardware P-state control */
    /* 8 */  "",                    /* tsc invariant mapped to constant_tsc */
    /* 9 */  "cpb",                 /* core performance boost */
    /* 10 */ "eff_freq_ro",         /* readonly aperf/mperf */
    /* 11 */ "proc_feedback",       /* processor feedback interface */
    /* 12 */ "acc_power"            /* accumulated power mechanism */
};

const char *
x86_bug_flags[] = {
    /* 0 */ "f00f",                 /* Intel F00F */
    /* 1 */ "fdiv",                 /* FPU FDIV */
    /* 2 */ "coma",                 /* Cyrix 6x86 coma */
    /* 3 */ "tlb_mmatch",           /* "tlb_mmatch" AMD Erratum 383 */
    /* 4 */ "apic_c1e",             /* "apic_c1e" AMD Erratum 400 */
    /* 5 */ "11ap",                 /* Bad local APIC aka 11AP */
    /* 6 */ "fxsave_leak",          /* FXSAVE leaks FOP/FIP/FOP */
    /* 7 */ "clflush_monitor",      /* AAI65, CLFLUSH required before MONITOR */
    /* 8 */ "sysret_ss_attrs",      /* SYSRET doesn't fix up SS attrs */
    /* 9 */ "espfix"                /* "" IRET to 16-bit SS corrupts ESP/RSP high bits */
};

const char *
x86_64_bug_flags[] = {
    /* 0 */  "null_seg",            /* Nulling a selector preserves the base */
    /* 1 */  "swapgs_fence",        /* SWAPGS without input dep on GS */
    /* 2 */  "monitor",             /* IPI required to wake up remote CPU */
    /* 3 */  "amd_e400",            /* CPU is among the affected by Erratum 400 */
    /* 4 */  "cpu_meltdown",        /* CPU is affected by meltdown attack and needs kernel page table isolation */
    /* 5 */  "spectre_v1",          /* CPU is affected by Spectre variant 1 attack with conditional branches */
    /* 6 */  "spectre_v2",          /* CPU is affected by Spectre variant 2 attack with indirect branches */
    /* 7 */  "spec_store_bypass",   /* CPU is affected by speculative store bypass attack */
    /* 8 */  "l1tf",                /* CPU is affected by L1 Terminal Fault */
    /* 9 */  "mds",                 /* CPU is affected by Microarchitectural data sampling */
    /* 10 */ "msdbs_only",          /* CPU is only affected by the  MSDBS variant of BUG_MDS */
    /* 11 */ "swapgs",              /* CPU is affected by speculation through SWAPGS */
    /* 12 */ "taa",                 /* CPU is affected by TSX Async Abort(TAA) */
    /* 13 */ "itlb_multihit",       /* CPU may incur MCE during certain page attribute changes */
    /* 14 */ "srbds"                /* CPU may leak RNG bits if not mitigated */
};



/* ============================================================
 * ARM64 implementation
 *
 * Produces output compatible with Linux /proc/cpuinfo ARM64
 * format:
 *
 *   processor       : 0
 *   BogoMIPS        : 48.00
 *   Features        : fp asimd evtstrm aes pmull sha1 sha2 ...
 *   CPU implementer : 0x41
 *   CPU architecture: 8
 *   CPU variant     : 0x0
 *   CPU part        : 0xd08
 *   CPU revision    : 3
 *
 * On Apple Silicon:
 *   - CPU implementer is always 0x61 (Apple, assigned by ARM)
 *   - CPU architecture is always 8 (ARMv8)
 *   - CPU part is derived from hw.cpufamily
 *   - CPU variant and revision are read from MIDR_EL1 where
 *     accessible, otherwise reported as 0
 *   - Features are enumerated via hw.optional.arm.* sysctls
 *
 * ============================================================ */
#elif defined(__arm64__) || defined(__aarch64__)

#include <arm/cpuid.h>

static SECURITY_READ_ONLY_LATE(arm_cpu_info_t) cpuid_cpu_info;

arm_cpu_info_t *
cpuid_info(void)
{
    return &cpuid_cpu_info;
}

/*
 * Apple CPU part numbers derived from hw.cpufamily values.
 * These correspond to ARM's assigned part numbers for Apple cores.
 *
 * Values sourced from XNU osfmk/arm/cpuid.h and public
 * ARM documentation for Apple Silicon.
 */
#define CPUFAMILY_ARM_SWIFT         0x1e2d6381  /* A6  - Swift        */
#define CPUFAMILY_ARM_CYCLONE       0x37a09642  /* A7  - Cyclone      */
#define CPUFAMILY_ARM_TYPHOON       0x2c91a47e  /* A8  - Typhoon      */
#define CPUFAMILY_ARM_TWISTER       0x92fb37c8  /* A9  - Twister      */
#define CPUFAMILY_ARM_HURRICANE     0x67ceee93  /* A10 - Hurricane    */
#define CPUFAMILY_ARM_MONSOON_MISTRAL 0xe81e7ef6 /* A11 - Monsoon     */
#define CPUFAMILY_ARM_VORTEX_TEMPEST  0x07d34b9f /* A12 - Vortex      */
#define CPUFAMILY_ARM_LIGHTNING_THUNDER 0x462504d2 /* A13 - Lightning  */
#define CPUFAMILY_ARM_FIRESTORM_ICESTORM 0x1b588bb3 /* A14/M1          */
#define CPUFAMILY_ARM_AVALANCHE_BLIZZARD 0xda33d83d /* A15/M2          */
#define CPUFAMILY_ARM_EVEREST_SAWTOOTH   0x8765edea /* A16/M3          */
#define CPUFAMILY_ARM_IBIZA              0xfa33415e /* A17/M4          */

/*
 * ARM implementer code for Apple Inc.
 * Assigned by ARM Holdings.
 */
#define ARM_IMPLEMENTER_APPLE       0x61

/*
 * arm64_cpu_part_string()
 *
 * Returns the ARM part number string for a given hw.cpufamily value.
 * Part numbers follow ARM convention (0xXXX hex).
 */
static const char *
arm64_cpu_part_string(uint32_t cpufamily)
{
    switch (cpufamily) {
        case CPUFAMILY_ARM_SWIFT:               return "0x01";
        case CPUFAMILY_ARM_CYCLONE:             return "0x02";
        case CPUFAMILY_ARM_TYPHOON:             return "0x03";
        case CPUFAMILY_ARM_TWISTER:             return "0x04";
        case CPUFAMILY_ARM_HURRICANE:           return "0x05";
        case CPUFAMILY_ARM_MONSOON_MISTRAL:     return "0x06";
        case CPUFAMILY_ARM_VORTEX_TEMPEST:      return "0x07";
        case CPUFAMILY_ARM_LIGHTNING_THUNDER:   return "0x08";
        case CPUFAMILY_ARM_FIRESTORM_ICESTORM:  return "0x22"; /* M1  */
        case CPUFAMILY_ARM_AVALANCHE_BLIZZARD:  return "0x25"; /* M2  */
        case CPUFAMILY_ARM_EVEREST_SAWTOOTH:    return "0x28"; /* M3  */
        case CPUFAMILY_ARM_IBIZA:               return "0x2b"; /* M4  */
        default:                                return "0x00";
    }
}

/*
 * arm64_cpu_name_string()
 *
 * Returns a human-readable chip name for a given hw.cpufamily.
 * Used in the model name field.
 */
static const char *
arm64_cpu_name_string(uint32_t cpufamily)
{
    switch (cpufamily) {
        case CPUFAMILY_ARM_SWIFT:               return "Apple Swift (A6)";
        case CPUFAMILY_ARM_CYCLONE:             return "Apple Cyclone (A7)";
        case CPUFAMILY_ARM_TYPHOON:             return "Apple Typhoon (A8)";
        case CPUFAMILY_ARM_TWISTER:             return "Apple Twister (A9)";
        case CPUFAMILY_ARM_HURRICANE:           return "Apple Hurricane (A10)";
        case CPUFAMILY_ARM_MONSOON_MISTRAL:     return "Apple Monsoon/Mistral (A11)";
        case CPUFAMILY_ARM_VORTEX_TEMPEST:      return "Apple Vortex/Tempest (A12)";
        case CPUFAMILY_ARM_LIGHTNING_THUNDER:   return "Apple Lightning/Thunder (A13)";
        case CPUFAMILY_ARM_FIRESTORM_ICESTORM:  return "Apple Firestorm/Icestorm (M1)";
        case CPUFAMILY_ARM_AVALANCHE_BLIZZARD:  return "Apple Avalanche/Blizzard (M2)";
        case CPUFAMILY_ARM_EVEREST_SAWTOOTH:    return "Apple Everest/Sawtooth (M3)";
        case CPUFAMILY_ARM_IBIZA:               return "Apple Ibiza (M4)";
        default:                                return "Apple ARM64";
    }
}

/*
 * is_amd_cpu() — always FALSE on ARM64.
 */
boolean_t
is_amd_cpu(void)
{
    return FALSE;
}

/*
 * is_intel_cpu() — always FALSE on ARM64.
 */
boolean_t
is_intel_cpu(void)
{
    return FALSE;
}

/*
 * extract_bitfield() — portable implementation, no x86 ASM.
 */
uint32_t
extract_bitfield(uint32_t infield, uint32_t width, uint32_t offset)
{
    uint32_t bitmask;

    if ((offset + width) == 32) {
        bitmask = (0xFFFFFFFF << offset);
    } else {
        bitmask = (0xFFFFFFFF << offset) ^ (0xFFFFFFFF << (offset + width));
    }

    return (infield & bitmask) >> offset;
}

/*
 * get_bitfield_width() — portable implementation using clz
 * instead of the x86 BSR instruction.
 */
uint32_t
get_bitfield_width(uint32_t number)
{
    if (number == 0) {
        return 0;
    }
    number--;
    if (number == 0) {
        return 0;
    }
    /* __builtin_clz is available on clang/ARM64 */
    return (uint32_t)(32 - (int)__builtin_clz(number));
}

/*
 * Write protection via CR0 is an x86-specific concept.
 * These are no-ops on ARM64.
 */
uint32_t
disable_write_protection(void)
{
    return 0;
}

uint32_t
enable_write_protection(void)
{
    return 0;
}

/*
 * MSR microcode versioning is x86-specific.
 * No equivalent exists on ARM64; return 0.
 */
uint32_t
get_microcode_version(void)
{
    return 0;
}

uint32_t
set_microcode_version(void)
{
    return 0;
}

/*
 * arm64_sysctl_int()
 *
 * Helper to read an integer sysctl by name.
 * Returns 0 on failure.
 */
static int __attribute__((unused))
arm64_sysctl_int(const char *name)
{
    int val = 0;
    size_t len = sizeof(val);
    sysctlbyname(name, &val, &len, NULL, 0);
    return val;
}

/*
 * get_cpu_flags()
 *
 * Returns a space-separated string of ARM64 CPU feature flags
 * in Linux /proc/cpuinfo format, populated from hw.optional.arm.*
 * sysctls.
 *
 * Linux ARM64 feature flag names are used verbatim for
 * compatibility with tooling that parses /proc/cpuinfo.
 */
char *
get_cpu_flags(void)
{
    /*
     * Static buffer — safe in kext context since this is
     * called once per procfs read and not reentrant in practice.
     * Size is generous enough for all known Apple Silicon flags.
     */
    static char flags[512];
    flags[0] = '\0';

    /*
     * Map of hw.optional.arm.* sysctl names to their Linux
     * /proc/cpuinfo feature flag equivalents.
     *
     * Flags follow the naming convention used by the Linux ARM64
     * kernel (arch/arm64/kernel/cpuinfo.c).
     */
    static const struct {
        const char *sysctl;
        const char *flag;
    } feature_map[] = {
        /* Floating point and SIMD */
        { "hw.optional.floatingpoint",          "fp"        },
        { "hw.optional.arm.AdvSIMD",            "asimd"     },
        { "hw.optional.arm.AdvSIMD_HPFPCvt",    "asimdhp"   },
        { "hw.optional.arm.FEAT_FP16",          "fphp"      },
        /* Event stream */
        { "hw.optional.arm.FEAT_ECV",           "evtstrm"   },
        /* Cryptography */
        { "hw.optional.arm.FEAT_AES",           "aes"       },
        { "hw.optional.arm.FEAT_PMULL",         "pmull"     },
        { "hw.optional.arm.FEAT_SHA1",          "sha1"      },
        { "hw.optional.arm.FEAT_SHA256",        "sha2"      },
        { "hw.optional.arm.FEAT_SHA512",        "sha512"    },
        { "hw.optional.arm.FEAT_SHA3",          "sha3"      },
        /* CRC32 */
        { "hw.optional.arm.FEAT_CRC32",         "crc32"     },
        /* Atomics */
        { "hw.optional.arm.FEAT_LSE",           "atomics"   },
        { "hw.optional.arm.FEAT_LSE2",          "uscat"     },
        /* Pointer authentication */
        { "hw.optional.arm.FEAT_PAuth",         "paca"      },
        { "hw.optional.arm.FEAT_PAuth",         "pacg"      },
        { "hw.optional.arm.FEAT_PACIMP",        "ilrcpc"    },
        /* Load-acquire RCpc */
        { "hw.optional.arm.FEAT_LRCPC",         "lrcpc"     },
        { "hw.optional.arm.FEAT_LRCPC2",        "ilrcpc"    },
        /* Data cache clean to PoP */
        { "hw.optional.arm.FEAT_DPB",           "dcpop"     },
        { "hw.optional.arm.FEAT_DPB2",          "dcpodp"    },
        /* JSCVT / FCMA */
        { "hw.optional.arm.FEAT_JSCVT",         "jscvt"     },
        { "hw.optional.arm.FEAT_FCMA",          "fcma"      },
        /* Speculation / security */
        { "hw.optional.arm.FEAT_SB",            "sb"        },
        { "hw.optional.arm.FEAT_SSBS",          "ssbs"      },
        { "hw.optional.arm.FEAT_CSV2",          "csv2"      },
        { "hw.optional.arm.FEAT_CSV3",          "csv3"      },
        { "hw.optional.arm.FEAT_DIT",           "dit"       },
        /* CPUID visibility */
        { "hw.optional.armv8_gpi",              "cpuid"     },
        /* Float rounding */
        { "hw.optional.arm.FEAT_FRINTTS",       "frint"     },
        /* Dot product / FHM */
        { "hw.optional.arm.FEAT_DotProd",       "asimddp"   },
        { "hw.optional.arm.FEAT_FHM",           "asimdfhm"  },
        /* Flag manipulation */
        { "hw.optional.arm.FEAT_FlagM",         "flagm"     },
        { "hw.optional.arm.FEAT_FlagM2",        "flagm2"    },
        /* RDM */
        { "hw.optional.arm.FEAT_RDM",           "asimdrdm"  },
        /* BTI */
        { "hw.optional.arm.FEAT_BTI",           "bti"       },
        /* BF16 */
        { "hw.optional.arm.FEAT_BF16",          "bf16"      },
        /* I8MM */
        { "hw.optional.arm.FEAT_I8MM",          "i8mm"      },
        /* SVE / SME — present for completeness, disabled on M1 */
        { "hw.optional.arm.FEAT_SME",           "sme"       },
        { "hw.optional.arm.FEAT_SME2",          "sme2"      },
    };

    size_t map_count = sizeof(feature_map) / sizeof(feature_map[0]);

    /*
     * Deduplicate: some flags appear twice in the map (e.g. ilrcpc).
     * We track which flag strings we've already appended.
     */
    const char *emitted[map_count];
    size_t emitted_count = 0;

    for (size_t i = 0; i < map_count; i++) {
        int val = 0;
        size_t len = sizeof(val);
        if (sysctlbyname(feature_map[i].sysctl, &val, &len, NULL, 0) != 0
            || val == 0) {
            continue;
        }

        /* Check for duplicates */
        boolean_t already = FALSE;
        for (size_t j = 0; j < emitted_count; j++) {
            if (strcmp(emitted[j], feature_map[i].flag) == 0) {
                already = TRUE;
                break;
            }
        }
        if (already) continue;

        strlcat(flags, feature_map[i].flag, sizeof(flags));
        strlcat(flags, " ", sizeof(flags));
        emitted[emitted_count++] = feature_map[i].flag;
    }

    /* Trim trailing space */
    size_t len = strlen(flags);
    if (len > 0 && flags[len - 1] == ' ') {
        flags[len - 1] = '\0';
    }

    return flags;
}

/*
 * get_cpu_ext_flags()
 *
 * Extended flags — no Linux /proc/cpuinfo equivalent on ARM64.
 * The Features line covers everything. Return empty string.
 */
char *
get_cpu_ext_flags(void)
{
    static char flags[1] = "";
    return flags;
}

/*
 * get_leaf7_flags()
 *
 * x86 CPUID leaf 7 features — not applicable on ARM64.
 */
char *
get_leaf7_flags(void)
{
    static char flags[1] = "";
    return flags;
}

/*
 * get_leaf7_ext_flags()
 *
 * x86 CPUID leaf 7 extended features — not applicable on ARM64.
 */
char *
get_leaf7_ext_flags(void)
{
    static char flags[1] = "";
    return flags;
}

/*
 * arm64_cpu_implementer()
 *
 * Returns the ARM implementer code as a hex string.
 * Apple's assigned implementer ID is 0x61.
 */
const char *
arm64_cpu_implementer(void)
{
    return "0x61";
}

/*
 * arm64_cpu_architecture()
 *
 * Returns the ARM architecture version string.
 * All Apple Silicon is ARMv8 or later; report as "8"
 * per Linux convention (architecture field does not
 * increment beyond 8 in /proc/cpuinfo for AArch64).
 */
const char *
arm64_cpu_architecture(void)
{
    return "8";
}

/*
 * arm64_cpu_variant()
 *
 * Returns the CPU variant as a hex string.
 * On Apple Silicon this is always 0x0 since Apple
 * does not expose MIDR_EL1 variant bits to software.
 */
const char *
arm64_cpu_variant(void)
{
    return "0x0";
}

/*
 * arm64_cpu_part()
 *
 * Returns the CPU part number as a hex string derived
 * from hw.cpufamily.
 */
const char *
arm64_cpu_part(void)
{
    uint32_t cpufamily = 0;
    size_t len = sizeof(cpufamily);
    sysctlbyname("hw.cpufamily", &cpufamily, &len, NULL, 0);
    return arm64_cpu_part_string(cpufamily);
}

/*
 * arm64_cpu_revision()
 *
 * Returns the CPU revision as a decimal string.
 * Apple Silicon does not expose MIDR_EL1 revision bits;
 * report as 0 per convention.
 */
const char *
arm64_cpu_revision(void)
{
    return "0";
}

/*
 * arm64_cpu_name()
 *
 * Returns a human-readable name for the CPU derived
 * from hw.cpufamily, for use in the model name field.
 */
const char *
arm64_cpu_name(void)
{
    uint32_t cpufamily = 0;
    size_t len = sizeof(cpufamily);
    sysctlbyname("hw.cpufamily", &cpufamily, &len, NULL, 0);
    return arm64_cpu_name_string(cpufamily);
}

/*
 * arm64_bogomips()
 *
 * Returns BogoMIPS as a formatted string.
 *
 * On Linux/ARM64, BogoMIPS = timer frequency / 500000 * 2.
 * Apple Silicon runs the ARM generic timer at 24 MHz, giving:
 *   24000000 / 500000 * 2 = 96.00 BogoMIPS (M1/M2/M3/M4)
 *
 * We read hw.tbfrequency (timebase frequency) to compute this
 * dynamically rather than hardcoding it.
 */
const char *
arm64_bogomips(void)
{
    static char bogomips_str[16];

    uint64_t tbfreq = 0;
    size_t len = sizeof(tbfreq);

    if (sysctlbyname("hw.tbfrequency", &tbfreq, &len, NULL, 0) != 0
        || tbfreq == 0) {
        /* Fallback: Apple Silicon timer is 24 MHz */
        tbfreq = 24000000ULL;
    }

    /*
     * Linux BogoMIPS formula for ARM64:
     *   bogomips = loops_per_jiffy * HZ * 2 / 1000000
     * where loops_per_jiffy ≈ timer_freq / HZ
     * Simplified: bogomips = timer_freq * 2 / 1000000
     */
    uint64_t bogo_int  = (tbfreq * 2) / 1000000;
    uint64_t bogo_frac = ((tbfreq * 2) % 1000000) / 10000;

    snprintf(bogomips_str, sizeof(bogomips_str), "%llu.%02llu",
             bogo_int, bogo_frac);

    return bogomips_str;
}

#endif /* __arm64__ */
