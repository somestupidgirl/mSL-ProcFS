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

kern_return_t
resolve_symbols(void)
{
    /*
     * No private symbols are resolved through libklookup anymore - every former
     * consumer now goes through the procfsd daemon. This only validates that the
     * staged kernel-symbol file matches the running kernel ("_version" is the
     * slide anchor, validated by construction) and is kept as a vestigial gate
     * until the staging pipeline is removed.
     */
    enum { I_VERSION, N_SYMS };
    static const char *const names[N_SYMS] = {
        [I_VERSION] = "_version",
    };
    void *addr[N_SYMS] = { NULL };

    klookup_resolve(names, addr, N_SYMS);

    if (addr[I_VERSION] != (void *)(uintptr_t)version) {
        printf("procfs: libklookup unavailable (staged symbols missing/stale)\n");
        return KERN_SUCCESS;
    }

    procfs_klookup_ok = TRUE;
    printf("procfs: libklookup OK\n");

    return KERN_SUCCESS;
}
