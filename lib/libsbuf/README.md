# libsbuf

A self-contained build of the kernel `sbuf(9)` safe string-buffer API for
third-party (non-Apple) macOS kernel extensions.

`sbuf` provides an auto-extending, bounds-checked buffer for building strings
and byte streams incrementally — `sbuf_new()`, `sbuf_cat()`, `sbuf_printf()`,
`sbuf_putc()`, `sbuf_finish()`, `sbuf_data()`, `sbuf_len()`, and friends —
without the overflow footguns of raw `snprintf()` into fixed buffers.

## Why a separate copy

The in-kernel `sbuf` implementation is written against the typed `kalloc_type()`
allocators in `kern/kalloc.h`, which live in `com.apple.kpi.private` and cannot
be linked from a third-party kext. This copy keeps the upstream call sites
unchanged but redirects the allocator macros onto the exported BSD
`_MALLOC()` / `_FREE()` KPI (via [`libbsdmalloc`](../libbsdmalloc)), so it links
cleanly from an ordinary kext.

## Usage

Add the library's `include/` directory to your header search path and link the
static archive:

```
-Ipath/to/libsbuf/include
-Lpath/to/libsbuf -lsbuf
```

Then `#include <sys/sbuf.h>`.

It depends on `libbsdmalloc` for `<sys/malloc.h>`; put that library's `include/`
on the search path ahead of the kernel headers.

## Layout

```
sbuf.c                  the implementation
include/sys/sbuf.h      the public header
Makefile                builds libsbuf.a
LICENSE                 APSL 2.0 (sbuf.c) + BSD (sbuf.h)
```

## Licensing

This library is licensed under two licenses; see [`LICENSE`](LICENSE):

- `sbuf.c` — Apple Public Source License, Version 2.0
- `include/sys/sbuf.h` — BSD License (Poul-Henning Kamp / Dag-Erling Smorgrav,
  originally from FreeBSD)
