#!/bin/bash
#
# test_features.sh - exercises every implemented procfs feature and reports
# PASS/FAIL per node. Run against the live mount (default /proc); override with
# PROC=/path. Uses the current shell's own PID for permission-sensitive reads,
# so it does not need the noprocperms mount option.
#
# Daemon-backed nodes (loadavg true values, vmstat, taskinfo, regs/fpregs,
# extensions/modules, /proc/sys non-KERN values) require procfsd to be running;
# they are marked accordingly if empty.
#
PROC=${PROC:-/proc}
pass=0; fail=0; warn=0

ok()   { printf "  \033[32mPASS\033[0m %s\n" "$1"; pass=$((pass+1)); }
bad()  { printf "  \033[31mFAIL\033[0m %s\n" "$1"; fail=$((fail+1)); }
note() { printf "  \033[33mNOTE\033[0m %s\n" "$1"; warn=$((warn+1)); }   # not counted as failure
hdr()  { printf "\n== %s ==\n" "$1"; }

# text file: readable and non-empty
tfile() {
    local p="$1" d="${2:-$1}"
    if out=$(cat "$p" 2>/dev/null) && [ -n "$out" ]; then
        ok "$d: $(printf '%s' "$out" | head -1 | cut -c1-64)"
    else
        bad "$d ($(cat "$p" 2>&1 >/dev/null || true))"
    fi
}

# binary/struct file: readable and non-empty
bfile() {
    local p="$1" d="${2:-$1}"
    if [ -s "$p" ] && cat "$p" >/dev/null 2>&1; then
        ok "$d ($(wc -c < "$p" | tr -d ' ') bytes)"
    else
        bad "$d ($(cat "$p" 2>&1 >/dev/null || true))"
    fi
}

# directory: lists at least one real entry
tdir() {
    local p="$1" d="${2:-$1}"
    if e=$(ls "$p" 2>/dev/null) && [ -n "$e" ]; then
        ok "$d: $(printf '%s' "$e" | wc -w | tr -d ' ') entries"
    else
        bad "$d (empty or errored)"
    fi
}

# symlink: readlink yields a target
tlink() {
    local p="$1" d="${2:-$1}"
    if t=$(readlink "$p" 2>/dev/null) && [ -n "$t" ]; then ok "$d -> $t"; else bad "$d readlink"; fi
}

# daemon-backed text node: non-empty -> PASS; empty -> NOTE if procfsd is not
# running (its data source), else FAIL.
dfile() {
    local p="$1" d="${2:-$1}"
    if out=$(cat "$p" 2>/dev/null) && [ -n "$out" ]; then
        ok "$d: $(printf '%s' "$out" | wc -l | tr -d ' ') lines"
    elif [ "$daemon" = no ]; then note "$d empty (procfsd not running)"
    else bad "$d empty"; fi
}

# node with no macOS analog (legitimately empty, e.g. ISA/IDE): readable is
# enough - non-empty is reported, empty is a NOTE rather than a failure.
efile() {
    local p="$1" d="${2:-$1}"
    if out=$(cat "$p" 2>/dev/null); then
        if [ -n "$out" ]; then ok "$d: $(printf '%s' "$out" | head -1 | cut -c1-48)"
        else note "$d empty (no macOS analog)"; fi
    else bad "$d unreadable"; fi
}

read_i32() { python3 -c "import struct,sys;print(struct.unpack('<i',open(sys.argv[1],'rb').read(4))[0])" "$1" 2>/dev/null; }

[ -d "$PROC" ] || { echo "$PROC not mounted (set PROC=...)"; exit 2; }
SELF=$$
P="$PROC/$SELF"
daemon=$(pgrep -x procfsd >/dev/null && echo yes || echo no)
echo "procfs at $PROC | self pid $SELF | procfsd running: $daemon"

hdr "Root: directory + info files"
tdir  "$PROC"                 "root readdir"
tfile "$PROC/cpuinfo"         "cpuinfo"
tfile "$PROC/meminfo"         "meminfo"
tfile "$PROC/loadavg"         "loadavg"
tfile "$PROC/uptime"          "uptime"
tfile "$PROC/version"         "version"
tfile "$PROC/stat"            "stat"
tfile "$PROC/vmstat"          "vmstat"
tfile "$PROC/buddyinfo"       "buddyinfo (free blocks by order)"
tfile "$PROC/rtc"             "rtc (real-time clock)"
tfile "$PROC/execdomains"     "execdomains (personalities)"
tdir  "$PROC/driver"          "driver dir"
tfile "$PROC/driver/rtc"      "driver/rtc (grouped rtc)"
# dma: legitimately empty on arm64 (no 8237 ISA DMA); "4: cascade" on x86
if d=$(cat "$PROC/dma" 2>/dev/null); then
    if [ -n "$d" ]; then ok "dma: $d"; else note "dma empty (no ISA DMA on arm64)"; fi
else bad "dma unreadable"; fi
# ioports: legitimately empty on arm64 (no port-mapped I/O); legacy ports on x86
if p=$(cat "$PROC/ioports" 2>/dev/null); then
    if [ -n "$p" ]; then ok "ioports: $(printf '%s' "$p" | wc -l | tr -d ' ') regions"; else note "ioports empty (no port I/O on arm64)"; fi
else bad "ioports unreadable"; fi
tfile "$PROC/iomem"           "iomem (System RAM map)"
tfile "$PROC/softirqs"        "softirqs (per-CPU softirq table)"
tfile "$PROC/partitions"      "partitions"
tfile "$PROC/diskstats"       "diskstats"
tfile "$PROC/mtab"            "mtab"
tfile "$PROC/mounts"          "mounts"
tfile "$PROC/swaps"           "swaps"
tfile "$PROC/filesystems"     "filesystems"
tfile "$PROC/cmdline"         "cmdline (kernel boot args)"
# bootconfig mirrors the boot-args; legitimately empty on a Mac with none set
if out=$(cat "$PROC/bootconfig" 2>/dev/null); then
    if [ -n "$out" ]; then ok "bootconfig: $(printf '%s' "$out" | head -1 | cut -c1-48)"
    else note "bootconfig empty (no boot-args set)"; fi
else bad "bootconfig unreadable"; fi

hdr "Root: daemon-backed listings"
for n in extensions modules devices allocinfo apm fb interrupts; do
    if out=$(cat "$PROC/$n" 2>/dev/null) && [ -n "$out" ]; then ok "$n: $(printf '%s' "$out" | wc -l | tr -d ' ') lines"
    elif [ "$daemon" = no ]; then note "$n empty (procfsd not running)"; else bad "$n empty"; fi
done

hdr "Root: kernel internals"
# kcore is a size-0 dynamic node (0 in getattr), so check its content: the read
# must yield the ELF magic (0x7f 'E' 'L' 'F') of the ELF64 core header.
if [ "$(head -c 4 "$PROC/kcore" 2>/dev/null | od -An -tx1 | tr -d ' \n')" = "7f454c46" ]; then
    ok "kcore (ELF64 core header, ELF magic)"
else bad "kcore (no ELF magic)"; fi
dfile "$PROC/kmsg"             "kmsg (kernel ring buffer)"
dfile "$PROC/ksyms"            "ksyms (exported symbols)"
dfile "$PROC/kallsyms"         "kallsyms (all symbols)"
dfile "$PROC/slabinfo"         "slabinfo (zone caches)"
dfile "$PROC/vmallocinfo"      "vmallocinfo (kernel VM allocations)"
dfile "$PROC/pagetypeinfo"     "pagetypeinfo (buddy blocks by type)"
dfile "$PROC/misc"             "misc (misc char devices)"
# last_kmsg: present only after a panic report has been saved
if out=$(cat "$PROC/last_kmsg" 2>/dev/null) && [ -n "$out" ]; then
    ok "last_kmsg: $(printf '%s' "$out" | wc -l | tr -d ' ') lines"
else note "last_kmsg empty (no saved panic report)"; fi
# locks: in-kernel (vnode walk); may be empty when no byte-range locks are held
if cat "$PROC/locks" >/dev/null 2>&1; then
    out=$(cat "$PROC/locks" 2>/dev/null)
    if [ -n "$out" ]; then ok "locks: $(printf '%s' "$out" | wc -l | tr -d ' ') held"
    else note "locks empty (none held)"; fi
else bad "locks unreadable"; fi

hdr "Root: /proc/scsi + /proc/sysvipc"
tdir  "$PROC/scsi"        "scsi dir"
dfile "$PROC/scsi/scsi"   "scsi/scsi (attached devices)"
tdir  "$PROC/sysvipc"     "sysvipc dir"
for n in msg sem shm; do dfile "$PROC/sysvipc/$n" "sysvipc/$n"; done

hdr "Root: legacy nodes (no macOS analog)"
efile "$PROC/isapnp"           "isapnp (ISA PnP)"
# ide is a directory that is empty on macOS (no IDE/ATA subsystem)
if ls "$PROC/ide" >/dev/null 2>&1; then note "ide dir present (empty; no IDE on macOS)"; else bad "ide dir unreadable"; fi

hdr "Root: /proc/bus (PCI)"
tdir "$PROC/bus"     "bus dir"
tdir "$PROC/bus/pci" "bus/pci dir"
if out=$(cat "$PROC/bus/pci/devices" 2>/dev/null) && [ -n "$out" ]; then ok "bus/pci/devices: $(printf '%s' "$out" | wc -l | tr -d ' ') devices"
elif [ "$daemon" = no ]; then note "bus/pci/devices empty (procfsd not running)"; else note "bus/pci/devices empty"; fi

hdr "Root: /proc/tty (drivers)"
tdir  "$PROC/tty"          "tty dir"
tfile "$PROC/tty/ldiscs"   "tty/ldiscs"
if out=$(cat "$PROC/tty/drivers" 2>/dev/null) && [ -n "$out" ]; then ok "tty/drivers: $(printf '%s' "$out" | wc -l | tr -d ' ') drivers"
elif [ "$daemon" = no ]; then note "tty/drivers empty (procfsd not running)"; else note "tty/drivers empty"; fi

hdr "Root: /proc/irq (SMP affinity)"
tdir  "$PROC/irq"                            "irq dir"
tfile "$PROC/irq/default_smp_affinity"       "irq/default_smp_affinity"
tfile "$PROC/irq/default_smp_affinity_list"  "irq/default_smp_affinity_list"

hdr "Root: /proc/fs (NFS exports)"
tdir "$PROC/fs"     "fs dir"
tdir "$PROC/fs/nfs" "fs/nfs dir"
# exports: empty when no /etc/exports (no NFS server) or no daemon
if e=$(cat "$PROC/fs/nfs/exports" 2>/dev/null); then
    if [ -n "$e" ]; then ok "fs/nfs/exports: $(printf '%s' "$e" | wc -l | tr -d ' ') lines"
    else note "fs/nfs/exports empty (no /etc/exports or no daemon)"; fi
else bad "fs/nfs/exports unreadable"; fi

hdr "Root: /proc/net (interfaces, sockets, routing, stats)"
tdir  "$PROC/net"        "net dir"
# net/dev is in-kernel (ifnet KPIs), so it is populated with or without procfsd.
tfile "$PROC/net/dev"    "net/dev (interface stats)"
# Socket / routing / ARP tables: served by procfsd; each prints its Linux header
# (a non-empty file) even when there are no entries.
for n in tcp tcp6 udp udp6 unix route arp; do
    if out=$(cat "$PROC/net/$n" 2>/dev/null) && [ -n "$out" ]; then
        ok "net/$n: $(printf '%s' "$out" | wc -l | tr -d ' ') lines"
    elif [ "$daemon" = no ]; then note "net/$n empty (procfsd not running)"
    else bad "net/$n empty"; fi
done
# netstat/snmp: two-line SNMP-style sections; validate the section labels.
if out=$(cat "$PROC/net/netstat" 2>/dev/null) && printf '%s' "$out" | grep -q "TcpExt:"; then
    ok "net/netstat (TcpExt/IpExt)"
elif [ "$daemon" = no ]; then note "net/netstat empty (procfsd not running)"
else bad "net/netstat missing TcpExt"; fi
if out=$(cat "$PROC/net/snmp" 2>/dev/null) && printf '%s' "$out" | grep -q "^Ip:"; then
    ok "net/snmp (Ip/Icmp/Tcp/Udp)"
elif [ "$daemon" = no ]; then note "net/snmp empty (procfsd not running)"
else bad "net/snmp missing Ip:"; fi

hdr "Root: symlinks + dirs"
tlink "$PROC/self"    "self"
tlink "$PROC/curproc" "curproc"
tdir  "$PROC/byname"  "byname"
fl=$(ls "$PROC/byname" 2>/dev/null | head -1); [ -n "$fl" ] && tlink "$PROC/byname/$fl" "byname/$fl"

hdr "Root: /proc/sys sysctl mirror"
tdir  "$PROC/sys"                "sys readdir"
tdir  "$PROC/sys/kern"           "sys/kern readdir"
v=$(cat "$PROC/sys/kern/ostype" 2>/dev/null)
# "Linux" when a kernel version is being spoofed (procfs.linux_version), else Darwin
case "$v" in Darwin|Linux) ok "sys/kern/ostype = $v";; *) bad "sys/kern/ostype = '$v'";; esac
# Linux-version-spoof oid should exist while the kext is loaded
sv=$(sysctl -n procfs.linux_version 2>/dev/null)
[ -n "$sv" ] && ok "procfs.linux_version oid present (= $sv)" || note "procfs.linux_version oid absent (kext not loaded?)"
# /proc/sys/kernel is a Linux-compat lookup alias for the macOS kern.* node
ka=$(cat "$PROC/sys/kernel/ostype" 2>/dev/null)
case "$ka" in Darwin|Linux) ok "sys/kernel/ostype alias = $ka";; *) bad "sys/kernel/ostype alias = '$ka'";; esac

hdr "Per-process: identity (binary int32)"
tdir "$P" "process dir readdir"
v=$(read_i32 "$P/pid");  [ "$v" = "$SELF" ] && ok "pid = $v" || bad "pid = '$v' (want $SELF)"
for n in ppid pgid sid; do v=$(read_i32 "$P/$n"); [ -n "$v" ] && ok "$n = $v" || bad "$n unreadable"; done

hdr "Per-process: text info"
tfile "$P/comm"     "comm"
tfile "$P/cmdline"  "cmdline (argv)"
tfile "$P/stat"     "stat"
tfile "$P/statm"    "statm"
tfile "$P/io"       "io (disk read/write bytes)"
tfile "$P/status"   "status"
tfile "$P/limit"    "limit"
# environ may legitimately be empty for some procs; accept readable
if cat "$P/environ" >/dev/null 2>&1; then ok "environ (readable)"; else bad "environ ($(cat "$P/environ" 2>&1 >/dev/null))"; fi

hdr "Per-process: symlinks (exe/cwd/root)"
tlink "$P/exe"  "exe"
tlink "$P/cwd"  "cwd"
tlink "$P/root" "root"

hdr "Per-process: fd / threads / task directories"
tdir "$P/fd"      "fd readdir"
tdir "$P/threads" "threads readdir"
tdir "$P/task"    "task readdir"

hdr "Per-process: virtual memory (map/maps/smaps/mem)"
tfile "$P/map"   "map (NetBSD)"
tfile "$P/maps"  "maps (Linux)"
tfile "$P/smaps" "smaps"
tfile "$P/smaps_rollup" "smaps_rollup"
tfile "$P/numa_maps" "numa_maps"
# mem: read semantics use the virtual address as offset. Read 16 bytes from the
# first mapped region reported by maps.
addr=$(head -1 "$P/maps" 2>/dev/null | cut -d- -f1)
if [ -n "$addr" ] && dd if="$P/mem" bs=1 count=16 skip=$((16#$addr)) >/dev/null 2>&1; then
    ok "mem (read 16B @ 0x$addr)"
else
    bad "mem (read at 0x$addr failed)"
fi

hdr "Per-process: registers / auxv / taskinfo"
# taskinfo has a fixed struct size; auxv is a size-0 dynamic node (report 0 in
# getattr) so it must be checked by content, not by [ -s ].
bfile "$P/taskinfo" "taskinfo"
a=$(cat "$P/auxv" 2>/dev/null | tr '\0' '\n' | grep -c .)
if [ "${a:-0}" -gt 0 ]; then ok "auxv ($a apple entries)"; else bad "auxv empty (should carry apple[] entries)"; fi
# regs/fpregs need procfsd AND task_for_pid, which is denied to root for many
# processes (SIP/AMFI) - an empty result there is expected, not a failure.
for n in regs fpregs; do
    if [ -s "$P/$n" ] && cat "$P/$n" >/dev/null 2>&1; then ok "$n ($(wc -c < "$P/$n" | tr -d ' ') bytes)"
    elif [ "$daemon" = no ]; then note "$n empty (procfsd not running)"
    else note "$n empty (task_for_pid denied for this process?)"; fi
done

hdr "Per-process: scheduling / state (cpu/wchan/stack/pagemap/clear_refs)"
# cpu: per-task CPU accounting (libkprocfs); needs procfsd for some fields
dfile "$P/cpu" "cpu (per-task CPU stat)"
# wchan: kernel symbol the task is blocked in, or "0" when on-CPU
if w=$(cat "$P/wchan" 2>/dev/null); then ok "wchan = ${w:-<empty>}"; else bad "wchan unreadable"; fi
# stack: continuation frame(s); empty for a running (on-CPU) task, as on Linux
if cat "$P/stack" >/dev/null 2>&1; then
    s=$(cat "$P/stack" 2>/dev/null)
    if [ -n "$s" ]; then ok "stack: $(printf '%s' "$s" | head -1)"; else note "stack empty (task on-CPU)"; fi
else bad "stack unreadable"; fi
# pagemap: one 8-byte entry per virtual page, indexed by vaddr/pagesize (seek
# like mem). Reuses $addr (first mapped region from maps, set above). Needs
# task_for_pid, which is denied for SIP/AMFI/hardened processes.
pgsz=$(getconf PAGE_SIZE 2>/dev/null || echo 16384)
if [ -n "$addr" ] && dd if="$P/pagemap" bs=8 count=1 skip=$(( 16#$addr / pgsz )) >/dev/null 2>&1; then
    ok "pagemap (entry for 0x$addr)"
else note "pagemap read failed (task_for_pid denied for this process?)"; fi
# clear_refs: write-only; accepts the digits 1-4, rejects anything else (no-op
# on macOS, but the parse/permission path is real)
if echo 1 > "$P/clear_refs" 2>/dev/null; then ok "clear_refs: '1' accepted"; else bad "clear_refs write rejected"; fi
if echo 9 > "$P/clear_refs" 2>/dev/null; then bad "clear_refs: '9' accepted (should be EINVAL)"; else ok "clear_refs: '9' rejected (EINVAL)"; fi

hdr "Per-process: tty"
if t=$(cat "$P/tty" 2>/dev/null) && [ -n "$t" ]; then ok "tty = $t"; else note "tty empty (no controlling terminal?)"; fi

hdr "Per-process: note (write -> signal)"
if echo cont > "$P/note" 2>/dev/null; then ok "note: 'cont' accepted (SIGCONT to self)"; else bad "note write rejected"; fi
if echo bogus_note_xyz > "$P/note" 2>/dev/null; then bad "note: bogus note accepted (should be EINVAL)"; else ok "note: bogus note rejected (EINVAL)"; fi

hdr "Per-thread: task/<tid>/*"
tid=$(ls "$P/task" 2>/dev/null | head -1)
if [ -n "$tid" ]; then
    T="$P/task/$tid"
    for n in comm stat status sched; do tfile "$T/$n" "task/$tid/$n"; done
    bfile "$T/info" "task/$tid/info"
    tfile "$T/maps" "task/$tid/maps"
else
    bad "task/<tid>: no thread dir to test"
fi

hdr "Result"
printf "  \033[32m%d passed\033[0m, \033[31m%d failed\033[0m, %d notes\n" "$pass" "$fail" "$warn"
exit $fail
