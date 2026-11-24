#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Boot `faultsurvive` under a debugger that pends SVCall where the worker's kill redirect starts,
# as an SVC whose entry the fault preempted leaves it, and require the kill to drop it: the
# worker's record names the dropped SVCall, no SVCall runs on the dead worker's exit, whose guard
# would refuse that stack as a contained wild PSP, and main runs after the kill.
#
#   check_fault_pendsvc.sh <faultsurvive.elf> <gdb> <nm>

set -u
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_fault_pendsvc.sh <faultsurvive.elf> <gdb> <nm>"
elf="${1:?$_usage}"
gdb="${2:?$_usage}"
nm="${3:?$_usage}"
need_qemu_machine
if ! command -v "$gdb" >/dev/null 2>&1; then
    echo "SKIP: $gdb not found"
    exit 77
fi
scratch_dir
stop="$("$nm" "$elf" | sed -n 's/^[0-9a-f]* T \(arch_fault_redirect_to_exit\)$/\1/p')"
[ -n "$stop" ] || fail "the image has no kill redirect to stop at"
cat > "$TMP/cmds.gdb" <<GDB
set pagination off
set confirm off
file $elf
target remote | sh -c 'cat $TMP/gdb.out & exec cat > $TMP/gdb.in'
break *$stop
continue
set {unsigned int}0xE000ED24 = *(unsigned int*)0xE000ED24 | 0x8000
printf "PENDED 0x%x\n", *(unsigned int*)0xE000ED24
delete
detach
GDB
mkfifo "$TMP/gdb.in" "$TMP/gdb.out" || fail "mkfifo failed in $TMP"
timeout "${QEMU_TIMEOUT:-20}" "$gdb" -nx -batch -x "$TMP/cmds.gdb" > "$TMP/gdb.log" 2>&1 &
QEMU_EXTRA="${QEMU_EXTRA:-} -chardev pipe,id=kosgdb,path=$TMP/gdb -gdb chardev:kosgdb -S"
run_image "$elf"
wait
pended="$(sed -n 's/^PENDED 0x\([0-9a-fA-F]*\)$/\1/p' "$TMP/gdb.log")"
[ -n "$pended" ] || fail "the debugger never reached the kill redirect:
$(tail -n 8 "$TMP/gdb.log")"
[ $(( 0x$pended & 0x8000 )) -ne 0 ] || fail "SHCSR=0x$pended: the emulator did not pend SVCall"

line_of() { printf '%s\n' "$OUT" | grep -nE "$1" | head -n1 | cut -d: -f1; }
killed="$(line_of "$(thread_fault_re faulter)")"
[ -n "$killed" ] || cfail killed "the worker was not killed"
record="$(printf '%s\n' "$OUT" | KOS_KP_BANNER="$(thread_fault_re faulter)" awk '
    on && !/^  / { exit }
    on { print }
    index($0, ENVIRON["KOS_KP_BANNER"]) > 0 { on = 1 }')"
if ! printf '%s\n' "$record" | grep -qx "  SVCall dropped"; then
    cfail svc "the kill dropped no pending SVCall"
fi
if has_e "CONTAINED \\(wild"; then
    cfail svc "a wild-stack refusal followed the kill, so the pended SVCall ran on the dead worker's
    exit"
fi
assert_no_panic "the pended SVCall ended the system"
survived="$(line_of "\[fs\] survivor ran after the fault")"
if [ -z "$survived" ] || [ "$survived" -le "$killed" ]; then
    cfail survived "main did not run after the kill"
fi
status_clause "the system exited 0 once main returned" 0
echo "PASS: the kill at line $killed dropped the SVCall the debugger pended, main ran at line $survived"
exit 0
