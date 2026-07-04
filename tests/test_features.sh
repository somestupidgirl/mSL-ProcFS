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
tfile "$PROC/partitions"      "partitions"
tfile "$PROC/diskstats"       "diskstats"
tfile "$PROC/mtab"            "mtab"
tfile "$PROC/mounts"          "mounts"
tfile "$PROC/swaps"           "swaps"
tfile "$PROC/filesystems"     "filesystems"
tfile "$PROC/cmdline"         "cmdline (kernel boot args)"
tfile "$PROC/net/dev"         "net/dev (interface stats)"
# bootconfig mirrors the boot-args; legitimately empty on a Mac with none set
if out=$(cat "$PROC/bootconfig" 2>/dev/null); then
    if [ -n "$out" ]; then ok "bootconfig: $(printf '%s' "$out" | head -1 | cut -c1-48)"
    else note "bootconfig empty (no boot-args set)"; fi
else bad "bootconfig unreadable"; fi

hdr "Root: daemon-backed listings"
for n in extensions modules devices allocinfo apm; do
    if out=$(cat "$PROC/$n" 2>/dev/null) && [ -n "$out" ]; then ok "$n: $(printf '%s' "$out" | wc -l | tr -d ' ') lines"
    elif [ "$daemon" = no ]; then note "$n empty (procfsd not running)"; else bad "$n empty"; fi
done

hdr "Root: /proc/bus (PCI)"
tdir "$PROC/bus"     "bus dir"
tdir "$PROC/bus/pci" "bus/pci dir"
if out=$(cat "$PROC/bus/pci/devices" 2>/dev/null) && [ -n "$out" ]; then ok "bus/pci/devices: $(printf '%s' "$out" | wc -l | tr -d ' ') devices"
elif [ "$daemon" = no ]; then note "bus/pci/devices empty (procfsd not running)"; else note "bus/pci/devices empty"; fi

hdr "Root: symlinks + dirs"
tlink "$PROC/self"    "self"
tlink "$PROC/curproc" "curproc"
tdir  "$PROC/byname"  "byname"
fl=$(ls "$PROC/byname" 2>/dev/null | head -1); [ -n "$fl" ] && tlink "$PROC/byname/$fl" "byname/$fl"

hdr "Root: /proc/sys sysctl mirror"
tdir  "$PROC/sys"                "sys readdir"
tdir  "$PROC/sys/kern"           "sys/kern readdir"
v=$(cat "$PROC/sys/kern/ostype" 2>/dev/null); [ "$v" = "Darwin" ] && ok "sys/kern/ostype = Darwin" || bad "sys/kern/ostype = '$v'"

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
