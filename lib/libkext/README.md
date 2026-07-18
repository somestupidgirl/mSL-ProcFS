# libkext

Minimalist's kernel extension library — small utility routines for writing
third-party macOS kernel extensions.

## What it provides

- **Logging** — `LOG()`, `LOG_INF()`, `LOG_WARN()`, `LOG_ERR()`, `LOG_BUG()`,
  `LOG_TRACE()` and `LOG_DBG()` (the last compiles away unless `DEBUG` is
  defined). These wrap the kernel's `printf()`, which avoids `os_log()`'s
  availability constraints; note the kernel's `printf()` does not accept some
  rarely used format specifiers.
- **Assertions** — `kassert()`, `kassertf()`, the comparison forms
  (`kassert_eq`, `kassert_ne`, `kassert_lt`, `kassert_le`, `kassert_gt`,
  `kassert_ge`, `kassert_null`, `kassert_nonnull`) and `panicf()`.
- **Tracked allocation** — `libkext_malloc()`, `libkext_realloc()`,
  `libkext_mfree()`, with `libkext_massert()` to assert at unload that nothing
  was leaked.
- **Kext control block** — `libkext_get_kcb()` / `libkext_put_kcb()` /
  `libkext_read_kcb()` / `libkext_invalidate_kcb()`, a counter for tracking
  in-flight activity so a kext is not unloaded from under a running call.
- **Misc helpers** — `libkext_vma_uuid()`, `libkext_format_uuid_string()`,
  `libkext_file_read()`, plus the usual `ARRAY_SIZE` / `GMIN` / `GMAX` /
  `BUILD_BUG_ON` / `likely` / `unlikely` conveniences.

## Usage

```
-Ipath/to/libkext
-Lpath/to/libkext -lkext
```

Then `#include <libkext.h>`.

## Configuration: `KEXTNAME_S`

`KEXTNAME_S` is the only build knob. It is the prefix printed before every
message logged through the macros above:

```
procfs: ERR something went wrong
^^^^^^
```

It defaults to `"libkext"`, so a standalone build needs no configuration at
all. To brand messages as your own kext instead:

```
make KEXTNAME=foo                 # when building this library
-DKEXTNAME_S=\"foo\"              # when compiling your own sources
```

**It is a compile-time string literal, expanded separately in each translation
unit that uses it.** That has a consequence worth understanding: the messages
libkext logs from its *own* sources carry whatever name the library was built
with, while messages your code logs carry whatever *you* define. The two are
independent.

That is usually what you want — a line prefixed `libkext:` tells you at a
glance that it came from the library rather than from your code. If you would
rather everything carried a single name, build the library with
`make KEXTNAME=foo` to match the `-DKEXTNAME_S` you pass to your own sources.

Note that libkext reaches `KEXTNAME_S` from its own sources through
`LOG_BUG()` and — in non-`DEBUG` builds only — `kassert()`, which routes
through `LOG_BUG()` rather than `panicf()`. So the setting matters for release
builds even if you never call the logging macros yourself.

## License

BSD 2-Clause — see [`LICENSE`](LICENSE). Copyright (c) 2018, lynnl.
