# libcpuft

Kernel-side **CPU feature and identification** helpers for third-party
(non-Apple) macOS kernel extensions, on both x86_64 and ARM64.

The library reads the CPU's identity and feature set directly from the kernel —
via the `cpuid` instruction and MSRs on x86, and the `MIDR`/`hw.optional.*`
sysctls on ARM64 — and renders the results as the strings needed to build a
Linux-style `/proc/cpuinfo` (feature/flag lists, implementer/part/variant,
BogoMIPS, chip name, microcode version, and so on).

## Not to be confused with libcpuid

This is unrelated to the well-known [anrieff/libcpuid](https://github.com/anrieff/libcpuid).
That library runs in **userspace** and, on macOS, cannot read x86 MSRs or ARM
CPUID registers. libcpuft runs **in the kernel**, which is exactly where that
register access is available on macOS. The two solve the same problem in
opposite layers and share no code.

## Scope: no daemon, no procfs

libcpuft is self-contained. It does **not** talk to any userspace daemon and has
no dependency on the procfs project. The per-CPU interrupt/softirq and E/P
cluster accounting that once lived here (and which *did* need the procfsd
control bridge) has been moved into the kext proper — daemon-backed data does
not belong in a standalone library.

## Usage

```
-Ipath/to/libcpuft/include
-Lpath/to/libcpuft -lcpuft
```

Then `#include <cpuft.h>`. On ARM64, `get_cpu_flags(linux_hwcap_order)` takes a
flag selecting Linux HWCAP print order (non-zero) versus the internal grouped
order (zero); the argument is ignored on x86.

It depends on `libbsdmalloc` for `<sys/malloc.h>` and on `libkext` for
`<libkext/libkext.h>`; put their include directories on the search path.

## Layout

```
cpuft.c                 the implementation (x86_64 + ARM64)
include/cpuft.h         the public header
Makefile                builds libcpuft.a
LICENSE                 MIT (+ attribution NOTICE)
```

## License

Dual-licensed — see [`LICENSE`](LICENSE) for the full text of both:

- **APSL 2.0** — portions of `cpuft.c` derived from Apple's XNU kernel and from
  Shaneee's [Mojave_AMD_XNU](https://github.com/Shaneee/Mojave_AMD_XNU) (an AMD
  fork of XNU; both are distributed under APSL 2.0). These include the AMD/CPUID
  helpers (`is_amd_cpu`, `is_intel_cpu`, `extract_bitfield`, `get_bitfield_width`)
  and the x86 `cpuft_cpuid_info()` forward-port.
- **MIT** — the remaining original code, including the ARM64 identification and
  the library glue.
