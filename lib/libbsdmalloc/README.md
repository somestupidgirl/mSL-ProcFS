# libbsdmalloc

A small compatibility library that restores the BSD-style `malloc()` / `free()`
allocator interface for third-party (non-Apple) kernel extensions, on top of
the exported `_MALLOC()` / `_FREE()` KPI.

## Why

Recent `<sys/malloc.h>` headers only prototype `_MALLOC()` and `_FREE()` in
their non-`XNU_KERNEL_PRIVATE` branch — they are deprecated in favour of the
typed `kalloc_type()` family, which lives in `com.apple.kpi.private` and cannot
be linked from a third-party kext. The `_MALLOC()` / `_FREE()` symbols remain
exported BSD KPI, though, so they are still usable; they are just no longer
declared in a kernel-private build.

`libbsdmalloc` provides a drop-in `<sys/malloc.h>` that:

- pulls in the real kernel `<sys/malloc.h>` (via `#include_next`),
- re-declares `_MALLOC()` and `_FREE()`,
- provides FreeBSD's `malloc(size, type, flags)` / `free(addr, type)`
  interface, including the M_ZERO zero-in-place optimisation macro, and
- supplies the FreeBSD spellings `__predict_true()` / `__predict_false()`,
  which XNU's `<sys/cdefs.h>` does not define (it calls them `__probable()` /
  `__improbable()`).

`malloc()` and `free()` are real functions, defined in `malloc.c` as thin
wrappers over `_MALLOC()` / `_FREE()`. Because `<sys/malloc.h>` also defines
`malloc` as a function-like macro, `malloc.c` parenthesises the name —
`(malloc)(...)` — to suppress that expansion, the same idiom FreeBSD uses in
`sys/kern/kern_malloc.c`.

## Usage

Add the library's `include/` directory to your compiler search path *before*
the kernel headers so the wrapper is found first, and link the archive:

```
-Ipath/to/libbsdmalloc/include
-Lpath/to/libbsdmalloc -lbsdmalloc
```

Then `#include <sys/malloc.h>` as usual.

Linking is required: `malloc()` and `free()` are functions, not macros, so a
consumer that calls them will otherwise be left with undefined `_malloc` /
`_free` symbols. A kext link tolerates undefined symbols and resolves them at
load time, so this does *not* fail the build — it fails when the kext is
loaded. Check with `nm -u` if in doubt. (`_MALLOC` / `_FREE` remaining
undefined is expected and correct: those are genuine kernel BSD KPI exports.)

## Layout

```
include/sys/malloc.h    the wrapper header
malloc.c                the malloc()/free() definitions
Makefile                builds libbsdmalloc.a
LICENSE                 BSD 3-Clause (FreeBSD) + MIT (original)
```

## License

Dual-licensed — see [`LICENSE`](LICENSE) for the full text of both:

- **BSD 3-Clause** — the portions of `include/sys/malloc.h` derived from
  FreeBSD's `sys/sys/malloc.h`: the `malloc()`/`free()` declarations and the
  M_ZERO zero-in-place `malloc()` macro.
  Copyright (c) 1987, 1993 The Regents of the University of California;
  Copyright (c) 2005, 2009 Robert N. M. Watson.
- **MIT** — the remaining original code: the `#include_next` wrapper
  arrangement, the `_MALLOC()`/`_FREE()` re-declarations, the XNU
  compatibility guards, and the wrapper definitions in `malloc.c`.
