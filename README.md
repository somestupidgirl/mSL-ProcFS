# ProcFS
ProcFS is a kernel-extension implementation of the `/proc` file system for macOS, exposing
running processes and threads as a filesystem with BSD- and Linux-compatible
per-process information.

## What is procfs?
*procfs* lets you view the processes running on a UNIX system as nodes in the file system, where each process is represented by a single directory named from its process id. Typically, the file system is mounted at `/proc`, so the directory for process 1 would be called `/proc/1`. Beneath a process’ directory are further directories and files that give more information about the process, such as its process id, its active threads, the files that it has open, and so on. *procfs* first appeared in an early version of AT&T’s UNIX and was later implemented in various forms in System V, BSD, Solaris and Linux. You can find a history of the implementation of *procfs* at https://en.wikipedia.org/wiki/Procfs.

In addition to letting you visualize running processes, *procfs* also allows some measure of control over them, at least to suitably privileged users. By writing specific data structures to certain files, you could do such things as set breakpoints and read and write process memory and registers. In fact, on some systems, this was how debugging facilities were provided. However, more modern operating systems do this differently, so some UNIX variants no longer include an implementation of *procfs*. In particular, macOS doesn’t provide *procfs* so, although it’s not strictly needed, I thought that implementing it would be an interesting side project. The code in this repository provides an implementation of *procfs* for macOS. You can use it to see what processes and threads are running on the system and what files they have open. Later, I plan to add more features, beginning with the ability to inspect a thread’s address space to see which executable it is running and what shared libraries it has loaded.

Tested on:

    - macOS 26 (Tahoe), Darwin 25.5.0, Apple Silicon (arm64e) — primary target
    - Builds as a universal binary (arm64e + x86_64)

> **Note on Apple Silicon:** under Pointer Authentication (PAC) the kernel's
> private symbols cannot be linked from a kext, and the in-memory symbol table is
> jettisoned after boot. Affected features are recovered either by forward-porting
> (e.g. `fd/`, `threads/`, `cmdline`) or by resolving the needed private symbols
> from the on-disk kernel collection via libklookup (e.g. `tty`). Anything still
> out of reach degrades gracefully (returns `ENOTSUP`/empty) rather than failing
> the mount. See [Feature status](#feature-status) below.

### Root directory

At the root of the file system, alongside the per-process directories, are a few
Linux-compatible files and helpers:

| Entry        | Summary                                                              |
|--------------|---------------------------------------------------------------------|
|`allocinfo`   | Linux-style memory-allocation profiling; macOS has no code-tag profiling, so this is the zone allocator (`mach_zone_info`, via `procfsd`): one row per zone with live bytes, live count and the zone name in place of Linux's `file:line func:` tag |
|`apm`         | Linux-style advanced-power-management line (AC status, battery charge %, time remaining) mapped from IOKit power sources via the `procfsd` daemon |
|`bootconfig`  | Linux-style boot configuration; macOS has no bootconfig blob, so this is the boot loader's boot-args (`kern.bootargs`) with the Linux `# Parameters from bootloader:` note |
|`buddyinfo`   | Linux-style buddy-allocator free-block counts; macOS is not a buddy allocator, so the free page count is decomposed into buddy orders (maximally coalesced) — one `Node 0, zone Normal` line |
|`bus/`        | Bus-specific info directory; macOS provides PCI via IOKit — `bus/pci/devices` is the Linux PCI device table (bus/devfn, vendor:device, name from `IOPCIDevice`; via the `procfsd` daemon) |
|`byname/`     | Directory of symbolic links, one per process, named by command name |
|`cmdline`     | Kernel boot command line (macOS boot-args / `kern.bootargs`; Linux's root `/proc/cmdline`) |
|`cpuinfo`     | Linux-style CPU information (text)                                   |
|`curproc/`     | Symbolic link to the calling process's directory (BSD name)         |
|`devices`     | Linux-style char/block device major-number listing (driver families reconstructed from `/dev` by the `procfsd` daemon) |
|`diskstats`   | Linux-style block-device I/O statistics (per whole disk, 14-field format; from IOKit `IOBlockStorageDriver`) |
|`dma`         | Linux-style list of ISA DMA channels in use; an x86-only concept — shows the reserved `cascade` channel on x86, empty on Apple Silicon (no 8237 ISA DMA) |
|`driver/`     | Directory grouping driver-specific files; currently `driver/rtc`, the same real-time-clock state as `/proc/rtc` |
|`execdomains` | Linux-style registered execution personalities; macOS has no exec-domain subsystem, so the single native personality is reported (`0-0  Darwin  [kernel]`) |
|`extensions`  | macOS-style list of loaded kernel extensions (kextstat-like: index, refs, address, size, name/version; via the `procfsd` daemon) |
|`fb`          | Linux-style framebuffer device list (`<index> <name>`); macOS IOKit framebuffers (`IOFramebuffer`/`IOMobileFramebuffer`) via the `procfsd` daemon |
|`filesystems` | Linux-style filesystem-type list (the mounted types, deduped; `nodev` for device-less) |
|`fs/`         | Filesystem-parameters directory; currently `fs/nfs/exports`, the NFS export table (macOS `/etc/exports`, read by the `procfsd` daemon) |
|`interrupts`  | Linux-style interrupt table (IRQ → controller → device); counts are 0 (macOS exposes no per-CPU interrupt counts), but the IRQ topology from IOKit is real (via the `procfsd` daemon) |
|`irq/`        | IRQ-to-CPU affinity directory; `default_smp_affinity`/`_list` (all online CPUs). macOS routes IRQs via the AIC with no user-settable per-IRQ affinity, so per-IRQ subdirectories are omitted |
|`loadavg`     | Linux-style load averages (text; true values via the `procfsd` daemon, CPU-utilisation approximation as fallback — see below) |
|`meminfo`     | Linux-style memory summary (text; `MemFree` is the FreeBSD non-wired estimate on Apple Silicon — see below) |
|`modules`     | Linux-style `/proc/modules` view of the same loaded kexts (`name size refcount deps state address`) |
|`mounts`      | The Linux name for the same mounted-filesystem table as `mtab`       |
|`mtab`        | Linux-style mounted-filesystem table (`/etc/mtab` format: `device mountpoint fstype options 0 0`) |
|`net/dev`     | Linux-style per-interface network statistics (in-kernel via the `ifnet` KPIs; fifo/frame/compressed/carrier columns are 0 — macOS keeps no such counters) |
|`partitions`  | Linux-style partition table (text; all block devices via IOKit — see below) |
|`rtc`         | Linux-style real-time-clock state; `rtc_time`/`rtc_date` are the UTC calendar time (`clock_get_calendar_microtime`), alarm/IRQ fields report their inactive defaults |
|`self/`        | Symbolic link to the calling process's directory (Linux name)       |
|`stat`        | Linux-style kernel/system statistics (`cpu`/`cpuN` ticks, `btime`, `processes`; see below) |
|`swaps`       | Linux-style swap-area table (aggregate `vm.swapusage`; macOS swaps dynamically under `/private/var/vm`) |
|`sys/`        | Dynamic mirror of the kernel sysctl MIB tree (Linux `/proc/sys`); directories are sysctl nodes, leaves read their value as text |
|`uptime`      | Linux-style uptime (seconds since boot; idle field `0.00`)          |
|`version`     | Kernel version string (text)                                        |
|`vmstat`      | Linux-style virtual-memory statistics (daemon-backed `host_statistics64`; see below) |

### Per-process files

Each directory named for a process id represents one process on the system. By default you can only see your own processes, although it is possible to set an option (`noprocperms`) when mounting the file system that will let you see and get details for every process. Obviously this is a security risk, so it’s not the default mode of operation. Within each process directory are the following files and two further directories. Most files contain binary structures rather than text, so they are intended to be used in applications rather than for direct human consumption. You’ll find definitions of the structures in this table in the file */usr/include/sys/proc_info.h*.

| File    | Summary                          | Structure                       |
|---------|----------------------------------|---------------------------------|
|`auxv`     | XNU's auxiliary-vector equivalent — dyld's `apple[]` array (`key=value` strings) | text (NUL-separated) |
|`cmdline`  | Process argument vector (NUL-separated, Linux format) | text |
|`comm`     | Process (command) name | text |
|`cwd/`      | Symlink to the current working directory | symlink |
|`environ`  | Process environment (NUL-separated, Linux format) | text |
|`exe`      | Symlink to the executable | symlink |
|`fd/`       | File descriptor| directory |
|`fpregs`   | Representative thread's FP/SIMD registers, native `arm_neon_state64_t` (x86_64: `x86_float_state64_t`) — served by the daemon | binary |
|`limit`    | Process resource limits, one `<name> <cur> <max>` line per limit (`-1` = unlimited) | text |
|`map`      | Process virtual-memory regions, NetBSD `procfs` format (address range, cur/max prot, sharing, wired) | text |
|`maps`     | Process virtual-memory regions, Linux `/proc/<pid>/maps` format | text |
|`mem`      | Process memory; the read offset is the virtual address (NetBSD/Linux `mem` semantics). Resident pages only — see below | binary |
|`note`     | Write a note to the process (NetBSD/Plan 9-style) | write-only (read returns `EINVAL`); a note delivers a signal to the process — see below |
|`numa_maps`| Linux `/proc/<pid>/numa_maps`: per-mapping NUMA locality (single-node: policy `default`, `N0=`) | text |
|`pid`      | Process id                       | `pid_t` (binary `int32`)         |
|`pgid`     | Process group id                 | `pid_t` (binary `int32`)         |
|`ppid`     | Parent process id                | `pid_t` (binary `int32`)         |
|`regs`     | Representative thread's general registers, native Mach `arm_thread_state64_t` (x86_64: `x86_thread_state64_t`) — served by the `procfsd` daemon | binary |
|`root/`     | Symlink to the root directory | symlink |
|`sid`      | Session id                       | `pid_t` (binary `int32`)         |
|`io`       | Linux per-process I/O accounting; `read_bytes`/`write_bytes` are real disk I/O (via the `procfsd` daemon's `proc_pid_rusage`), the `rchar`/`wchar`/`syscr`/`syscw`/`cancelled_write_bytes` fields are 0 (untracked on macOS) | text |
|`smaps`    | Linux `/proc/<pid>/smaps`: per-region memory detail (`Rss`/`Pss`/dirty/`Swap`/`VmFlags`) | text |
|`smaps_rollup` | Linux `/proc/<pid>/smaps_rollup`: the `smaps` fields summed across all mappings (one `[rollup]` block) | text |
|`stat`     | Linux single-line process stat (52 space-separated fields) | text |
|`statm`    | Linux memory usage in pages (`size resident shared text lib data dt`) | text |
|`status`   | Basic process info (mode-switched) | native: `struct proc_bsdshortinfo`; linux: `Name:/State:/Pid:/Uid:/VmRSS:…` text |
|`task/`       | Linux-style task directory | directory |
|`taskinfo` | Info for the process’s Mach task | `struct proc_taskinfo` — exact via the `procfsd` daemon; falls back to the kext’s partial fill without it (see Feature status) |
|`threads/`   | Thread directory (BSD name), one subdirectory per thread id | directory |
|`tty`      | Controlling terminal device path (e.g. `/dev/ttys001`) | text |

The `fd` directory contains one entry for each file that the process has open. Each entry is a directory that’s numbered for the corresponding file descriptor. Within each subdirectory you’ll find two files called `details` and `socket`. The `details` file contains a `vnode_fdinfowithpath` structure, which contains information about the file including its path name if it is a file system file. If the file is a socket endpoint, you can read a `socket_fdinfo` structure from the `socket` file.

The `threads` directory contains a subdirectory for each of the process’ threads, named by thread id (TID). Each thread directory contains a file called `info` that holds a `proc_threadinfo` structure. Thread *enumeration* works on Apple Silicon (the directory lists the real thread ids), and the per-thread `info` *contents* are now supplied exactly by the `procfsd` daemon (`proc_pidinfo(PROC_PIDTHREADID64INFO)`); they read zero only when no daemon is connected (the private `fill_taskthreadinfo` is stripped from the arm64 kernel).

The `task` directory mirrors the `threads` directory except that its TID sub-directories also contain Linux-like files. 

| File    | Summary                          | Structure                       |
|---------|----------------------------------|---------------------------------|
|`comm`   | Exposes and allows modification of a specific thread's command name (comm value) | text |
|`info`   | The `proc_threadinfo` structure| `pid_t` (binary `int32`)   |
|`maps`   | Displays the memory layout and mapped memory regions for a specific thread within a process | text |
|`sched`   | Provides real-time scheduler statistics and execution data for a specific thread | text |
|`stat`   | Provides detailed status and performance statistics for a specific thread | text |
|`status`   | Provides human-readable status information for a specific thread | text |

## Feature status

Verified with `tests/test_features.sh`.

**Working (real data):**

  - Directory listing of the root and per-process directories via `ls`, `find`, `readdir(3)` and `getdirentries64(2)`
  - `version` — kernel version string
  - `cpuinfo` — Linux-style CPU information (some flag fields incomplete; see Issues)
  - `loadavg` — process count plus the true 1/5/15-minute load averages from the
    `procfsd` daemon (`getloadavg`); falls back to a CPU-utilisation approximation
    when no daemon is connected (see Apple Silicon note)
  - `meminfo` — Linux-style memory summary; `MemTotal` and `MemFree` are
    populated (`MemFree` via the FreeBSD non-wired estimate — see Apple Silicon note)
  - `partitions` — Linux-style table of block devices via IOKit (`IOMedia`):
    every whole disk and partition, mounted or not, with real major/minor, 1 K
    block counts and BSD names (falls back to the mounted-only list without
    IOKit — see note below)
  - `mtab` — Linux `/etc/mtab`-style table of every mounted filesystem
    (`device mountpoint fstype options 0 0`), via `vfs_iterate` + `vfs_statfs`
  - `stat` — Linux `/proc/stat`: per-CPU `cpu`/`cpuN` user/nice/system/idle
    ticks (from `processor_info`, the loadavg per-CPU source), `btime`
    (`kern.boottime`) and `processes`; interrupt/ctxt/fork counters read 0
  - `vmstat` — Linux `/proc/vmstat`: VM page counters from the `procfsd` daemon's
    `host_statistics64(HOST_VM_INFO64)`, mapped onto Linux keys (`nr_free_pages`,
    `pgpgin`/`pgpgout`, `pgfault`, …); zero without the daemon
  - `curproc` symlink and the `byname/` directory of per-process symlinks
  - Per-process `pid`, `ppid`, `pgid`, `sid` (binary `int32`)
  - Per-process `status` — `proc_bsdshortinfo` (pid/ppid/pgid, status, command
    name, real/effective/saved uids and gids, process flags)
  - `cmdline` — the process's argument vector, NUL-separated (the Linux
    `/proc/<pid>/cmdline` format); zombies and system processes report `(comm)`
  - `environ` — the process's environment, NUL-separated (Linux
    `/proc/<pid>/environ`), read from the same argument region as `cmdline`
  - `comm`, `stat`, `statm` — the process's Linux `/proc/<pid>/comm` name,
    single-line 52-field `stat`, and page-count `statm`
  - `status` — native binary `proc_bsdshortinfo`, or Linux `Name:/State:/Pid:/
    Uid:/VmRSS:…` text when the `procfs.linux` sysctl is set (see below)
  - `exe`, `cwd`, `root` — per-process symlinks to the executable, current
    directory and root directory (`vn_getpath` on `p_textvp` / `fd_cdir` /
    `fd_rdir`)
  - `self` (root) — symlink to the caller's own process directory (Linux name
    for `curproc`); `uptime`, `mounts` (root) — Linux `/proc/uptime` and the
    Linux name for `mtab`
  - `swaps`, `filesystems` (root) — Linux `/proc/swaps` (aggregate
    `vm.swapusage`) and `/proc/filesystems` (mounted types, deduped, with the
    `nodev` prefix)
  - `sys/` (root) — a live mirror of the kernel's sysctl MIB tree (Linux
    `/proc/sys`): every sysctl node is a directory and every leaf a text file
    holding its formatted value (`kern/ostype`, `kern/hostname`, `hw/ncpu`, …).
    Values come from the `procfsd` daemon for full coverage, with an in-kernel
    fallback that reaches the `CTLFLAG_KERN` subset (see below)
  - `fd/` — enumerates the process's open file descriptors; per-fd `details`
    (`vnode_fdinfowithpath`) and `socket` (`socket_fdinfo`, common fields plus
    UNIX/IPv4 addresses)
  - `threads/` — enumerates the process's threads (one directory per thread id)
  - `task/` — the Linux name for the same per-thread view (`/proc/<pid>/task/<tid>`),
    one directory per thread id, with Linux-format per-thread files: `comm`,
    `stat`, `status`, `sched`, `maps` (and the binary `info`). Per-thread data
    (name, run state, user/system CPU time, priority, policy) comes from the
    `procfsd` daemon via `proc_pidinfo(PROC_PIDTHREADID64INFO)`; fields with no
    macOS source (Linux fault counters, CFS scheduler internals, register
    addresses, ctxt-switch counts) are reported as 0
  - `tty` — the process's controlling terminal device path (e.g. `/dev/ttys001`),
    empty when it has none
  - `limit` — the process's resource limits (FreeBSD `procfs_rlimit` format: one
    `<name> <cur> <max>` line per limit, `-1` for unlimited)
  - `mem` — the process's memory; the read offset is the virtual address (the
    NetBSD/Linux `mem` semantics). Only resident pages are returned (see Apple
    Silicon note)
  - `map` / `maps` — the process's virtual-memory regions (`map` in NetBSD
    procfs format, `maps` in Linux `/proc/<pid>/maps` format), with address
    ranges and protections (see Apple Silicon note)
  - `smaps` — Linux `/proc/<pid>/smaps`: each `maps` region followed by
    per-region memory detail (`Size`, `Rss`, `Pss`, shared/private clean/dirty,
    `Anonymous`, `Swap`, `VmFlags`) from `VM_REGION_EXTENDED_INFO`; `Pss` and
    `Referenced` approximate `Rss`, and the region's share mode classifies its
    whole `Rss` as shared or private (see Apple Silicon note)
  - `smaps_rollup` — the same `smaps` fields summed across every mapping into a
    single `[rollup]` block (Linux's faster-than-parsing-`smaps` accumulation)
  - `numa_maps` — one line per mapping with its NUMA locality; Apple Silicon is
    single-node, so the policy is always `default` and resident pages report as
    `N0=` (with `anon=`/`dirty=` from `VM_REGION_EXTENDED_INFO`)
  - `regs` / `fpregs` — the representative thread's general and FP/SIMD register
    state as the native Mach `arm_thread_state64_t` / `arm_neon_state64_t`
    (x86_64: `x86_thread_state64_t` / `x86_float_state64_t`), supplied by the
    `procfsd` daemon; `EPERM` for Apple platform/hardened binaries, `ENOTSUP`
    without a daemon (see below)
  - `auxv` — XNU's auxiliary-vector equivalent: dyld's `apple[]` array
    (`executable_path=`, `stack_guard=`, `dyld_file=`, `malloc_entropy=`,
    `arm64e_abi=`, …), read from the target's user stack via its pmap

`cmdline`, `fd/` and `threads/` required forward-porting work to function under
PAC on Apple Silicon rather than relying on the unavailable private KPIs: `fd/`
walks the process's file-descriptor table directly, `threads/` enumerates threads
via the BSD `proc->p_uthlist` instead of the inaccessible Mach `task->threads`
queue, and `cmdline` reads the target's user-stack arguments through its pmap
(the `KERN_PROCARGS2` `vm_map_copyin` path is `com.apple.kpi.private`).

`tty` takes a different route. Its accessor `proc_gettty` is
`com.apple.kpi.private` and reaches the terminal through the SMR-protected
`p->p_pgrp`, so it cannot be linked or safely forward-ported. Instead it is
resolved at load time from the on-disk kernel collection (libklookup, fed by the
`procfs_ksyms` staging helper run at install) and called directly — its SMR and
session locking run inside the kernel's own code, so the resolved call is safe.

`loadavg`'s load values come from the `procfsd` daemon's `getloadavg()` — the
kernel's true 1/5/15-minute averages — when it is connected. They are not
reachable from the kext itself: `averunnable`, `compute_averunnable`,
`host_statistics` and `processor_set_info` are all stripped from the kernel and
unexported. So without a daemon the node falls back to a CPU-utilisation
approximation: a `thread_call` samples per-CPU tick counts via the exported
`processor_info(PROCESSOR_CPU_LOAD_INFO)` (with a `processor_t` from the
libklookup-resolved `cpu_to_processor()`) every 5 seconds and feeds
`utilisation × ncpu` through the standard load-average EWMA. That approximation
tracks CPU utilisation rather than run-queue depth, so it saturates near the CPU
count and under-reports a genuinely overloaded machine; it reads `0.00` if
libklookup cannot resolve `cpu_to_processor` either.

`meminfo` reports `MemTotal` from the `hw.memsize` sysctl and `MemFree` using
FreeBSD's `linprocfs_domeminfo` estimate (`MemFree = MemTotal − wired`). The
wired-page count comes from the kernel's `vm_page_wire_count` global resolved
via libklookup, because the `vm.*` page-count sysctls are not readable from
kernel context and most `vm_page_*_count` globals are stripped on arm64.
`Cached`, `Buffers` and swap have no kernel-reachable source there and read 0
(`MemFree` reads 0 if libklookup cannot resolve the wired count).

`mem` reads the target's memory through its pmap (`get_task_pmap`,
`pmap_find_phys`, `ml_phys_read`) — the same physical-aperture path `cmdline`
uses — since the BSD faulting path (`vm_map_copyin`) is `com.apple.kpi.private`.
A consequence of not faulting is that only resident pages are returned: reading
an unmapped or paged-out address (including offset 0, the NULL page) returns
`EIO`, and a read stops cleanly at the first hole. Access is gated by the same
credential check as the rest of the filesystem.

`map` and `maps` enumerate the process's VM regions through the libklookup-
resolved `mach_vm_region()` (with `get_task_map()`): macOS exports no region-
enumeration KPI a kext may link, and the internal walkers are stripped on
arm64. `mach_vm_region()` takes the map's read lock itself, so this needs no
struct-walking or manual locking. Both nodes share one walk and differ only in
formatting (`map` NetBSD-style, `maps` Linux-style). Backing-file paths (the
trailing Linux column) are not resolved — the region's object→vnode is not
reachable here — so the device/inode/path columns read `00:00 0`.

`regs` and `fpregs` expose a thread's register state. `thread_get_state()` is
neither a bindable KPI symbol nor present in the arm64 kernelcache symbol table
(so it can be neither linked *nor* resolved via libklookup), so the kext cannot
read register state itself. The `procfsd` daemon does it from userspace instead —
`task_for_pid()` + `task_threads()` + `thread_get_state()` on the process's
representative thread — and returns the native Mach state struct over the
kernel-control bridge. Because `task_for_pid` is denied to root for Apple
platform/hardened binaries under SIP/AMFI, those processes return `EPERM` — the
same permission model as `ptrace` on Linux. On arm64e the saved `pc`/`lr` carry
their PAC bits and are emitted raw, leaving stripping to the consumer.

`auxv` reports XNU's equivalent of the ELF auxiliary vector. macOS binaries are
Mach-O, so there is no `AT_*` array on the stack; the closest analog is dyld's
`apple[]` array — the `key=value` strings the kernel places after `argv`/`envp`.
The node finds them by skipping the `argc` slot and the `argv`/`envp` pointer
arrays on the target's user stack (read through its pmap, as `cmdline` does) and
following each `apple` pointer to its string. Zombies and system processes report
empty.

`partitions` enumerates block devices through IOKit, matching the `IOMedia`
class — every whole disk and partition, mounted or not, exactly as Linux's
`/proc/partitions` does. That data lives in the IORegistry, reachable only via
the C++ IOKit runtime (the C IOKit KPI exposes no registry matching), so this is
the kext's one C++ translation unit (`procfs_iokit.cpp`): it matches `IOMedia`
by name and reads each object's `BSD Name`/`BSD Major`/`BSD Minor`/`Size`
properties off the base `IORegistryEntry`, needing only the base IOKit and
libkern KPIs (no dependency on `IOStorageFamily`'s `IOMedia` class). Each row is
`major minor #blocks name` with the 1 K block count from the media size. If
IOKit matching fails, the node falls back to the mounted-filesystem list
(`vfs_iterate()` + `vfs_statfs()`), which shows mounted volumes only.

`diskstats` shares that same C++ IOKit translation unit for the Linux
`/proc/diskstats` node. It enumerates the whole-disk `IOMedia` entries
(`Whole` = true) and reads the cumulative I/O counters from each disk's
`IOBlockStorageDriver` provider (the `Statistics` dictionary: `Operations`,
`Bytes` and `Total Time` for reads and writes) off the base `IORegistryEntry`.
Each line is the classic 14-field Linux format (`major minor name reads
rd_merged rd_sectors rd_ticks writes wr_merged wr_sectors wr_ticks in_flight
io_ticks time_in_queue`); bytes become 512-byte sectors and nanosecond service
times become milliseconds. macOS keeps no per-device merge, in-flight or queue
accounting, so those fields read 0 and `io_ticks` is approximated as the sum of
read and write service times.

`sys/` is a dynamic mirror of the kernel's sysctl MIB — the macOS counterpart
to Linux's `/proc/sys`. Rather than static structure nodes, every `/proc/sys`
vnode is one shared marker node distinguished by the kernel address of its
`struct sysctl_oid` (carried in the node id; the tree root is `sysctl__children`);
a `CTLTYPE_NODE` oid is a directory and any other oid is a leaf. Directory
listing and lookup walk the live `oid_link` sibling lists, so the tree always
reflects the kernel's current MIB (internal `__anchor__(…)` and `CTLFLAG_MASKED`
oids are hidden). A leaf read builds the dotted MIB name and formats the value by
the oid's declared type (`int`/`quad`/`string`; opaque/struct oids read empty).
The value itself is fetched from the `procfsd` daemon (userspace `sysctlbyname`,
which serves *every* oid) and falls back to in-kernel `sysctlbyname()` when no
daemon is connected. The kernel path only serves oids marked `CTLFLAG_KERN`, so
without the daemon the non-`KERN` leaves (e.g. `kern.hostname`, `kern.maxproc`)
read empty; invoking the oid handler in-kernel to bypass that gate is unsafe —
the custom handlers assume the sysctl lock is held and panic — so the daemon is
the path to full coverage. Because every sysctl vnode shares one structure node,
`..` traversal is resolved specially: from a nested `/proc/sys` directory it walks
back to the enclosing sysctl oid (and from `/proc/sys` itself to the `/proc`
root), so relative paths through the tree behave normally.

`extensions` lists the loaded kernel extensions, macOS's answer to the module
list, in a `kextstat`-style format (load index, reference count, load address,
size, wired size, bundle id and version). A kext cannot enumerate loaded kexts
itself — that needs the private C++ `OSKext` class, which is neither a bindable
KPI nor safe to call — so the listing is produced by the `procfsd` daemon via
`KextManagerCopyLoadedKextInfo()` and delivered over the control bridge. The
listing (200+ kexts, tens of KB) far exceeds one bridge payload, so it is
streamed in `PROCFS_CTL_MAXPAYLOAD`-sized chunks (`procfs_ctl_request_blob()`):
the daemon rebuilds the listing when a read starts at offset 0 and returns the
slice for each subsequent offset, and the kext reassembles them into an `sbuf`.
Without a connected daemon the node is empty. `modules` is the same listing in
Linux `/proc/modules` format (`name size refcount deps state address`, with
`deps` always `-` and state always `Live`, since macOS exposes no per-kext
dependency string here); it shares the entire chunked-transfer path and differs
only in the daemon-side formatting.

`allocinfo` is the Linux memory-allocation profiling node (`/proc/allocinfo`).
Linux keys it on code tags (`file:line func:name`); macOS has no code-tag
allocation profiling, so the closest faithful source is the **zone allocator** —
the same `mach_zone_info()` data `zprint` reports. Because `mach_zone_info`
requires the privileged host port, the `procfsd` daemon gathers it and emits one
row per zone — `<live bytes> <live count> zone:<name>`, where live bytes is the
zone's element count times its element size and the zone name stands in for
Linux's code tag — sorted by size descending, streamed over the same chunked
transfer as `extensions`/`modules`/`devices`. Empty without a connected daemon.

`apm` is the Linux advanced-power-management line (`/proc/apm`). macOS has no APM,
so the power state comes from IOKit power sources (the data `pmset -g batt`
reports): the `procfsd` daemon reads the AC/battery state, charge percentage,
charging flag and time remaining, and the kext maps them onto the classic APM
format — `driver_ver bios_ver bios_flags ac_status battery_status battery_flag
percentage% time units` — using the Linux apm-emulation status/flag byte
encoding (e.g. `0x01`/`0x00` AC on/off; battery status high/low/critical/charging;
time in minutes). A machine with no battery reports the no-battery flag `0x80`;
the node is empty without a connected daemon.

`bootconfig` is Linux's boot-configuration node (`/proc/bootconfig`). Linux fills
it from a bootconfig blob appended to the initrd, followed by a `# Parameters
from bootloader:` note carrying the original bootloader command line. macOS has
no such blob — its only boot configuration is the boot-args the boot loader
(iBoot / NVRAM) passes to the kernel (`kern.bootargs`, the same source as
`/proc/cmdline`). So the boot-args are emitted as the boot config and, because on
macOS the kernel parameters come from the boot loader, repeated in the
`# Parameters from bootloader:` form. Read in-kernel with the same `procfsd`
sysctl-bridge fallback as `/proc/cmdline`; empty when no boot-args are set.

`buddyinfo` is Linux's buddy-allocator free-block report (`/proc/buddyinfo`):
one line per node and zone giving the number of free blocks of each order
(`2^order` pages), orders 0 through `MAX_ORDER-1`. macOS is not a buddy allocator
and exposes no per-order free lists, so there is no real fragmentation data.
Instead the free page count (`host_statistics64`'s `free_count`, from the daemon
via the same request as `vmstat`) is decomposed greedily into buddy orders,
largest first — a valid buddyinfo whose blocks account for exactly the free pages
(`Σ count[order] × 2^order = free_pages`), i.e. free memory presented as maximally
coalesced. Apple Silicon is single-node UMA, so a single `Node 0, zone Normal`
line; all-zero without a connected daemon.

`bus/` is Linux's directory of bus-specific information. The classic content is
`bus/pci/devices`, the PCI device table. macOS exposes PCI through the IORegistry
(`IOPCIDevice`) rather than a `/proc` table, so the `procfsd` daemon enumerates
those devices and formats the Linux line for each: `bus/devfn`, `vendor/device`
id (from the little-endian `vendor-id`/`device-id` properties) and the device
name (from `IOName`), in the standard 18-field layout. IRQ, base addresses and
region sizes are not read from IOKit and report 0. `/proc/bus` and `/proc/bus/pci`
are plain directories; the `devices` listing is empty without a connected daemon.

`dma` lists the ISA DMA channels in use (one `%2d: <owner>` line per busy channel
of the legacy 8237 controller). This is an x86-only concept: on x86 the DMA
subsystem always reserves channel 4 as the cascade wiring the two 8237
controllers, so `/proc/dma` there is never empty. macOS/XNU uses no 8237 ISA DMA,
and Apple Silicon has no such hardware at all, so the node shows only the
reserved `cascade` channel on x86 and is empty on arm64 — matching Linux's own
per-architecture behaviour.

`rtc` reports the real-time-clock state (Linux `drivers/rtc/proc.c`). The core is
the current RTC time and date; macOS keeps its hardware RTC in UTC, exposed via
`clock_get_calendar_microtime()`, so `rtc_time`/`rtc_date` are the UTC calendar
time (epoch seconds converted to a civil date in-kernel, since the kernel has no
`gmtime`). macOS does not expose the RTC's alarm or periodic-interrupt state to a
kext, so the `alrm_*`/`*IRQ*` fields report their inactive defaults (no alarm,
IRQs off, 24-hour mode, `batt_status: okay`). Fully in-kernel. The same content
is also served at `/proc/driver/rtc`, where Linux groups driver-specific files.

`execdomains` lists the kernel's registered execution personalities (the legacy
`personality(2)` / `exec_domain` mechanism for running foreign-OS binaries).
Modern Linux keeps only the native personality and emits a single fixed line,
`0-0\tLinux\t[kernel]`. macOS has no exec-domain subsystem; its native
personality is Darwin/Mach-O, so — as `/proc/version` reports Darwin rather than
Linux — the sole domain is reported with the native name: `0-0  Darwin  [kernel]`.
Fully in-kernel.

`fb` lists the registered framebuffer devices, one `<index> <name>` line each.
macOS drives displays through IOKit framebuffers — `IOFramebuffer` on Intel,
`IOMobileFramebuffer` on Apple Silicon — so the `procfsd` daemon enumerates both
classes and formats a line per device, using the device's IORegistry name as the
Linux `fix.id` (e.g. `0 AppleCLCD2`). Streamed over the same chunked transfer as
`/proc/bus/pci/devices`; empty without a connected daemon.

`interrupts` is the Linux per-CPU interrupt table (`/proc/interrupts`), one line
per IRQ with the controller and owning device. macOS does not expose per-CPU
interrupt counts to userspace or a kext (only the private IOReporting framework
has them), so the count columns are 0. The IRQ *topology*, though, is real: the
`procfsd` daemon walks the IORegistry for each device's `IOInterruptSpecifiers`
(the IRQ numbers) and `IOInterruptControllers`, producing genuine
IRQ → controller → device rows (e.g. `cpu0`, `spi2`, `i2c1`, `pci-bridge0`),
sorted by IRQ. Streamed over the same chunked transfer as `/proc/bus/pci/devices`;
empty without a connected daemon.

`irq/` is Linux's IRQ-to-CPU affinity tree. On Linux each `/proc/irq/<N>/` holds
an `smp_affinity` bitmask naming the CPUs that may service IRQ `<N>`, plus a
`default_smp_affinity` for new IRQs. macOS routes interrupts through the AIC with
no user-settable or per-IRQ-queryable CPU affinity — every IRQ may run on any
CPU — so only the default masks are exposed and the per-IRQ subdirectories are
omitted. The directory holds `default_smp_affinity` (hex cpumask of all online
CPUs, e.g. `ff` for 8 CPUs, comma-grouped in 32-bit words for more) and
`default_smp_affinity_list` (the CPU range, e.g. `0-7`). Fully in-kernel.

`fs/` is Linux's directory of filesystem parameters; the classic entry is
`fs/nfs/exports`, the NFS export table. Linux shows the kernel NFS server's
active exports there; macOS keeps the export configuration in `/etc/exports`
(which `nfsd` registers with the kernel), so the `procfsd` daemon serves that
file's contents. `/proc/fs` and `/proc/fs/nfs` are plain directories; the
`exports` listing is empty when no NFS server is configured (no `/etc/exports`)
or when no daemon is connected.

### The `procfsd` daemon

Several fields are unreachable from a kext on Apple Silicon: the kernel
functions that produce them (`fill_taskprocinfo`, `task_info`, per-thread info,
the VM-statistics globals) are stripped from the kernel symbol table, and the
data lives in per-CPU/`recount` structures with no linkable accessor. The
`procfsd` userspace daemon supplies these via `libproc`'s `proc_pidinfo()` and
`host_statistics64()` — the same interfaces `top`/Activity Monitor use — over a
privileged `PF_SYSTEM` kernel control (`procfs_ctl.c`): a node read sends a
request and the daemon replies. So `taskinfo` (all 18 fields exact),
`task/<tid>/{info,comm,stat,status,sched}`, `vmstat`, the `sys/` sysctl values
and the `extensions`/`modules` kext listings are fully populated when the daemon runs, and
fall back to the kext's best-effort values (or zero, the `CTLFLAG_KERN` sysctl
subset, or an empty node) when it does not. The daemon also stages the libklookup symbol file at boot and,
when armed, loads the kext; see *Installing*. `taskinfo`'s `pti_resident_size`
is then the exact `phys_footprint` from the daemon (the kext's own estimate is
only the fallback). Without the daemon the per-thread `info` reads zero
(`fill_taskthreadinfo` is stripped from the arm64 kernel).

The daemon is also the *only* source for the `regs`/`fpregs` register nodes:
`thread_get_state()` is unreachable from the kext (neither bindable nor in the
kernelcache symtab), so those nodes require a connected daemon and return
`ENOTSUP` without one, or `EPERM` for a `task_for_pid`-denied (SIP/AMFI) target.

`note` is a writable per-process node (NetBSD `procfs_donote()` lineage). macOS
has no native "note" primitive, so — following the Plan 9 origin of procfs notes,
where a note is the signal mechanism — writing a note delivers a signal to the
target: a recognised name (`hup`, `int`/`interrupt`, `quit`, `kill`, `term`,
`stop`, `cont`, `usr1`, `usr2`) or a numeric signal is posted via `proc_signal()`,
and anything else returns `EINVAL`. Reads return `EINVAL` (the node is
write-only), as on NetBSD. Permission is the filesystem's own model: the node's
write mode is owner/group (or root under `noprocperms`), so only a user who owns
the target process — or root — can open it for writing. Writing is via the newly
added `vnop_write`; a companion `vnop_setattr` accepts the `O_TRUNC` that shells
issue on `>` so `echo kill > /proc/<pid>/note` works. Because a read-only mount
makes the VFS reject every write before it reaches the filesystem, the mount is
no longer `MNT_RDONLY` (as with Linux `/proc`); non-writable nodes still reject
writes themselves.

**Presentation mode (native vs Linux):** the `procfs.linux` sysctl selects how
nodes that have both renderings present themselves — `0` (default) = native
BSD/XNU, `1` = Linux-compatible. It is a live global toggle:

    sudo sysctl -w procfs.linux=1     # Linux-compatible
    sudo sysctl -w procfs.linux=0     # native (default)

Currently `status`, `regs`, `fpregs` and `auxv` honour it: in native mode
`status` emits the binary `proc_bsdshortinfo` and the register/auxv nodes emit
the binary Mach state / raw `apple[]` array; in Linux mode they emit the
human-readable text forms (`Name:/State:/…`, `x0 0x…`, `q0 0x…`, `AT_PAGESZ …`)
from `procfs_linux.c`. Other nodes keep their single format for now.

The sysctl lives in the kext, so it resets to `0` every time the kext loads.
To make the choice **persist across reboots**, the setting is saved to
`/var/db/procfs.linux` (just `0` or `1`) and `procfsd` re-applies it via
`sysctlbyname` each time the kext (re)appears — at boot and after any reload.
Toggling *Linux compatibility* in the menu-bar app writes that file for you (it
already runs the change with administrator rights); to persist a change made on
the command line, write the file yourself, e.g. `echo 1 | sudo tee
/var/db/procfs.linux`. Removing the file reverts to the native-by-default
behaviour.

## Menu-bar app

`ProcFS.app` is a lightweight menu-bar (status-bar) app for controlling procfs
without the command line. Its menu shows live status — whether `/proc` is
mounted, whether the `procfsd` daemon is running, and whether Linux presentation
mode is on — and offers one-click toggles for each (mount/unmount, Linux
compatibility on/off, start/stop daemon), plus the current version. The
mutating actions need root, so they go through the standard macOS
administrator-authorization prompt; status is read unprivileged. It is built as
part of `make` (into `bin/ProcFS.app`, version stamped from `VERSION`) and
installed to `/Applications` by `sudo make install`.

## How to build procfs
`make` builds the kext, the `procfs.fs` mount bundle, the userspace tools
(`procfsd`, `procfs_ksyms`), the LaunchDaemon plist and the `ProcFS.app`
menu-bar app into `bin/`:

    make                    # native arch (arm64e on Apple Silicon)
    make ARCH=universal     # fat arm64e + x86_64

To build and install in one step, use the install script — it runs
`make clean && make && sudo make install` and prompts for your password:

    ./install.sh                # native arch
    ./install.sh ARCH=universal

`sudo make install` **only copies** the prebuilt artifacts into place (it never
compiles, so `bin/` and the build tree stay owned by you and `make clean` never
needs sudo). It installs, with `root:wheel`/`755`: the kext to
`/Library/Extensions`, the `procfs.fs` bundle to `/Library/Filesystems`,
`procfsd`/`procfs_ksyms` to `/usr/local/sbin`, the LaunchDaemon plist to
`/Library/LaunchDaemons`, and `ProcFS.app` to `/Applications`. It also adds
`proc` to `/etc/synthetic.conf` (so `/proc`
is created at boot) and enables the LaunchDaemon. `sudo make uninstall` reverses
all of this — unmounts, unloads the kext, clears the staging cache, and removes
the installed files.

Code signing is optional; the kext is ad-hoc signed by default. To sign with
your own certificate instead, edit `Makefile.inc` and set the `SIGNCERT`
variable to the identity in your keychain.

Auto-load of the kext and auto-mount of `/proc` stay **disarmed** until you
create the arm flag, so a kext panic during development cannot boot-loop the
machine:

    sudo touch /var/db/procfs.enabled
    sudo reboot

After the reboot, `procfsd` stages the kernel symbols, loads the kext, and mounts
`/proc` for all users.

### Loading the kext (Apple Silicon, macOS 26)

Third-party kexts are loaded from the Auxiliary Kernel Collection (AuxKC). After
the first install you must approve the extension in **System Settings → Privacy
& Security** and reboot.

When **re-installing a rebuilt kext**, the build's code identity (cdhash)
changes, and a stale staging record will cause `kernelmanagerd` to reject it
with *"tried to insert an invalid codeful kernel extension in the restricted
lookup table."* `./install.sh` handles this: `make install`'s preinstall step
unmounts procfs, unloads the old kext and clears the staging cache before
copying the new build. To load into the running kernel without a reboot:

    sudo kmutil load -p /Library/Extensions/procfs.kext
    kextstat | grep procfs

## Mounting
With the arm flag set (see *How to build procfs*), `procfsd` mounts `/proc`
automatically at boot. `/proc` itself is created by `/etc/synthetic.conf`.

To mount manually (the kext must be loaded):

    sudo mount -t procfs procfs /proc

## Exploring the file system
Once mounted you can execute the `ls` command to list the contents:

    ls -l /proc

Or recursively:

    ls /proc/*/*/*/*

Likewise you can use the `cat` command to get the contents of a file:

    cat /proc/version
    cat /proc/sys/kern/ostype        # like Linux's /proc/sys/kern/ostype
    ls  /proc/sys/kern
    cat /proc/extensions             # kextstat-style loaded-kext listing

For per-process files that contain binary structures rather than text, you must pipe them
through `hexdump` to read the raw contents:

    cat /proc/curproc/taskinfo | hexdump -C
    cat /proc/self/regs | hexdump -C
    cat /proc/self/auxv | tr '\0' '\n'

## TODO:
 - Extend the `procfs.linux` presentation-mode switch to more nodes as native
   and Linux renderings diverge (only `regs`/`fpregs`/`auxv` differ today).
 - Implement more linux-compatible features (see the roadmap).

## Issues
Currently known issues:

- On Apple Silicon, `cmdline`, `fd/`, `threads/` and `tty` previously required private kernel symbols unavailable under PAC; they now work (the first three forward-ported, `tty` via libklookup-resolved `proc_gettty`). `tty` depends on the `procfs_ksyms` staging helper having run (it does during `make install`); if the staged symbol file is missing or stale for the running kernel, `tty` falls back to `ENOTSUP`.
- `taskinfo` and per-thread `info` are populated by the `procfsd` daemon (`proc_pidinfo`); they read the kext fallback / zero only when no daemon is connected, since the private `fill_taskprocinfo` / `fill_taskthreadinfo` are stripped from the arm64 kernel.
- `note` is writable: a note is delivered to the process as a signal (Plan 9 semantics — recognised name or numeric signal via `proc_signal()`, else `EINVAL`); reads return `EINVAL`. Gated by the node's owner/group write mode.
- `regs`/`fpregs` require the `procfsd` daemon (`thread_get_state` is unreachable from the kext — neither a bindable KPI nor in the arm64 kernelcache symtab) and return `EPERM` for Apple platform/hardened binaries, whose task ports `task_for_pid` denies even to root under SIP/AMFI — analogous to `ptrace` permissions on Linux.
- `partitions` now enumerates all block devices (whole disks and partitions, mounted or not) via IOKit's `IOMedia` class (`kext/procfs_iokit.cpp`, the kext's one C++ file). It falls back to the mounted-filesystem list (`vfs_iterate`) only if IOKit matching fails.
- The x86 `/proc/cpuinfo` `bugs` and `power management` fields are now populated (`kext/lib/cpu.c`): `bugs` from `IA32_ARCH_CAPABILITIES` plus CPU vendor/family (the common speculative-execution/errata classes; Linux's full per-model whitelist tables are not reproduced), and `power management` from CPUID `0x80000007`. The x86 flag getters (`get_cpu_flags` etc.) were also rewritten to use proper static buffers, fixing the earlier dangling-stack-pointer bug where flags did not "stick."
- AMD CPUs now emit their extended feature flags: on AMD the `flags` line is built from CPUID `0x80000001` EDX/ECX (`get_amd_feature_flags`/`get_amd_feature2_flags` - `svm`, `sse4a`, `3dnowprefetch`, `xop`, `fma4`, `tbm`, `topoext`, `perfctr_core`, `mwaitx`, `nx`, `lm`, ...) with Linux-compatible names, replacing the generic Intel-named extended getter for that vendor. AMD leaf-7 extended features are also read now (Zen, family >= 23), and the previously non-functional `set_microcode_version()` no longer dereferences a NULL `i386_cpu_info`.

## Contributing and Bug Reporting
If you wish to contribute to this project then feel free to make a pull request. If you encounter any undocumented bugs then you may also file an issue under the "Issues" tab.

## Credits
This project builds on the work of others, with thanks to:

- **Kim Topley** — the original proof-of-concept `procfs` for XNU that this project started from.
  <https://github.com/kimtopley/ProcFS>
- **leiless** — the `libkext` kernel-extension helper library.
  <https://github.com/leiless/libkext>
- **Syncretic** — the `klookup` kernel symbol-resolution code, from the `latebloom` project (0BSD).
  <https://github.com/reenigneorcim/latebloom>
- **Linus Henze** - for filesystem code pulled from his `Unrootless-Kext`
  <https://github.com/LinusHenze/Unrootless-Kext>
- **Acidanthera** - the `MacKernelSDK` macOS kernel SDK targeting various XNU versions.
  <https://github.com/acidanthera/MacKernelSDK>
- **Apple** — the XNU kernel source, headers and reference implementation.
  <https://github.com/apple-oss-distributions/xnu>
- **Apple** — the `libutil source and headers, needed by the mount_procfs tool.
  <https://github.com/apple-oss-distributions/libutil>
- **The NetBSD project** — `procfs` design and reference implementation.
  <https://github.com/NetBSD/src>
- **The NextBSD project** — additional `procfs` reference.
  <https://github.com/NextBSD/NextBSD/tree/NextBSD-CURRENT>
- **The FreeBSD project** — `procfs`/`pseudofs` reference implementation.
  <https://github.com/freebsd/freebsd-src>
