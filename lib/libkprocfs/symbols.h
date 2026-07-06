/*
 * symbols.h
 *
 * libklookup staged-symbol validation. No private-KPI stubs remain - every
 * former consumer moved to public KPIs, forward-ports, or the procfsd daemon.
 * This header (and libklookup) is removed with the staging pipeline.
 *
 * Copyright (c) 2022-2026 Sunneva N. Mariu
 */
#ifndef _symbols_h
#define _symbols_h

#include <mach/mach_types.h>    /* boolean_t */

/* Set at load if the staged kernel-symbol file validates against the running
 * kernel. Vestigial - nothing consumes it. */
extern boolean_t                procfs_klookup_ok;

#endif /* _symbols_h */
