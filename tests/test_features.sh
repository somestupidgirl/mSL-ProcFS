#!/bin/bash
#
# test_features.sh — exercises every procfs feature against a mounted ~/proc.
# Reports PASS/FAIL per check. Does not require the mount to use noprocperms;
# uses the current process and its own PID for permission-sensitive reads.
#
PROC=${PROC:-$HOME/proc}
pass=0; fail=0

ok()   { printf "  \033[32mPASS\033[0m %s\n" "$1"; pass=$((pass+1)); }
bad()  { printf "  \033[31mFAIL\033[0m %s\n" "$1"; fail=$((fail+1)); }
skip() { printf "  \033[33mN/A \033[0m %s\n" "$1"; }   # expected-unavailable, not counted
hdr()  { printf "\n== %s ==\n" "$1"; }

# check_file <path> <description> — passes if readable and non-empty
check_file() {
    local p="$1" d="$2"
    if [ -r "$p" ] && out=$(cat "$p" 2>/dev/null) && [ -n "$out" ]; then
        ok "$d: $(echo "$out" | head -1 | cut -c1-70)"
    else
        bad "$d (path=$p, errno: $(cat "$p" 2>&1 >/dev/null))"
    fi
}

# check_dir <path> <description> — passes if it lists at least one entry
check_dir() {
    local p="$1" d="$2"
    if entries=$(ls "$p" 2>/dev/null) && [ -n "$entries" ]; then
        ok "$d: $(echo "$entries" | wc -l | tr -d ' ') entries"
    else
        bad "$d (path=$p)"
    fi
}

[ -d "$PROC" ] || { echo "$PROC not mounted"; exit 1; }
SELF=$$

hdr "Root readdir + static files"
check_dir  "$PROC"               "root readdir"
check_file "$PROC/cpuinfo"       "cpuinfo"
check_file "$PROC/loadavg"       "loadavg"
check_file "$PROC/partitions"    "partitions"
check_file "$PROC/version"       "version"

hdr "Symlinks"
if t=$(readlink "$PROC/curproc" 2>/dev/null); then ok "curproc -> $t"; else bad "curproc readlink"; fi

hdr "byname directory"
check_dir "$PROC/byname" "byname readdir"
firstlink=$(ls "$PROC/byname" 2>/dev/null | head -1)
if [ -n "$firstlink" ]; then
    if t=$(readlink "$PROC/byname/$firstlink" 2>/dev/null); then ok "byname/$firstlink -> $t"; else bad "byname/$firstlink readlink"; fi
fi

# read a 4-byte little-endian int32 from a procfs file (pid/ppid/pgid/sid are binary)
read_i32() { python3 -c "import struct,sys; print(struct.unpack('<i', open(sys.argv[1],'rb').read(4))[0])" "$1" 2>/dev/null; }

hdr "Per-process files (self pid=$SELF)"
P="$PROC/$SELF"
check_dir  "$P"            "process dir readdir"
# pid/ppid/pgid/sid return a binary int32 by design
v=$(read_i32 "$P/pid");  [ "$v" = "$SELF" ] && ok "pid = $v (matches self)" || bad "pid = $v (expected $SELF)"
v=$(read_i32 "$P/ppid"); [ -n "$v" ] && ok "ppid = $v" || bad "ppid unreadable"
v=$(read_i32 "$P/pgid"); [ -n "$v" ] && ok "pgid = $v" || bad "pgid unreadable"
v=$(read_i32 "$P/sid");  [ -n "$v" ] && ok "sid = $v"  || bad "sid unreadable"
# info and taskinfo are binary structs — just check readable & non-empty
if [ -r "$P/info" ] && [ -s "$P/info" ]; then ok "info ($(wc -c < "$P/info" | tr -d ' ') bytes)"; else bad "info"; fi
if [ -r "$P/taskinfo" ] && [ -s "$P/taskinfo" ]; then ok "taskinfo ($(wc -c < "$P/taskinfo" | tr -d ' ') bytes)"; else bad "taskinfo"; fi

hdr "Known-unavailable on this build (private symbols NULL under PAC)"
# These require private kernel symbols that are stubbed NULL; we verify they
# degrade GRACEFULLY (clean ENOTSUP / empty listing), not that they return data.
err=$(cat "$P/tty" 2>&1 >/dev/null)
case "$err" in
    *"Operation not supported"*) skip "tty: graceful ENOTSUP (no _proc_gettty on ARM64)";;
    "") skip "tty: returned data (private symbols available?)";;
    *) bad "tty: unexpected error: $err";;
esac
if ls "$P/fd" >/dev/null 2>&1; then skip "fd readdir: graceful (lists . .. only; no fd enumeration KPI)"; else bad "fd readdir errored"; fi
if ls "$P/threads" >/dev/null 2>&1; then skip "threads readdir: graceful (lists . .. only; no thread enumeration KPI)"; else bad "threads readdir errored"; fi

hdr "Result"
printf "  %d passed, %d failed\n" "$pass" "$fail"
exit $fail
