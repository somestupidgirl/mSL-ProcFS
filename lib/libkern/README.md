# libkern

The **forward-porting library**: routines from Apple's XNU kernel that a
third-party (non-Apple) kernel extension needs but cannot link, re-implemented
so that they can be.

Many useful XNU routines live in `com.apple.kpi.private` — or, on Apple
Silicon, are stripped from the kernel's symbol table entirely — so a
third-party kext can neither link nor resolve them. libkern re-implements those
routines on top of public KPIs and structures, keeping the original XNU
semantics and names.

## Layout

Each source file is named after the XNU file it was forward-ported from and
carries that file's Apple copyright and APSL 2.0 header:

| File | Ported from | Provides |
|---|---|---|
| `bsd_init.c` | `xnu/bsd/kern/bsd_init.c` | `initproc` |
| `param.c` | `xnu/bsd/conf/param.c` | `maxproc`, `maxprocperuid`, `hard_maxproc`, `nprocs` |
| `kern_proc.c` | `xnu/bsd/kern/kern_proc.c` | the `pidlist_*` helpers, `allproc`, `zombproc` |
| `kern_descrip.c` | `xnu/bsd/kern/kern_descrip.c` | `fg_get_data_volatile()` |
| `proc_info.c` | `xnu/bsd/kern/proc_info.c` | `proc_pidshortbsdinfo()`, `fill_fileinfo()`, `fill_vnodeinfo()`, `proc_fdlock()`/`proc_fdunlock()`/`proc_fdlist()`, `fd_vnode_info()`, `fd_socket()` |
| `include/kern.h` | the corresponding XNU internal headers | the public interface |

Where a routine has no direct XNU equivalent because the original is
unlinkable, it is named for what it replaces — for example `fd_vnode_info()`
stands in for the private `fp_getfvp()`, capturing the vnode and its vnode id
under `proc_fdlock()` so the caller can take an iocount with
`vnode_getwithvid()` instead of a `fileproc` reference.

## Scope: no daemon, no procfs

libkern is self-contained. It does not talk to any userspace daemon and does
not depend on the procfs project.

Two categories of routine were deliberately **left out**:

- Anything requiring the opaque Mach `struct task` / `struct thread` layout.
  `proc_pidtaskinfo()` and `proc_pidthreadinfo()` are the notable casualties:
  XNU's fillers (`fill_taskprocinfo`, `fill_taskthreadinfo`) need `task_lock()`,
  the `task->threads` queue, `task->ledger` and several private symbols
  (`vm_map_adjusted_size`, `recount_task_times`, `ledger_get_balance`,
  `counter_load`). None of that is reachable from a third-party kext, so those
  routines cannot be forward-ported and belong to whatever consumer can obtain
  the data another way.
- Anything that is really consumer-domain logic rather than an XNU routine.

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

## License

Apple Public Source License, Version 2.0 — see [`LICENSE`](LICENSE). This is
Apple's Original Code plus Modifications; per APSL 2.0 §2.2 the Modifications
are released under the same license, and each file carries a notice recording
that it was changed.
