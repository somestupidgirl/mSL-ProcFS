/*
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 *
 * procfs_linux.c
 *
 * Linux-compatible features.
 *
 * ARM64 support added 2024. On ARM64, procfs_docpuinfo() emits
 * output in Linux AArch64 /proc/cpuinfo format rather than the
 * x86 format, using hw.optional.arm.* sysctls and hw.cpufamily
 * for CPU identification.
 */
#include <stdint.h>
#include <string.h>
#if defined(__x86_64__)
#include <i386/cpuid.h>
#include <i386/tsc.h>
#else
#include <arm/cpuid.h>
#endif
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <libkern/OSMalloc.h>
#include <libkern/version.h>
#include <mach/machine.h>
#include <mach/mach_types.h>
#include <mach/processor_info.h>
#include <mach/thread_act.h>
#include <mach/thread_status.h>
#if defined(__arm64__) || defined(__aarch64__)
#include <mach/arm/thread_status.h>
#elif defined(__x86_64__)
#include <mach/i386/thread_status.h>
#endif
#include <os/log.h>
#include <ptrauth.h>
#include <sys/disklabel.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#if defined (__x86_64__)
#include <i386/proc_reg.h>
#else
#include <arm/proc_reg.h>
#endif
#include <sys/mount.h>
#include <sys/queue.h>
#include <sys/kauth.h>
#include <sys/sbuf.h>
#include <sys/sysctl.h>
#include <sys/types.h>
#if defined(__x86_64__)
#include <mach/i386/vm_param.h>
#else
#include <mach/vm_param.h>
#endif
#include <mach/vm_prot.h>

#include <mach/thread_info.h>
#include <mach/vm_statistics.h>
#include <sys/proc.h>
#include <sys/proc_info.h>

#include <bsdcompat/sys/malloc.h>

#include <fs/procfs/procfs.h>
#include <fs/procfs/procfs_iokit.h>
#include <fs/procfs/procfs_ctl.h>
#include "lib/symbols.h"

#include "lib/cpu.h"
#include "lib/symbols.h"

#pragma mark -
#pragma mark Common Definitions and Macros

/*
 * A buffer size.
 */
#define LBFSZ           (8 * 1024)

/*
 * Convert pages to bytes.
 */
#define PGTOB(p) \
        ((unsigned long)(p) << PAGE_SHIFT)
/*
 * Convert pages to kbytes
 */
#define PGTOKB(p) \
        ((unsigned long)(p) << (PAGE_SHIFT - 10))
/*
 * Convert bytes to pages.
 */
#define BTOPG(b) \
        ((b) >> PAGE_SHIFT)
/*
 * Convert bytes to kbytes
 */
#define BTOKB(b) \
        ((b) >> 10)
/*
 * XXX
 */
#define TVTOJ(x) \
        ((x)->tv_sec * 100UL + (x)->tv_usec / 10000)

/*
 * Various conversion macros
 */
#define T2J(x) ((long)(((x) * 100ULL) / (stathz ? stathz : hz)))    /* ticks to jiffies */
#define T2CS(x) ((unsigned long)(((x) * 100ULL) / (stathz ? stathz : hz)))  /* ticks to centiseconds */
#define T2S(x) ((x) / (stathz ? stathz : hz))       /* ticks to seconds */
#define B2K(x) ((x) >> 10)              /* bytes to kbytes */
#define B2P(x) ((x) >> PAGE_SHIFT)          /* bytes to pages */
#define P2B(x) ((x) << PAGE_SHIFT)          /* pages to bytes */
#define P2K(x) ((x) << (PAGE_SHIFT - 10))       /* pages to kbytes */
#define TV2J(x) ((x)->tv_sec * 100UL + (x)->tv_usec / 10000)

/**
 * @brief Mapping of ki_stat in struct kinfo_proc to the linux state
 *
 * The linux procfs state field displays one of the characters RSDZTW to
 * denote running, sleeping in an interruptible wait, waiting in an
 * uninterruptible disk sleep, a zombie process, process is being traced
 * or stopped, or process is paging respectively.
 *
 * Our struct kinfo_proc contains the variable ki_stat which contains a
 * value out of SIDL, SRUN, SSLEEP, SSTOP, SZOMB, SWAIT and SLOCK.
 *
 * This character array is used with ki_stati-1 as an index and tries to
 * map our states to suitable linux states.
 */
static char linux_state[] = "RRSTZDD";

#pragma mark -
#pragma mark Linux-emulation functions

/*
 * Linux-compatible /proc/cpuinfo
 */
int
procfs_docpuinfo(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int len = 0, xlen = 0;
    vm_offset_t pageno, uva;
    off_t page_offset = 0;
    size_t buffer_size = 0;
    char *buffer;

    /*
     * Overall processor count for the current CPU.
     *
     * Not to be conflated with cpu_cores (number of cores)
     * as these are not the same.
     */
    /* processor_count is a private symbol - use sysctlbyname instead */
    uint32_t max_cpus = 1;
    if (_processor_count != NULL) {
        max_cpus = processor_count;
    } else {
        size_t max_cpus_size = sizeof(max_cpus);
        sysctlbyname("hw.logicalcpu", &max_cpus, &max_cpus_size, NULL, 0);
    }
    uint32_t cnt_cpus = 0;

    /*
     * Initialize the processor counter.
     * This should always begin at 0 and
     * add 1 for each loop according to the
     * number of processors present.
     */
    buffer_size = (LBFSZ * 4);
    buffer = malloc(buffer_size, M_TEMP, M_WAITOK);

    /*
     * Get the userspace virtual address.
     */
    uva = uio_offset(uio);

    /*
     * Get the page number of this segment.
     */
    pageno = trunc_page(uva);
    page_offset = uva - pageno;

/* ============================================================
 * x86_64 /proc/cpuinfo
 * ============================================================ */
#if defined(__x86_64__)

    /*
     * Initialize the TSC frequency variables.
     */
    uint64_t freq = tscFreq;
    int fqmhz = 0, fqkhz = 0;

    /* 
     * Set the TSC frequency variables
     */
    if (freq != 0) {
        fqmhz = (freq + 4999) / 1000000;
        fqkhz = ((freq + 4999) / 10000) % 100;
    }

    /*
     * The apicid variable begins at 0 and get increased
     * by 2 for each loop until the number becomes greater
     * than max_cpus, in which case the loop resets the
     * variable to a value of 1 and then contines increasing
     * that number by 2 for each loop.
     */
    int apicid = 0, initial_apicid = 0;

    /*
     * The core id should always start at 0.
     */
    int core_id = 0;

    /*
     * Here we can utilize the i386_cpu_info structure in i386/cpuid.h
     * to get the information we need. The cpuid_info() function sets up
     * the i386_cpu_info structure and returns a pointer to the structure.
     */
    char *vendor_id      = cpuid_info()->cpuid_vendor;
    uint8_t cpu_family   = cpuid_info()->cpuid_family;
    uint8_t model        = cpuid_info()->cpuid_model
                         + (cpuid_info()->cpuid_extmodel << 4);
    char *model_name     = cpuid_info()->cpuid_brand_string;
    int microcode        = (int)get_microcode_version();
    uint32_t cache_size  = cpuid_info()->cpuid_cache_size;
    uint8_t stepping     = cpuid_info()->cpuid_stepping;
    uint32_t cpu_cores   = cpuid_info()->core_count;
    uint32_t cpuid_level = cpu_cores;
    uint32_t tlb_size    = cpuid_info()->cache_linesize * 40;
    uint32_t clflush_size    = cpuid_info()->cache_linesize;
    uint32_t cache_alignment = clflush_size;
    uint32_t addr_bits_phys  = cpuid_info()->cpuid_address_bits_physical;
    uint32_t addr_bits_virt  = cpuid_info()->cpuid_address_bits_virtual;

    /*
     * Check if the FPU feature is present.
     */
    char *fpu, *fpu_exception;
    if (cpuid_info()->cpuid_features & CPUID_FEATURE_FPU) {
        fpu = "yes";
        fpu_exception = "yes";
    } else {
        fpu = "no";
        fpu_exception = "no";
    }

    /*
     * Check for CPU write protection.
     */
    char *wp;
    if (get_cr0() & CR0_WP) {
        wp = "yes";
    } else {
        wp = "no";
    }

    /*
     * CPU flags.
     *
     * Bug note: not all of the flag variables stick
     * after the first iteration of the main loop if we,
     * define them here, likely due to a memory-related
     * issue that has been pointed out to me, though I'm
     * still unsure on how to go about fixing it.
     * For now, we'll declare the pointer variables here
     * and fetch the flags inside the loop.
     */
    char *cpuflags, *cpuextflags, *leaf7flags, *leaf7extflags, *amdflags2;

    /* Power-management line and CPU bug classes (CPUID/MSR-derived, cpu.c).
     * All bugs are reported through x86_bugs; x86_64_bugs is kept as a second
     * (currently empty) slot so the "bugs" format string stays unchanged. */
    char *pm = get_pm_flags();
    char *x86_bugs = get_cpu_bugs();
    char *x86_64_bugs = "";

    /*
     * The main loop iterates over each processor number stored in the
     * max_cpus variable and prints out a list of data for each processor.
     *
     * For example:
     *
     * If your CPU only has one processor, it should print only a single list.
     * If your CPU has two processors, it will print out a set of two lists,
     * and so on and so forth. My CPU processor number is 32 so I get 32 lists
     * with processor numbers starting from 0 - 31.
     */
    while (cnt_cpus < max_cpus) {
        /*
         * Fetch the CPU flags.
         */
        cpuflags     = get_cpu_flags();
        /* On AMD, the 0x80000001 flags come from the AMD getters (correct Linux
         * names, EDX + ECX) instead of the generic ext getter, avoiding both the
         * Intel-style names (xd/em64t/...) and duplicate entries. */
        cpuextflags  = is_amd_cpu() ? get_amd_feature_flags() : get_cpu_ext_flags();
        amdflags2    = get_amd_feature2_flags();   /* empty on Intel */
        leaf7flags   = get_leaf7_flags();
        leaf7extflags = get_leaf7_ext_flags();

        if (cnt_cpus <= max_cpus) {
            /* 
             * The data which to copy over to userspace.
             */
            len += snprintf(buffer, buffer_size,
                "processor\t\t: %u\n"
                "vendor_id\t\t: %s\n"
                "cpu family\t\t: %u\n"
                "model\t\t\t: %u\n"
                "model name\t\t: %s\n"
                "microcode\t\t: 0x%07x\n"
                "stepping\t\t: %u\n"
                "cpu MHz\t\t\t: %d.%02d\n"
                "cache size\t\t: %d KB\n"
                "physical id\t\t: %u\n"
                "siblings\t\t: %u\n"
                "core id\t\t\t: %d\n"
                "cpu cores\t\t: %u\n"
                "apicid\t\t\t: %u\n"
                "initial apicid\t\t: %u\n"
                "fpu\t\t\t: %s\n"
                "fpu_exception\t\t: %s\n"
                "cpuid level\t\t: %u\n"
                "wp\t\t\t: %s\n"
                "flags\t\t\t: %s%s%s%s%s\n"
                "bugs\t\t\t: %s%s\n"
                "bogomips\t\t: %d.%02d\n"
                "TLB size\t\t: %u 4K pages\n"
                "clflush_size\t\t: %u\n"
                "cache_alignment\t\t: %d\n"
                "address sizes\t\t: %d bits physical, %d bits virtual\n"
                "power management\t: %s\n\n",
                cnt_cpus,
                vendor_id,
                cpu_family,
                model,
                model_name,
                microcode,
                stepping,
                fqmhz, fqkhz,
                cache_size,
                0,
                max_cpus,
                core_id,
                cpu_cores,
                apicid,
                initial_apicid,
                fpu,
                fpu_exception,
                cpuid_level,
                wp,
                cpuflags, cpuextflags, amdflags2, leaf7flags, leaf7extflags,
                x86_bugs, x86_64_bugs,
                fqmhz * 2, fqkhz,
                tlb_size,
                clflush_size,
                cache_alignment,
                addr_bits_phys,
                addr_bits_virt,
                pm
            );

            /*
             * How many bytes to copy.
             */
            xlen = (len - page_offset);

            /*
             * Do the move into userspace.
             */
            uiomove((const char *)buffer, xlen, uio);

            /* 
             * Set len back to 0 before entering into the next loop.
             */
            if (len != 0) {
                len = 0;
            }

            /*
             * Increase by 2 for each loop.
             */
            apicid += 2;
            if ((int)apicid >= (int)max_cpus) {
                /* If the number exceeds max_cpus, reset to 1. */
                apicid = 1;
            }

            /*
             * The initial apicid is the same as apicid.
             */
            initial_apicid = apicid;

            /*
             * Add 1 to the CPU counter for each iteration.
             */
            cnt_cpus++;

            /*
             * Add 1 to the core_id counter for each iteration.
             */
            core_id++;

            /*
             * Reset the core_id counter if it exceeds the maximum number of cores.
             */
            if ((int)core_id > (int)cpu_cores - 1) {
                core_id = 0;
            }
            if (_processor_count != NULL && max_cpus != processor_count) {
                max_cpus = processor_count;
            }
        } else if (cnt_cpus > max_cpus) {
            break;
        }
    }

/* ============================================================
 * ARM64 /proc/cpuinfo
 *
 * Produces one entry per logical CPU in Linux AArch64 format:
 *
 *   processor       : 0
 *   BogoMIPS        : 48.00
 *   Features        : fp asimd aes pmull sha1 sha2 crc32 ...
 *   CPU implementer : 0x61
 *   CPU architecture: 8
 *   CPU variant     : 0x0
 *   CPU part        : 0x22
 *   CPU revision    : 0
 *
 * ============================================================ */
#elif defined(__arm64__) || defined(__aarch64__)

    const char *bogomips     = arm64_bogomips();
    const char *features     = get_cpu_flags();
    const char *implementer  = arm64_cpu_implementer();
    const char *architecture = arm64_cpu_architecture();
    const char *variant      = arm64_cpu_variant();
    const char *part         = arm64_cpu_part();
    const char *revision     = arm64_cpu_revision();

    while (cnt_cpus < max_cpus) {
        if (cnt_cpus <= max_cpus) {
            len += snprintf(buffer, buffer_size,
                "processor\t\t: %u\n"
                "BogoMIPS\t\t: %s\n"
                "Features\t\t: %s\n"
                "CPU implementer\t\t: %s\n"
                "CPU architecture\t: %s\n"
                "CPU variant\t\t: %s\n"
                "CPU part\t\t: %s\n"
                "CPU revision\t\t: %s\n\n",
                cnt_cpus,
                bogomips,
                features,
                implementer,
                architecture,
                variant,
                part,
                revision
            );

            xlen = (len - page_offset);
            uiomove((const char *)buffer, xlen, uio);

            if (len != 0) len = 0;

            cnt_cpus++;

            /*
             * Reset the max_cpus variable at the end of each loop.
             * An unknown bug is causing the variable to reduce the
             * count down to 23 unless we do this.
             */
            if (_processor_count != NULL && max_cpus != processor_count) {
                max_cpus = processor_count;
            }
        } else if (cnt_cpus > max_cpus) {
            /*
             * If the counter exceeds the processor count,
             * break the loop.
             */
            break;
        }
    }

#endif /* __x86_64__ / __arm64__ */

    free(buffer, M_TEMP);

    return 0;
}

/*
 * Load-average sampler for the loadavg node.
 *
 * A true run-queue load average is unreachable on arm64: averunnable,
 * compute_averunnable, host_statistics and processor_set_info are all stripped
 * from the kernel and unexported. What IS reachable is per-CPU tick counts via
 * processor_info(PROCESSOR_CPU_LOAD_INFO), given a processor_t from
 * cpu_to_processor() (resolved through libklookup). So we periodically sample
 * CPU utilisation and feed util*ncpu through the standard load-average EWMA,
 * writing the result into the local `averunnable` (lib/kern.c) the node reads.
 *
 * NOTE: this approximates CPU utilisation (it saturates near ncpu), not the
 * true run-queue length, so it under-reports an overloaded machine. It is a
 * best-effort stand-in for the otherwise-unavailable load average.
 */
extern kern_return_t processor_info(processor_t processor, processor_flavor_t flavor,
    host_t *host, processor_info_t info, mach_msg_type_number_t *count);

typedef processor_t (*cpu_to_processor_fn)(int);

#define LA_SAMPLE_SECS 5

/* exp(-5s/{60,300,900}s) * FSCALE - the classic 5-second-sample decay table. */
static const fixpt_t la_cexp[3] = { 1884, 2014, 2037 };

static thread_call_t        la_call;
static cpu_to_processor_fn  la_cpu_to_processor;
static int                  la_ncpu;
static uint64_t             la_busy;        /* cumulative across CPUs, last sample */
static uint64_t             la_idle;
static boolean_t            la_primed;

/*
 * Sum busy/idle ticks across all CPUs, then fold a utilisation-derived
 * instantaneous load into the three EWMAs.
 */
static void
la_sample(void)
{
    uint64_t busy = 0, idle = 0;

    for (int i = 0; i < la_ncpu; i++) {
        processor_t pr = la_cpu_to_processor(i);
        if (pr == PROCESSOR_NULL) {
            continue;
        }
        processor_cpu_load_info_data_t info;
        mach_msg_type_number_t count = PROCESSOR_CPU_LOAD_INFO_COUNT;
        host_t host;
        if (processor_info(pr, PROCESSOR_CPU_LOAD_INFO, &host,
                (processor_info_t)&info, &count) != KERN_SUCCESS) {
            continue;
        }
        busy += (uint64_t)info.cpu_ticks[CPU_STATE_USER] +
                info.cpu_ticks[CPU_STATE_SYSTEM] +
                info.cpu_ticks[CPU_STATE_NICE];
        idle += info.cpu_ticks[CPU_STATE_IDLE];
    }

    if (!la_primed) {
        la_busy = busy;
        la_idle = idle;
        la_primed = TRUE;
        return;
    }

    uint64_t dbusy = busy - la_busy;
    uint64_t didle = idle - la_idle;
    la_busy = busy;
    la_idle = idle;

    /* Instantaneous "load" = utilisation * ncpu, in fixed point. */
    uint64_t total = dbusy + didle;
    fixpt_t nrun = 0;
    if (total > 0) {
        nrun = (fixpt_t)((dbusy * (uint64_t)la_ncpu * FSCALE) / total);
    }

    for (int i = 0; i < 3; i++) {
        averunnable.ldavg[i] = (fixpt_t)(((uint64_t)la_cexp[i] * averunnable.ldavg[i] +
            (uint64_t)nrun * (FSCALE - la_cexp[i])) >> FSHIFT);
    }
    averunnable.fscale = FSCALE;
}

static void
la_timer(__unused thread_call_param_t a, __unused thread_call_param_t b)
{
    uint64_t deadline;

    la_sample();
    clock_interval_to_deadline(LA_SAMPLE_SECS, NSEC_PER_SEC, &deadline);
    thread_call_enter_delayed(la_call, deadline);
}

/*
 * Start the periodic sampler. No-op (loadavg values stay zero) if libklookup
 * could not resolve cpu_to_processor.
 */
void
procfs_loadavg_start(void)
{
    if (la_call != NULL || procfs_kl_cpu_to_processor == NULL) {
        return;
    }

    /* Sign the klookup-resolved address for the arm64e ABI (disc 0; the
     * assignment resigns to the call's type discriminator). */
    la_cpu_to_processor = ptrauth_sign_unauthenticated(procfs_kl_cpu_to_processor,
        ptrauth_key_function_pointer, 0);

    int ncpu = 0;
    size_t sz = sizeof(ncpu);
    if (sysctlbyname("hw.logicalcpu", &ncpu, &sz, NULL, 0) != 0 || ncpu <= 0) {
        ncpu = 1;
    }
    la_ncpu = ncpu;
    averunnable.fscale = FSCALE;

    la_call = thread_call_allocate(la_timer, NULL);
    if (la_call == NULL) {
        return;
    }

    la_sample();    /* prime the cumulative counters */

    uint64_t deadline;
    clock_interval_to_deadline(LA_SAMPLE_SECS, NSEC_PER_SEC, &deadline);
    thread_call_enter_delayed(la_call, deadline);
}

/*
 * Stop the sampler (kext unload).
 */
void
procfs_loadavg_stop(void)
{
    if (la_call != NULL) {
        thread_call_cancel(la_call);
        thread_call_free(la_call);
        la_call = NULL;
    }
}

/*
 * Linux-compatible /proc/loadavg
 */
int
procfs_doloadavg(__unused pfsnode_t *pnp, uio_t uio, vfs_context_t ctx)
{
    int error = 0;
    int len = 0, xlen = 0;
    vm_offset_t off = uio_offset(uio);
    vm_offset_t pgno = trunc_page(off);
    off_t pgoff = (off - pgno);

    char *buf = malloc(LBFSZ, M_TEMP, M_WAITOK);

    // Report the load averages from our local averunnable (lib/kern.c), which
    // procfs_loadavg_start()'s sampler keeps populated from per-CPU utilisation
    // (the kernel's own averunnable and every run-queue source are stripped on
    // arm64). When libklookup can't resolve cpu_to_processor the sampler never
    // runs and these stay 0.00.
    long fscale = averunnable.fscale > 0 ? averunnable.fscale : FSCALE;
    int load1  = (int)((uint64_t)averunnable.ldavg[0] * 100 / fscale);
    int load5  = (int)((uint64_t)averunnable.ldavg[1] * 100 / fscale);
    int load15 = (int)((uint64_t)averunnable.ldavg[2] * 100 / fscale);

    // Preferred: the procfsd daemon returns the kernel's true 1/5/15-minute load
    // averages (getloadavg), scaled x100. Without a daemon we keep the CPU-
    // utilisation approximation from the local averunnable above.
    uint32_t la[3] = { 0, 0, 0 };
    uint32_t got = 0;
    if (procfs_ctl_request(PROCFS_REQ_LOADAVG, 0, 0, &la, sizeof(la), &got) == 0 &&
        got == sizeof(la)) {
        load1  = (int)la[0];
        load5  = (int)la[1];
        load15 = (int)la[2];
    }

    int total_procs = procfs_get_process_count(vfs_context_ucred(ctx));
    int running = 1;
    int lastpid = 0;

    len = snprintf(buf, LBFSZ,
        "%d.%02d %d.%02d %d.%02d %d/%d %d\n",
        load1  / 100, load1  % 100,
        load5  / 100, load5  % 100,
        load15 / 100, load15 % 100,
        running,
        total_procs,
        lastpid
    );

    xlen = (len - pgoff);
    error = uiomove((const char *)buf, xlen, uio);

    free(buf, M_TEMP);

    return error;
}

/*
 * Linux-compatible /proc/stat. The cpu/cpuN lines (user/nice/system/idle ticks)
 * come from the same per-CPU source as the loadavg sampler:
 * processor_info(PROCESSOR_CPU_LOAD_INFO) on each processor_t from the
 * libklookup-resolved cpu_to_processor(). The macOS cpu_ticks are already in
 * 1/CLK_TCK units (USER_HZ jiffies), so they map straight onto the Linux
 * columns. btime comes from kern.boottime; interrupt/ctxt/fork counters have no
 * kernel-reachable source and are reported as 0.
 */
int
procfs_dostat(__unused pfsnode_t *pnp, uio_t uio, vfs_context_t ctx)
{
    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 2048, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    int    ncpu = 0;
    size_t sz   = sizeof(ncpu);
    if (sysctlbyname("hw.logicalcpu", &ncpu, &sz, NULL, 0) != 0 || ncpu <= 0) {
        ncpu = 1;
    }

    struct cpuline { uint64_t user, nice, sys, idle; };
    struct cpuline *cl = malloc((size_t)ncpu * sizeof(*cl), M_TEMP, M_WAITOK);
    if (cl == NULL) {
        sbuf_delete(&sb);
        return ENOMEM;
    }
    bzero(cl, (size_t)ncpu * sizeof(*cl));

    struct cpuline agg = { 0, 0, 0, 0 };
    if (la_cpu_to_processor != NULL) {
        for (int i = 0; i < ncpu; i++) {
            processor_t pr = la_cpu_to_processor(i);
            if (pr == PROCESSOR_NULL) {
                continue;
            }
            processor_cpu_load_info_data_t info;
            mach_msg_type_number_t count = PROCESSOR_CPU_LOAD_INFO_COUNT;
            host_t host;
            if (processor_info(pr, PROCESSOR_CPU_LOAD_INFO, &host,
                    (processor_info_t)&info, &count) != KERN_SUCCESS) {
                continue;
            }
            cl[i].user = info.cpu_ticks[CPU_STATE_USER];
            cl[i].nice = info.cpu_ticks[CPU_STATE_NICE];
            cl[i].sys  = info.cpu_ticks[CPU_STATE_SYSTEM];
            cl[i].idle = info.cpu_ticks[CPU_STATE_IDLE];
            agg.user += cl[i].user;
            agg.nice += cl[i].nice;
            agg.sys  += cl[i].sys;
            agg.idle += cl[i].idle;
        }
    }

    /* cpu line columns: user nice system idle iowait irq softirq steal guest guest_nice */
    sbuf_printf(&sb, "cpu  %llu %llu %llu %llu 0 0 0 0 0 0\n",
        (unsigned long long)agg.user, (unsigned long long)agg.nice,
        (unsigned long long)agg.sys,  (unsigned long long)agg.idle);
    for (int i = 0; i < ncpu; i++) {
        sbuf_printf(&sb, "cpu%d %llu %llu %llu %llu 0 0 0 0 0 0\n", i,
            (unsigned long long)cl[i].user, (unsigned long long)cl[i].nice,
            (unsigned long long)cl[i].sys,  (unsigned long long)cl[i].idle);
    }
    free(cl, M_TEMP);

    long long btime = 0;
    struct timeval bt;
    sz = sizeof(bt);
    if (sysctlbyname("kern.boottime", &bt, &sz, NULL, 0) == 0) {
        btime = (long long)bt.tv_sec;
    }

    int total_procs = procfs_get_process_count(vfs_context_ucred(ctx));

    sbuf_printf(&sb, "intr 0\n");
    sbuf_printf(&sb, "ctxt 0\n");
    sbuf_printf(&sb, "btime %lld\n", btime);
    sbuf_printf(&sb, "processes %d\n", total_procs);
    sbuf_printf(&sb, "procs_running 1\n");
    sbuf_printf(&sb, "procs_blocked 0\n");

    sbuf_finish(&sb);
    int error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * Linux-compatible /proc/vmstat. The VM page-count globals are stripped on
 * arm64, so the figures come from the procfsd daemon's
 * host_statistics64(HOST_VM_INFO64) (a vm_statistics64). macOS and Linux model
 * memory differently, so the macOS counters are mapped onto the closest Linux
 * keys, plus a few macOS-specific lines (nr_wired/nr_compressed/...). All counts
 * are in pages. Without a daemon every value reads 0.
 */
int
procfs_dovmstat(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    vm_statistics64_data_t vm;
    bzero(&vm, sizeof(vm));
    uint32_t got = 0;
    (void)procfs_ctl_request(PROCFS_REQ_VMSTAT, 0, 0, &vm, sizeof(vm), &got);

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 2048, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    sbuf_printf(&sb,
        "nr_free_pages %u\n"
        "nr_inactive_anon 0\n"
        "nr_active_anon 0\n"
        "nr_inactive_file %u\n"
        "nr_active_file %u\n"
        "nr_anon_pages %u\n"
        "nr_file_pages %u\n"
        "nr_wired %u\n"
        "nr_purgeable %u\n"
        "nr_speculative %u\n"
        "nr_throttled %u\n"
        "nr_compressed %u\n"
        "pgpgin %llu\n"
        "pgpgout %llu\n"
        "pswpin %llu\n"
        "pswpout %llu\n"
        "pgfault %llu\n"
        "pgmajfault %llu\n"
        "pgreactivate %llu\n"
        "cow_faults %llu\n"
        "decompressions %llu\n"
        "compressions %llu\n",
        (unsigned)vm.free_count,
        (unsigned)vm.inactive_count, (unsigned)vm.active_count,
        (unsigned)vm.internal_page_count, (unsigned)vm.external_page_count,
        (unsigned)vm.wire_count, (unsigned)vm.purgeable_count,
        (unsigned)vm.speculative_count, (unsigned)vm.throttled_count,
        (unsigned)vm.compressor_page_count,
        (unsigned long long)vm.pageins, (unsigned long long)vm.pageouts,
        (unsigned long long)vm.swapins, (unsigned long long)vm.swapouts,
        (unsigned long long)vm.faults, (unsigned long long)vm.pageins,
        (unsigned long long)vm.reactivations, (unsigned long long)vm.cow_faults,
        (unsigned long long)vm.decompressions, (unsigned long long)vm.compressions);

    sbuf_finish(&sb);
    int error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * Linux-compatible /proc/meminfo, modelled on FreeBSD's linprocfs_domeminfo()
 * (sys/compat/linux/linprocfs/linprocfs.c) - same field set and "%9lu kB"
 * layout, and the same "all memory that isn't wired down is free" estimate
 * (FreeBSD: memused = vm_wire_count * PAGE_SIZE; memfree = memtotal - memused).
 *
 * Data sources differ on macOS. MemTotal comes from the hw.memsize sysctl
 * (readable from kernel context). The wired-page count comes from the kernel's
 * vm_page_wire_count global, resolved through libklookup (it survives in the
 * arm64 kernel symbol table; the vm.* page-count sysctls are not readable from
 * kernel context, and most vm_page_*_count globals are stripped). Cached,
 * Buffers and swap have no comparable source on arm64 and are reported as 0
 * (Buffers is 0 on FreeBSD too). If libklookup did not resolve the wired count,
 * MemFree is reported as 0 rather than guessed.
 */
int
procfs_domeminfo(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    uint64_t memtotal = 0;                       /* total memory in bytes */
    size_t   sz = sizeof(memtotal);
    (void)sysctlbyname("hw.memsize", &memtotal, &sz, NULL, 0);

    unsigned long memfree = 0;
    if (procfs_vm_page_wire_count != NULL) {
        uint64_t wired = (uint64_t)*procfs_vm_page_wire_count * PAGE_SIZE;
        memfree = (unsigned long)(memtotal > wired ? memtotal - wired : 0);
    }

    unsigned long      cached    = 0;
    unsigned long      buffers   = 0;
    unsigned long long swaptotal = 0;
    unsigned long long swapfree  = 0;

    char buf[512];
    struct sbuf sb;
    if (sbuf_new(&sb, buf, sizeof(buf), SBUF_FIXEDLEN) == NULL) {
        return ENOMEM;
    }

    sbuf_printf(&sb,
        "MemTotal: %9lu kB\n"
        "MemFree:  %9lu kB\n"
        "MemShared:%9lu kB\n"
        "Buffers:  %9lu kB\n"
        "Cached:   %9lu kB\n"
        "SwapTotal:%9llu kB\n"
        "SwapFree: %9llu kB\n",
        (unsigned long)B2K(memtotal), (unsigned long)B2K(memfree), 0UL,
        (unsigned long)B2K(buffers), (unsigned long)B2K(cached),
        (unsigned long long)B2K(swaptotal), (unsigned long long)B2K(swapfree));
    sbuf_finish(&sb);

    int error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * Linux-compatible /proc/partitions. Linux lists every block device; macOS has
 * no kext-linkable way to enumerate raw/unmounted disks (that lives in IOKit),
 * so we list the mounted block-device filesystems instead - real major/minor,
 * block counts and names for everything backed by a /dev node. Each mounted
 * volume is reported (on APFS that means the container's volumes), which is the
 * closest faithful equivalent reachable from a VFS-only kext.
 */
struct procfs_part_ctx {
    struct sbuf *sb;
};

/*
 * vfs_iterate() callout: emit one partitions line per mounted /dev device.
 * Runs with the mount referenced; uses only vfs_statfs() (no blocking I/O or
 * vnode lookups) so it is safe inside the iteration.
 */
static int
procfs_partitions_cb(mount_t mp, void *arg)
{
    struct procfs_part_ctx *pc = (struct procfs_part_ctx *)arg;
    struct vfsstatfs *st = vfs_statfs(mp);

    if (st == NULL || strncmp(st->f_mntfromname, "/dev/", 5) != 0) {
        return VFS_RETURNED;        /* skip non-block-device mounts */
    }

    dev_t    dev    = (dev_t)st->f_fsid.val[0];
    uint64_t blocks = ((uint64_t)st->f_blocks * st->f_bsize) >> 10;  /* 1K blocks */

    sbuf_printf(pc->sb, "%4d %7d %10llu %s\n",
        major(dev), minor(dev), (unsigned long long)blocks,
        st->f_mntfromname + 5 /* strip "/dev/" */);

    return VFS_RETURNED;
}

int
procfs_dopartitions(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 2048, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    sbuf_printf(&sb, "major minor  #blocks  name\n\n");

    /*
     * Prefer true block-device enumeration via IOKit (procfs_iokit.cpp): every
     * whole disk and partition, mounted or not - the real Linux /proc/partitions
     * semantics. Fall back to the mounted-filesystem list (vfs_iterate) if IOKit
     * matching fails.
     */
    enum { PROCFS_MAX_PARTS = 128 };
    struct procfs_partition *parts = (struct procfs_partition *)
        OSMalloc(PROCFS_MAX_PARTS * sizeof(struct procfs_partition), procfs_osmalloc_tag);
    int n = 0;
    if (parts != NULL &&
        procfs_iokit_get_partitions(parts, PROCFS_MAX_PARTS, &n) == 0 && n > 0) {
        for (int i = 0; i < n; i++) {
            sbuf_printf(&sb, "%4u %7u %10llu %s\n",
                parts[i].major, parts[i].minor,
                (unsigned long long)(parts[i].size / 1024), parts[i].name);
        }
    } else {
        struct procfs_part_ctx pc = { &sb };
        vfs_iterate(0, procfs_partitions_cb, &pc);
    }
    if (parts != NULL) {
        OSFree(parts, PROCFS_MAX_PARTS * sizeof(struct procfs_partition), procfs_osmalloc_tag);
    }

    sbuf_finish(&sb);
    int error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * Linux-compatible /proc/mtab (the /etc/mtab equivalent): one line per mounted
 * filesystem, "device mountpoint fstype options 0 0". Enumerated the same way as
 * partitions (vfs_iterate + vfs_statfs), but lists every mount, not just block
 * devices, with the mount options decoded from f_flags.
 */
static void
procfs_mtab_options(uint64_t flags, char *out, size_t cap)
{
    static const struct { uint64_t flag; const char *opt; } tab[] = {
        { MNT_NOSUID,      ",nosuid"     },
        { MNT_NODEV,       ",nodev"      },
        { MNT_NOEXEC,      ",noexec"     },
        { MNT_SYNCHRONOUS, ",sync"       },
        { MNT_ASYNC,       ",async"      },
        { MNT_NOATIME,     ",noatime"    },
        { MNT_UNION,       ",union"      },
        { MNT_JOURNALED,   ",journaled"  },
        { MNT_LOCAL,       ",local"      },
        { MNT_DONTBROWSE,  ",nobrowse"   },
    };

    strlcpy(out, (flags & MNT_RDONLY) ? "ro" : "rw", cap);
    for (size_t i = 0; i < sizeof(tab) / sizeof(tab[0]); i++) {
        if (flags & tab[i].flag) {
            strlcat(out, tab[i].opt, cap);
        }
    }
}

struct procfs_mtab_ctx {
    struct sbuf *sb;
};

static int
procfs_mtab_cb(mount_t mp, void *arg)
{
    struct procfs_mtab_ctx *mc = (struct procfs_mtab_ctx *)arg;
    struct vfsstatfs *st = vfs_statfs(mp);

    if (st == NULL) {
        return VFS_RETURNED;
    }

    char opts[160];
    procfs_mtab_options(st->f_flags, opts, sizeof(opts));

    sbuf_printf(mc->sb, "%s %s %s %s 0 0\n",
        st->f_mntfromname, st->f_mntonname, st->f_fstypename, opts);

    return VFS_RETURNED;
}

int
procfs_domtab(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 4096, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    struct procfs_mtab_ctx mc = { &sb };
    vfs_iterate(0, procfs_mtab_cb, &mc);

    sbuf_finish(&sb);
    int error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);

    return error;
}

/*
 * Linux-compatible /proc/<pid>/maps. Shares the VM-region walk in procfs_map.c
 * (procfs_map_render) with the NetBSD-format `map` node; only the per-region
 * line format is Linux-specific:
 *   start-end perms offset dev inode path
 * The dev/inode/path columns are reported as "00:00 0" with no path, since the
 * region's backing file is not reachable here (see procfs_map.c).
 */
static void
procfs_maps_fmt_linux(struct sbuf *sb, const struct procfs_region *r)
{
    sbuf_printf(sb, "%016llx-%016llx %c%c%c%c %016llx 00:00 0 \n",
        (unsigned long long)r->start, (unsigned long long)r->end,
        (r->prot & VM_PROT_READ)    ? 'r' : '-',
        (r->prot & VM_PROT_WRITE)   ? 'w' : '-',
        (r->prot & VM_PROT_EXECUTE) ? 'x' : '-',
        r->shared ? 's' : 'p',
        (unsigned long long)r->offset);
}

int
procfs_domaps(pfsnode_t *pnp, uio_t uio, vfs_context_t ctx)
{
    return procfs_map_render(pnp, uio, ctx, procfs_maps_fmt_linux);
}

/*
 * Linux-compatible per-thread files: /proc/<pid>/task/<tid>/{comm,stat,status,
 * sched}. The per-thread data (run state, user/system time, name, priority,
 * policy) comes from the procfsd daemon via proc_pidinfo(PROC_PIDTHREADID64INFO)
 * keyed on the tid; process-level fields come from the kext directly. Fields
 * with no macOS source (Linux fault counters, CFS scheduler internals, register
 * addresses) are reported as 0 to keep the layout parseable.
 */
#define PROCFS_NS_PER_TICK 10000000ULL   /* 100 Hz: ns -> clock ticks */

/* Fetch per-thread info from the daemon (ti left zeroed if unavailable). */
static int
procfs_thread_info(pfsnode_t *pnp, struct proc_threadinfo *ti)
{
    bzero(ti, sizeof(*ti));
    uint32_t got = 0;
    if (procfs_ctl_request(PROCFS_REQ_THREADINFO, pnp->node_id.nodeid_pid,
            pnp->node_id.nodeid_objectid, ti, sizeof(*ti), &got) == 0 &&
        got == sizeof(*ti)) {
        return 0;
    }
    return ENOTSUP;     /* best-effort: callers format the zeroed struct */
}

/* Fetch proc_taskinfo from the daemon (ti left zeroed if unavailable). */
static int
procfs_task_info(pfsnode_t *pnp, struct proc_taskinfo *ti)
{
    bzero(ti, sizeof(*ti));
    uint32_t got = 0;
    if (procfs_ctl_request(PROCFS_REQ_TASKINFO, pnp->node_id.nodeid_pid, 0,
            ti, sizeof(*ti), &got) == 0 && got == sizeof(*ti)) {
        return 0;
    }
    return ENOTSUP;     /* best-effort: callers format the zeroed struct */
}

/* Map a BSD p_stat to the Linux process state character. */
static char
procfs_proc_state(int p_stat)
{
    switch (p_stat) {
    case SIDL:   return 'I';
    case SRUN:   return 'R';
    case SSTOP:  return 'T';
    case SZOMB:  return 'Z';
    case SSLEEP: return 'S';
    default:     return 'S';
    }
}

/* Process-level context for the thread's owning process. */
struct procfs_pctx {
    int      pid, ppid, pgid, sid, nthreads;
    uint64_t vsize, rsize;
    char     comm[MAXCOMLEN + 1];
    char     state;     /* Linux process-state char from p_stat */
};

static void
procfs_pctx_get(pfsnode_t *pnp, struct procfs_pctx *c)
{
    bzero(c, sizeof(*c));
    c->state = 'S';
    c->pid = pnp->node_id.nodeid_pid;
    proc_t p = proc_find(c->pid);
    if (p == PROC_NULL) {
        return;
    }
    c->ppid     = proc_ppid(p);
    c->pgid     = proc_pgrpid(p);
    c->sid      = proc_sessionid(p);
    c->nthreads = procfs_get_task_thread_count(p);
    c->state    = procfs_proc_state(p->p_stat);
    (void)procfs_task_vm_sizes(p, &c->vsize, &c->rsize);
    proc_name(c->pid, c->comm, sizeof(c->comm));
    proc_rele(p);
}

static char
procfs_thread_state(int run_state)
{
    switch (run_state) {
    case TH_STATE_RUNNING:         return 'R';
    case TH_STATE_STOPPED:         return 'T';
    case TH_STATE_UNINTERRUPTIBLE: return 'D';
    case TH_STATE_WAITING:
    case TH_STATE_HALTED:
    default:                       return 'S';
    }
}

static const char *
procfs_thread_state_word(char c)
{
    switch (c) {
    case 'R': return "running";
    case 'T': return "stopped";
    case 'D': return "disk sleep";
    case 'Z': return "zombie";
    case 'I': return "idle";
    default:  return "sleeping";
    }
}

int
procfs_dothreadcomm(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct proc_threadinfo ti;
    procfs_thread_info(pnp, &ti);

    const char *name = ti.pth_name[0] ? ti.pth_name : NULL;
    char comm[MAXCOMLEN + 1] = { 0 };
    if (name == NULL) {
        proc_name(pnp->node_id.nodeid_pid, comm, sizeof(comm));
        name = comm;
    }

    char buf[MAXTHREADNAMESIZE + 2];
    int len = snprintf(buf, sizeof(buf), "%s\n", name);
    return procfs_copy_data(buf, len, uio);
}

int
procfs_dothreadstat(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct proc_threadinfo ti;
    struct procfs_pctx     c;
    procfs_thread_info(pnp, &ti);
    procfs_pctx_get(pnp, &c);

    uint64_t    tid   = pnp->node_id.nodeid_objectid;
    const char *name  = ti.pth_name[0] ? ti.pth_name : c.comm;
    char        state = procfs_thread_state(ti.pth_run_state);
    uint64_t    utime = ti.pth_user_time   / PROCFS_NS_PER_TICK;
    uint64_t    stime = ti.pth_system_time / PROCFS_NS_PER_TICK;
    uint64_t    rss_pages = c.rsize / PAGE_SIZE;

    /* Linux /proc/<pid>/task/<tid>/stat: 52 space-separated fields. Field 41 is
     * the scheduling policy; fields with no macOS source are 0/-1. */
    char buf[640];
    int len = snprintf(buf, sizeof(buf),
        "%llu (%s) %c %d %d %d 0 -1 0 0 0 0 0 "                /* 1-13  */
        "%llu %llu 0 0 %d 0 %d 0 0 "                           /* 14-22 */
        "%llu %llu 18446744073709551615 "                     /* 23-25 */
        "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 "                       /* 26-40 */
        "%d "                                                  /* 41 policy */
        "0 0 0 0 0 0 0 0 0 0 0\n",                             /* 42-52 */
        (unsigned long long)tid, name, state, c.ppid, c.pgid, c.sid,
        (unsigned long long)utime, (unsigned long long)stime,
        ti.pth_curpri, c.nthreads,
        (unsigned long long)c.vsize, (unsigned long long)rss_pages,
        ti.pth_policy);
    return procfs_copy_data(buf, len, uio);
}

int
procfs_dothreadstatus(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct proc_threadinfo ti;
    struct procfs_pctx     c;
    procfs_thread_info(pnp, &ti);
    procfs_pctx_get(pnp, &c);

    uint64_t    tid  = pnp->node_id.nodeid_objectid;
    const char *name = ti.pth_name[0] ? ti.pth_name : c.comm;
    char        st   = procfs_thread_state(ti.pth_run_state);

    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "Name:\t%s\n"
        "State:\t%c (%s)\n"
        "Tgid:\t%d\n"
        "Pid:\t%llu\n"
        "PPid:\t%d\n"
        "VmSize:\t%8llu kB\n"
        "VmStk:\t%8llu kB\n"
        "Threads:\t%d\n"
        "voluntary_ctxt_switches:\t0\n"
        "nonvoluntary_ctxt_switches:\t0\n",
        name, st, procfs_thread_state_word(st),
        c.pid, (unsigned long long)tid, c.ppid,
        (unsigned long long)(c.vsize >> 10), 0ULL, c.nthreads);
    return procfs_copy_data(buf, len, uio);
}

int
procfs_dothreadsched(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct proc_threadinfo ti;
    struct procfs_pctx     c;
    procfs_thread_info(pnp, &ti);
    procfs_pctx_get(pnp, &c);

    uint64_t    tid  = pnp->node_id.nodeid_objectid;
    const char *name = ti.pth_name[0] ? ti.pth_name : c.comm;

    /* Linux's CFS-internal se.* metrics (vruntime, load.weight, avg.*) have no
     * macOS equivalent and are omitted; we report the metrics we do have. */
    char buf[512];
    int len = snprintf(buf, sizeof(buf),
        "%s (%llu, #threads: %d)\n"
        "-------------------------------------------------------------------\n"
        "se.sum_exec_runtime  : %20llu\n"
        "policy               : %20d\n"
        "prio                 : %20d\n",
        name, (unsigned long long)tid, c.nthreads,
        (unsigned long long)(ti.pth_user_time + ti.pth_system_time),
        ti.pth_policy, ti.pth_curpri);
    return procfs_copy_data(buf, len, uio);
}

/*
 * Linux-compatible /proc/version
 */
int
procfs_doversion(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    int error = 0;
    int len = 0, xlen = 0;

    vm_offset_t off = uio_offset(uio);
    vm_offset_t pgno = trunc_page(off);
    off_t pgoff = (off - pgno);

    char *buf = malloc(LBFSZ, M_TEMP, M_WAITOK);

    /*
     * Print out the kernel version string.
     */
    len = snprintf(buf, LBFSZ, "%s\n", version);

    xlen = (len - pgoff);
    error = uiomove((const char *)buf, xlen, uio);

    free(buf, M_TEMP);

    return error;
}

/*
 * Linux-compatibility-mode register dump (human-readable text).
 *
 * This is the Linux-flavoured counterpart to the native binary /proc/<pid>/regs
 * node implemented in procfs_regs.c. It reports the same machine state - the
 * representative thread's, via thread_get_state() - but as "name 0xvalue" lines
 * instead of a raw struct.
 *
 * GUARDED FOR NOW: it is compiled but not attached to any node. It will be
 * selected by the planned userspace switch that toggles procfs between native
 * BSD/XNU and Linux-compatible presentation; until that selector exists, the
 * structure wires the native procfs_doregs() unconditionally.
 */
int
procfs_doregs_linux(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    if (uio_rw(uio) != UIO_READ) {
        return EOPNOTSUPP;
    }

    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p == PROC_NULL) {
        return ESRCH;
    }
    int sysproc = (p->p_stat == SZOMB) || (p->p_flag & P_SYSTEM) != 0;
    proc_rele(p);
    if (sysproc) {
        return ESRCH;
    }

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 1024, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    /* The register state comes from the procfsd daemon (see procfs_regs.c);
     * an empty body means the daemon is unavailable or task_for_pid was denied. */
#if defined(__arm64__) || defined(__aarch64__)
    arm_thread_state64_t st;
    uint32_t got = 0;
    if (procfs_ctl_request(PROCFS_REQ_REGS, pnp->node_id.nodeid_pid, 0,
                           &st, sizeof(st), &got) == 0 && got > 0) {
        /* non-opaque arm_thread_state64 layout; pc/lr carry PAC bits, emitted raw. */
        for (int i = 0; i < 29; i++) {
            sbuf_printf(&sb, "x%-2d  0x%016llx\n", i, (uint64_t)st.x[i]);
        }
        sbuf_printf(&sb, "fp   0x%016llx\n", (uint64_t)st.fp);
        sbuf_printf(&sb, "lr   0x%016llx\n", (uint64_t)st.lr);
        sbuf_printf(&sb, "sp   0x%016llx\n", (uint64_t)st.sp);
        sbuf_printf(&sb, "pc   0x%016llx\n", (uint64_t)st.pc);
        sbuf_printf(&sb, "cpsr 0x%08x\n",    (uint32_t)st.cpsr);
    }
#elif defined(__x86_64__)
    x86_thread_state64_t st;
    uint32_t got = 0;
    if (procfs_ctl_request(PROCFS_REQ_REGS, pnp->node_id.nodeid_pid, 0,
                           &st, sizeof(st), &got) == 0 && got > 0) {
        sbuf_printf(&sb, "rax 0x%016llx\nrbx 0x%016llx\nrcx 0x%016llx\n"
                         "rdx 0x%016llx\nrsi 0x%016llx\nrdi 0x%016llx\n"
                         "rbp 0x%016llx\nrsp 0x%016llx\n",
            (uint64_t)st.rax, (uint64_t)st.rbx, (uint64_t)st.rcx,
            (uint64_t)st.rdx, (uint64_t)st.rsi, (uint64_t)st.rdi,
            (uint64_t)st.rbp, (uint64_t)st.rsp);
        sbuf_printf(&sb, "r8  0x%016llx\nr9  0x%016llx\nr10 0x%016llx\n"
                         "r11 0x%016llx\nr12 0x%016llx\nr13 0x%016llx\n"
                         "r14 0x%016llx\nr15 0x%016llx\n",
            (uint64_t)st.r8,  (uint64_t)st.r9,  (uint64_t)st.r10,
            (uint64_t)st.r11, (uint64_t)st.r12, (uint64_t)st.r13,
            (uint64_t)st.r14, (uint64_t)st.r15);
        sbuf_printf(&sb, "rip 0x%016llx\nrflags 0x%016llx\n",
            (uint64_t)st.rip, (uint64_t)st.rflags);
    }
#endif

    sbuf_finish(&sb);
    int error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);
    return error;
}

/*
 * Linux-compatibility-mode floating-point/SIMD register dump (human-readable
 * text). Counterpart to the native binary /proc/<pid>/fpregs node in
 * procfs_fpregs.c. GUARDED FOR NOW: compiled but not attached to any node (the
 * structure wires the native procfs_dofpregs() unconditionally); it will be
 * selected by the planned userspace native/linux mode switch.
 */
int
procfs_dofpregs_linux(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    if (uio_rw(uio) != UIO_READ) {
        return EOPNOTSUPP;
    }

    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p == PROC_NULL) {
        return ESRCH;
    }
    int sysproc = (p->p_stat == SZOMB) || (p->p_flag & P_SYSTEM) != 0;
    proc_rele(p);
    if (sysproc) {
        return ESRCH;
    }

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 2048, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }

    /* FP/SIMD state comes from the procfsd daemon (see procfs_fpregs.c). */
#if defined(__arm64__) || defined(__aarch64__)
    arm_neon_state64_t st;
    uint32_t got = 0;
    if (procfs_ctl_request(PROCFS_REQ_FPREGS, pnp->node_id.nodeid_pid, 0,
                           &st, sizeof(st), &got) == 0 && got > 0) {
        /* Each q register is 128-bit; print as hi:lo 64-bit halves. */
        for (int i = 0; i < 32; i++) {
            const uint64_t *qw = (const uint64_t *)&st.q[i];
            sbuf_printf(&sb, "q%-2d  0x%016llx%016llx\n", i,
                        (uint64_t)qw[1], (uint64_t)qw[0]);
        }
        sbuf_printf(&sb, "fpsr 0x%08x\n", (uint32_t)st.fpsr);
        sbuf_printf(&sb, "fpcr 0x%08x\n", (uint32_t)st.fpcr);
    }
#elif defined(__x86_64__)
    /* Text formatting of the x86 FP/SSE state for the linux-compat mode is not
     * yet implemented; the native binary node provides the full state. */
    sbuf_printf(&sb, "fpregs: x86_64 text format not implemented\n");
#endif

    sbuf_finish(&sb);
    int error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);
    return error;
}

/*
 * Linux-compatibility-mode auxiliary vector (human-readable AT_* text).
 *
 * Counterpart to the native /proc/<pid>/auxv node in procfs_auxv.c, which emits
 * XNU's apple[] array. macOS (Mach-O) has no real ELF aux vector, so this
 * synthesizes the AT_* entries that are cheaply derivable from the kernel - a
 * best-effort Linux-style view. HWCAP/PHDR/ENTRY/RANDOM/EXECFN are not yet
 * synthesized (they need cpu-feature bits and the Mach-O image layout) and can
 * be added later.
 *
 * GUARDED FOR NOW: compiled but not attached to any node; the structure wires
 * the native procfs_doauxv() unconditionally until the userspace native/linux
 * mode switch exists.
 */
int
procfs_doauxv_linux(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    if (uio_rw(uio) != UIO_READ) {
        return EOPNOTSUPP;
    }

    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p == PROC_NULL) {
        return ESRCH;
    }

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 512, SBUF_AUTOEXTEND) == NULL) {
        proc_rele(p);
        return ENOMEM;
    }

    kauth_cred_t cr = kauth_cred_proc_ref(p);
    sbuf_printf(&sb, "AT_PAGESZ\t%u\n", (unsigned)PAGE_SIZE);
    sbuf_printf(&sb, "AT_CLKTCK\t%d\n", 100);
    sbuf_printf(&sb, "AT_UID\t%u\n",    kauth_cred_getruid(cr));
    sbuf_printf(&sb, "AT_EUID\t%u\n",   kauth_cred_getuid(cr));
    sbuf_printf(&sb, "AT_GID\t%u\n",    kauth_cred_getrgid(cr));
    sbuf_printf(&sb, "AT_EGID\t%u\n",   kauth_cred_getgid(cr));
    sbuf_printf(&sb, "AT_SECURE\t%d\n", 0);
    kauth_cred_unref(&cr);

    sbuf_finish(&sb);
    int error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);
    proc_rele(p);
    return error;
}

#pragma mark -
#pragma mark Process-level Linux text nodes

/* /proc/<pid>/comm - the process (command) name, one line. */
int
procfs_docomm(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct procfs_pctx c;
    procfs_pctx_get(pnp, &c);

    char buf[MAXCOMLEN + 2];
    int len = snprintf(buf, sizeof(buf), "%s\n", c.comm);
    return procfs_copy_data(buf, len, uio);
}

/*
 * /proc/<pid>/statm - memory usage in pages:
 *   size resident shared text lib data dt
 * Only size (virtual) and resident have a macOS source; the rest are 0.
 */
int
procfs_dostatm(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct procfs_pctx c;
    procfs_pctx_get(pnp, &c);

    char buf[128];
    int len = snprintf(buf, sizeof(buf), "%llu %llu 0 0 0 0 0\n",
        (unsigned long long)(c.vsize / PAGE_SIZE),
        (unsigned long long)(c.rsize / PAGE_SIZE));
    return procfs_copy_data(buf, len, uio);
}

/*
 * /proc/<pid>/stat - the process's single-line stat (52 space-separated fields,
 * same layout as the per-thread stat). CPU time comes from the daemon's
 * proc_taskinfo; fields with no macOS source are 0/-1 (priority 20, policy 0).
 */
int
procfs_doprocstat(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct procfs_pctx   c;
    struct proc_taskinfo ti;
    procfs_pctx_get(pnp, &c);
    procfs_task_info(pnp, &ti);

    uint64_t utime = ti.pti_total_user   / PROCFS_NS_PER_TICK;
    uint64_t stime = ti.pti_total_system / PROCFS_NS_PER_TICK;
    uint64_t rss_pages = c.rsize / PAGE_SIZE;

    char buf[640];
    int len = snprintf(buf, sizeof(buf),
        "%d (%s) %c %d %d %d 0 -1 0 0 0 0 0 "                  /* 1-13  */
        "%llu %llu 0 0 20 0 %d 0 0 "                           /* 14-22 */
        "%llu %llu 18446744073709551615 "                     /* 23-25 */
        "0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 "                       /* 26-40 */
        "0 "                                                  /* 41 policy */
        "0 0 0 0 0 0 0 0 0 0 0\n",                             /* 42-52 */
        c.pid, c.comm, c.state, c.ppid, c.pgid, c.sid,
        (unsigned long long)utime, (unsigned long long)stime, c.nthreads,
        (unsigned long long)c.vsize, (unsigned long long)rss_pages);
    return procfs_copy_data(buf, len, uio);
}

/*
 * /proc/<pid>/status in Linux text form - the linux-mode rendering of the status
 * node (native mode emits the binary proc_bsdshortinfo). Reuses procfs_pctx for
 * the process context and the credential for the Uid/Gid rows.
 */
int
procfs_doprocstatus_linux(pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct procfs_pctx c;
    procfs_pctx_get(pnp, &c);

    uid_t ruid = 0, euid = 0, svuid = 0;
    gid_t rgid = 0, egid = 0, svgid = 0;
    proc_t p = proc_find(pnp->node_id.nodeid_pid);
    if (p != PROC_NULL) {
        kauth_cred_t cr = kauth_cred_proc_ref(p);
        ruid  = kauth_cred_getruid(cr);
        euid  = kauth_cred_getuid(cr);
        svuid = kauth_cred_getsvuid(cr);
        rgid  = kauth_cred_getrgid(cr);
        egid  = kauth_cred_getgid(cr);
        svgid = kauth_cred_getsvgid(cr);
        kauth_cred_unref(&cr);
        proc_rele(p);
    }

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 1024, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }
    sbuf_printf(&sb,
        "Name:\t%s\n"
        "State:\t%c (%s)\n"
        "Tgid:\t%d\n"
        "Pid:\t%d\n"
        "PPid:\t%d\n"
        "TracerPid:\t0\n"
        "Uid:\t%u\t%u\t%u\t%u\n"
        "Gid:\t%u\t%u\t%u\t%u\n"
        "VmSize:\t%8llu kB\n"
        "VmRSS:\t%8llu kB\n"
        "Threads:\t%d\n"
        "voluntary_ctxt_switches:\t0\n"
        "nonvoluntary_ctxt_switches:\t0\n",
        c.comm, c.state, procfs_thread_state_word(c.state),
        c.pid, c.pid, c.ppid,
        ruid, euid, svuid, euid,        /* Uid: real effective saved fs (fs~=eff) */
        rgid, egid, svgid, egid,        /* Gid: real effective saved fs           */
        (unsigned long long)(c.vsize >> 10),
        (unsigned long long)(c.rsize >> 10),
        c.nthreads);

    sbuf_finish(&sb);
    int error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);
    return error;
}

/* /proc/uptime - seconds since boot and (approximate) idle seconds. */
int
procfs_douptime(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct timeval tv;
    microuptime(&tv);

    /* Field 2 is summed CPU idle time; macOS has no cheap kernel-context source
     * for it here, so report 0.00 (the common consumer, uptime(1), uses field 1). */
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%lld.%02d 0.00\n",
        (long long)tv.tv_sec, (int)(tv.tv_usec / 10000));
    return procfs_copy_data(buf, len, uio);
}

/* /proc/swaps - macOS uses dynamic swap files under /private/var/vm; report the
 * aggregate usage from the vm.swapusage sysctl as a single swap area. */
int
procfs_doswaps(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 256, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }
    sbuf_printf(&sb, "Filename\t\t\t\tType\t\tSize\tUsed\tPriority\n");

    struct xsw_usage xsu;
    size_t len = sizeof(xsu);
    if (sysctlbyname("vm.swapusage", &xsu, &len, NULL, 0) == 0 && xsu.xsu_total > 0) {
        sbuf_printf(&sb, "/private/var/vm/swapfile\tfile\t\t%llu\t%llu\t-1\n",
            (unsigned long long)(xsu.xsu_total / 1024),
            (unsigned long long)(xsu.xsu_used  / 1024));
    }

    sbuf_finish(&sb);
    int error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);
    return error;
}

/* Filesystem types that mount without a backing block device (Linux "nodev"). */
static boolean_t
procfs_fs_is_nodev(const char *type)
{
    static const char *const nodev[] = {
        "devfs", "procfs", "autofs", "nullfs", "fdesc", "tmpfs", "mtmfs",
        "lifs", "bindfs", "webdav", "nfs", "smbfs", "ftp", NULL
    };
    for (int i = 0; nodev[i] != NULL; i++) {
        if (strncmp(type, nodev[i], MFSTYPENAMELEN) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

struct procfs_fstypes_ctx {
    char names[32][MFSTYPENAMELEN];
    int  count;
};

/* vfs_iterate() callout: collect each distinct mounted filesystem type. */
static int
procfs_fstypes_cb(mount_t mp, void *arg)
{
    struct procfs_fstypes_ctx *fc = (struct procfs_fstypes_ctx *)arg;
    struct vfsstatfs *st = vfs_statfs(mp);
    if (st == NULL) {
        return VFS_RETURNED;
    }
    for (int i = 0; i < fc->count; i++) {
        if (strncmp(fc->names[i], st->f_fstypename, MFSTYPENAMELEN) == 0) {
            return VFS_RETURNED;                /* already seen this type */
        }
    }
    if (fc->count < (int)(sizeof(fc->names) / sizeof(fc->names[0]))) {
        strlcpy(fc->names[fc->count], st->f_fstypename, MFSTYPENAMELEN);
        fc->count++;
    }
    return VFS_RETURNED;
}

/*
 * /proc/filesystems - the distinct filesystem types currently in use (macOS has
 * no kernel-context enumeration of all *registered* types, so this lists the
 * mounted ones, deduplicated). "nodev" prefixes types that need no block device.
 */
int
procfs_dofilesystems(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    struct procfs_fstypes_ctx fc;
    bzero(&fc, sizeof(fc));
    vfs_iterate(0, procfs_fstypes_cb, &fc);

    struct sbuf sb;
    if (sbuf_new(&sb, NULL, 512, SBUF_AUTOEXTEND) == NULL) {
        return ENOMEM;
    }
    for (int i = 0; i < fc.count; i++) {
        sbuf_printf(&sb, "%s\t%s\n",
            procfs_fs_is_nodev(fc.names[i]) ? "nodev" : "", fc.names[i]);
    }

    sbuf_finish(&sb);
    int error = procfs_copy_data(sbuf_data(&sb), sbuf_len(&sb), uio);
    sbuf_delete(&sb);
    return error;
}

/*
 * /proc/modules - the Linux-format view of the loaded kernel extensions
 * (name size refcount deps state address). The listing is produced by the
 * procfsd daemon and reassembled by the shared helper in procfs_extensions.c;
 * only the daemon request type differs from /proc/extensions.
 */
int
procfs_domodules(__unused pfsnode_t *pnp, uio_t uio, __unused vfs_context_t ctx)
{
    return procfs_dokextlist(PROCFS_REQ_MODULES, uio);
}

#pragma mark -
#pragma mark Presentation-mode switch (native vs Linux)

/*
 * Presentation mode, toggled live from userspace via the `procfs.linux` sysctl:
 *   sysctl -w procfs.linux=1   # Linux-compatible rendering
 *   sysctl -w procfs.linux=0   # native BSD/XNU rendering (default)
 *
 * Nodes with both renderings (regs/fpregs/auxv) check this; other nodes keep
 * their single format. The oids are registered/unregistered by the kext's
 * start/stop (a kext must register its sysctl oids explicitly - they are not
 * wired into the kernel's linker set).
 */
int procfs_linux_mode = 0;

/*
 * The `procfs.linux` oids are built by hand rather than with the SYSCTL_NODE /
 * SYSCTL_INT macros. Under XNU_KERNEL_PRIVATE (which the kext compiles with)
 * those macros expand to an in-kernel STARTUP auto-registration that references
 * sysctl_register_oid_early() - an internal symbol NOT exported to kexts, so the
 * kext would fail to bind and never load. Constructing the sysctl_oid structs
 * directly and registering them through the KPI sysctl_register_oid() avoids
 * that symbol entirely; omitting CTLFLAG_PERMANENT lets us remove them at unload.
 */
static struct sysctl_oid_list procfs_sysctl_children;

static struct sysctl_oid procfs_sysctl_node = {
    .oid_parent  = &sysctl__children,
    .oid_number  = OID_AUTO,
    .oid_kind    = CTLTYPE_NODE | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2,
    .oid_arg1    = &procfs_sysctl_children,
    .oid_arg2    = 0,
    .oid_name    = "procfs",
    .oid_handler = NULL,
    .oid_fmt     = "N",
    .oid_descr   = "procfs filesystem",
    .oid_version = SYSCTL_OID_VERSION,
};

static struct sysctl_oid procfs_sysctl_linux = {
    .oid_parent  = &procfs_sysctl_children,
    .oid_number  = OID_AUTO,
    .oid_kind    = CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_OID2,
    .oid_arg1    = &procfs_linux_mode,
    .oid_arg2    = 0,
    .oid_name    = "linux",
    .oid_handler = sysctl_handle_int,
    .oid_fmt     = "I",
    .oid_descr   = "procfs presentation mode: 0 = native BSD/XNU, 1 = Linux-compatible",
    .oid_version = SYSCTL_OID_VERSION,
};

void
procfs_sysctl_register(void)
{
    sysctl_register_oid(&procfs_sysctl_node);   /* parent first */
    sysctl_register_oid(&procfs_sysctl_linux);
}

void
procfs_sysctl_unregister(void)
{
    sysctl_unregister_oid(&procfs_sysctl_linux);
    sysctl_unregister_oid(&procfs_sysctl_node);
}