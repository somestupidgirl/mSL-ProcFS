/*
 * symbols.c - Private KPI symbol resolution
 *
 * On ARM64 macOS, kernel memory scanning is not possible due to PAC
 * (Pointer Authentication Code) enforcement. All private symbols are
 * stubbed as NULL. Public KPI alternatives are used where available.
 *
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 */
#include <mach/kern_return.h>
#include <libkern/libkern.h>
#include <ptrauth.h>
#include "symbols.h"

/* libklookup: resolves private kernel functions from the on-disk kernelcache
 * symbol table (applying the KASLR slide), reaching symbols absent from every
 * .exports and jettisoned from the running kernel's __LINKEDIT. */
extern int klookup_resolve(const char *const *names, void **out, int count);
extern const char version[];

/* Set once at load if libklookup validates (in resolve_symbols). Code that uses
 * a klookup-resolved private symbol should gate on this. */
boolean_t procfs_klookup_ok = FALSE;

/*
 * Make a klookup-resolved raw function address callable under the arm64e kernel
 * ABI. Sign it with key IA and discriminator 0; the subsequent assignment to a
 * typed function pointer triggers clang's void*->fnptr conversion, which auths
 * with disc 0 and re-signs with that pointer type's discriminator - i.e. the
 * scheme the call site expects. (Signing with the type discriminator directly is
 * wrong: the conversion's auth uses disc 0 and would corrupt the pointer.) On
 * non-ptrauth targets this is a plain cast.
 */
#if __has_feature(ptrauth_calls)
#define KL_SIGN_FN(addr) \
    ptrauth_sign_unauthenticated((void *)(addr), ptrauth_key_function_pointer, 0)
#else
#define KL_SIGN_FN(addr) ((void *)(addr))
#endif

#define SYM_INIT(sym) \
	__typeof(_##sym) _##sym = NULL

/*
 * These stay NULL: resolve_symbols() below never assigns them, so every call
 * site guards on `_sym != NULL` and takes a public-KPI/daemon fallback. They
 * remain only for the handful of guarded references still in the tree and are
 * being removed stage by stage.
 */
SYM_INIT(proc_issetugid);
SYM_INIT(proc_get_darwinbgstate);
SYM_INIT(fill_taskprocinfo);
SYM_INIT(fill_taskthreadinfo);
SYM_INIT(vn_stat);
SYM_INIT(dead_mountp);
SYM_INIT(processor_count);
#if defined(__x86_64__)
SYM_INIT(tscFreq);
SYM_INIT(cpuid_info);
#endif

/*
 * Raw runtime addresses of private kernel functions resolved via libklookup
 * (NULL if unavailable). These are unsigned; sign with KL_SIGN_FN at the call
 * site, using the function-pointer type the call expects.
 *
 * On arm64 the fill_taskinfo/fill_taskthreadinfo symbols are stripped from the
 * kernel entirely (see reference memory), so taskinfo/threadinfo cannot use
 * them. proc_task and the VM-region accessors survive in the symtab.
 */
task_t (*procfs_kl_proc_task)(proc_t p) = NULL;            /* PAC-signed */
void *procfs_kl_get_task_map = NULL;
void *procfs_kl_mach_vm_region = NULL;

kern_return_t
resolve_symbols(void)
{
    /*
     * Resolve the private symbols we use from the staged kernel-symbol file.
     * "_version" is the slide anchor and validates by construction; klookup
     * additionally validates against "_kernel_pmap", so a non-matching staged
     * file yields NULLs and we leave the features disabled.
     */
    enum { I_VERSION, I_GET_TASK_MAP, I_MACH_VM_REGION, I_PROC_TASK, N_SYMS };
    static const char *const names[N_SYMS] = {
        [I_VERSION]             = "_version",
        [I_GET_TASK_MAP]        = "_get_task_map",
        [I_MACH_VM_REGION]      = "_mach_vm_region",
        [I_PROC_TASK]           = "_proc_task",
    };
    void *addr[N_SYMS] = { NULL };

    klookup_resolve(names, addr, N_SYMS);

    if (addr[I_VERSION] != (void *)(uintptr_t)version) {
        printf("procfs: libklookup unavailable (staged symbols missing/stale)\n");
        return KERN_SUCCESS;
    }

    procfs_klookup_ok = TRUE;

    /* proc_task: call the kernel's own accessor rather than reading struct proc
     * at a compile-time offset, which drifts across kernel point-releases. */
    if (addr[I_PROC_TASK] != NULL) {
        procfs_kl_proc_task = KL_SIGN_FN(addr[I_PROC_TASK]);
    }

    /* mach_vm_region + get_task_map are called directly (PAC-signed at the use
     * site in procfs_map.c) to enumerate a task's VM regions. */
    procfs_kl_get_task_map   = addr[I_GET_TASK_MAP];
    procfs_kl_mach_vm_region = addr[I_MACH_VM_REGION];

    printf("procfs: libklookup OK (get_task_map=%d mach_vm_region=%d proc_task=%d)\n",
           procfs_kl_get_task_map != NULL,
           procfs_kl_mach_vm_region != NULL, procfs_kl_proc_task != NULL);

    return KERN_SUCCESS;
}
