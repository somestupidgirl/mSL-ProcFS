# libkern

The **kernel porting library**: routines from Apple's XNU kernel that a
third-party (non-Apple) kernel extension needs but cannot link, re-implemented
so that they can be.

Many useful XNU routines live in `com.apple.kpi.private` — or, on Apple
Silicon, are stripped from the kernel's symbol table entirely — so a
third-party kext can neither link nor resolve them. libkern re-implements those
routines against XNU's private struct layouts, keeping the original XNU
semantics and names.

This is not a public-KPI wrapper, and it cannot be one: a routine is
unavailable precisely because it reads private state. There is, for instance,
no public KPI that enumerates a process's file descriptors at all, so every
descriptor-related routine here necessarily works through `struct proc`'s
`p_fd`. See [Struct-layout dependency](#struct-layout-dependency) for what that
obliges of consumers.

## Layout

Each source file is named after the XNU file it was forward-ported from and
carries that file's Apple copyright and APSL 2.0 header:

| File | Ported from | Provides |
|---|---|---|
| `kern_descrip.c` | `xnu/bsd/kern/kern_descrip.c` | `fg_get_data_volatile()` |
| `proc_info.c` | `xnu/bsd/kern/proc_info.c` | `proc_pidshortbsdinfo()`, `fill_fileinfo()`, `fill_vnodeinfo()`, `proc_fdlock()`/`proc_fdunlock()`/`proc_fdlist()`, `fd_vnode_info()`, `fd_socket()` |
| `include/kern.h` | the corresponding XNU internal headers | the public interface |

Where a routine has no direct XNU equivalent because the original is
unlinkable, it is named for what it replaces — for example `fd_vnode_info()`
stands in for the private `fp_getfvp()`, capturing the vnode and its vnode id
under `proc_fdlock()` so the caller can take an iocount with
`vnode_getwithvid()` instead of a `fileproc` reference.

## Usage

```
-Ipath/to/libkern/include
-Lpath/to/libkern -lkern
```

Then `#include <kern.h>`, after the usual kernel headers (`sys/proc.h`,
`sys/vnode.h`, `sys/proc_info.h`, `sys/file_internal.h`, …) — exactly as XNU's
own internal headers expect.

It depends on `libbsdmalloc` for `<sys/malloc.h>`; put that library's
`include/` on the search path ahead of the kernel headers.

## Struct-layout dependency

Because these routines read private kernel structures directly, the library is
coupled to the layout of the kernel it runs against. Two obligations follow,
and neither is optional:

**1. Compile with the same configuration as the running kernel.** Several
structures libkern dereferences have members guarded by `CONFIG_*` macros. If
they do not match the shipping kernel, fields land at the wrong offsets. For
the release macOS kernel that means at least:

```
-DCONFIG_PERSONAS -DCONFIG_AUDIT -DCONFIG_DTRACE
```

`CONFIG_PERSONAS` and `CONFIG_AUDIT` sit before `p_fd` in `struct proc`;
omitting them makes `&p->p_fd.fd_lock` point at garbage, which panics with
"Invalid/destroyed mutex" rather than failing cleanly. `CONFIG_DTRACE` is on in
the shipping kernel and shifts the fields after it. `CONFIG_PROC_RESOURCE_LIMITS`
is DEV/DEBUG-only and must be left *undefined* to match release. The
kernel-private macros (`-DKERNEL -DPRIVATE -DKERNEL_PRIVATE
-DXNU_KERNEL_PRIVATE -DBSD_KERNEL_PRIVATE ...`) must also be set on the command
line, early enough to be visible to the first header of every translation unit.

**2. Validate the layout before trusting it.** `proc_info.c` sanity-checks
`struct filedesc` via `fd_layout_ok()` before taking any lock, so a layout
mismatch degrades to "no descriptors" instead of panicking on a bogus mutex.
Any routine added here that dereferences a private structure should be guarded
the same way.

Consequently libkern is version-coupled to macOS: a release that changes these
structures requires re-checking the layout, not merely recompiling.

## License

Apple Public Source License, Version 2.0 — see [`LICENSE`](LICENSE). This is
Apple's Original Code plus Modifications; per APSL 2.0 §2.2 the Modifications
are released under the same license, and each file carries a notice recording
that it was changed.
