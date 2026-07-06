/*
 * symbols.c - libklookup staged-symbol validation
 *
 * No private kernel symbols are resolved through libklookup anymore: every
 * former consumer has moved to public KPIs, forward-ports, or the procfsd
 * daemon. resolve_symbols() only validates that the staged kernel-symbol file
 * matches the running kernel; it is vestigial and removed with the staging
 * pipeline.
 *
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 */
#include <mach/kern_return.h>
#include <libkern/libkern.h>
#include "symbols.h"

/* libklookup: resolves symbols from the on-disk kernelcache symbol table
 * (applying the KASLR slide). Only the "_version" anchor is looked up now. */
extern int klookup_resolve(const char *const *names, void **out, int count);
extern const char version[];

/* Set once at load if the staged symbol file validates. */
boolean_t procfs_klookup_ok = FALSE;

kern_return_t
resolve_symbols(void)
{
    /*
     * "_version" is the slide anchor and validates by construction; a
     * non-matching staged file yields a mismatch and we simply leave
     * procfs_klookup_ok FALSE. Nothing else depends on it.
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
