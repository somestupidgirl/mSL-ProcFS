# libbsdmalloc

A tiny, header-only compatibility shim that restores the BSD-style
`_MALLOC()` / `_FREE()` allocator interface for third-party (non-Apple) kernel
extensions.

## Why

Recent `<sys/malloc.h>` headers only prototype `_MALLOC()` and `_FREE()` in
their non-`XNU_KERNEL_PRIVATE` branch — they are deprecated in favour of the
typed `kalloc_type()` family, which lives in `com.apple.kpi.private` and cannot
be linked from a third-party kext. The `_MALLOC()` / `_FREE()` symbols remain
exported BSD KPI, though, so they are still usable; they are just no longer
declared in a kernel-private build.

`libbsdmalloc` provides a drop-in `<sys/malloc.h>` that:

- pulls in the real kernel `<sys/malloc.h>` (via `#include_next`),
- re-declares `_MALLOC()` and `_FREE()`, and
- restores the classic `malloc(size, type, flags)` / `free(addr, type)`
  macros.

## Usage

Add the library's `include/` directory to your compiler search path *before*
the kernel headers so the wrapper is found first:

```
-Ipath/to/libbsdmalloc/include
```

Then `#include <sys/malloc.h>` as usual.

## Layout

```
include/sys/malloc.h    the wrapper header (the entire library)
Makefile                header-only; `make install` copies the header
```

This library is header-only — there is nothing to compile or link.
