#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Boot `callrace` under a debugger that pends the waker's device line at the instruction after
# the SVC trap reads the pending-switch bit, on a register call, as a late interrupt would, and
# require the app's verdict: every call reached the server under the caller's badge.
#
#   check_callrace.sh <callrace.elf> <gdb> <nm>

set -u
. "$(dirname "$0")/../lib/gate.sh"

INJECTIONS=8

_usage="usage: check_callrace.sh <callrace.elf> <gdb> <nm>"
elf="${1:?$_usage}"
gdb="${2:?$_usage}"
nm="${3:?$_usage}"
need_qemu_machine
if ! command -v "$gdb" >/dev/null 2>&1; then
    echo "SKIP: $gdb not found"
    exit 77
fi
scratch_dir
stop="$("$nm" "$elf" | sed -n 's/^[0-9a-f]* T \(kickos_armv[67]m_svc_pend_read\)$/\1/p')"
[ -n "$stop" ] || fail "the image has no SVC pend-read label to stop at"
cat > "$TMP/cmds.gdb" <<GDB
set pagination off
set confirm off
file $elf
target remote | sh -c 'cat $TMP/gdb.out & exec cat > $TMP/gdb.in'
break *$stop if *(unsigned int*)&g_callrace_armed == 1
set \$n = 0
while \$n < $INJECTIONS
  continue
  set \$l = *(int*)&g_callrace_line
  set {unsigned int}(0xE000E200 + 4 * (\$l / 32)) = 1 << (\$l % 32)
  printf "INJECTED %d\n", \$n
  set \$n = \$n + 1
end
delete
detach
GDB
mkfifo "$TMP/gdb.in" "$TMP/gdb.out" || fail "mkfifo failed in $TMP"
timeout "${QEMU_TIMEOUT:-20}" "$gdb" -nx -batch -x "$TMP/cmds.gdb" > "$TMP/gdb.log" 2>&1 &
QEMU_EXTRA="${QEMU_EXTRA:-} -chardev pipe,id=kosgdb,path=$TMP/gdb -gdb chardev:kosgdb -S"
run_image "$elf"
wait
grep -q "^INJECTED 0" "$TMP/gdb.log" || fail "the debugger never pended the line in the trap:
$(tail -n 8 "$TMP/gdb.log")"

if has_e "\[callrace\] (FAIL|ERROR)"; then
    printf '%s\n' "$OUT" | grep -E "\[callrace\] (FAIL|ERROR)"
    fail "callrace reported a call the kernel ran as another thread"
fi
assert_no_panic "callrace panicked before reaching a verdict"
has "^\[callrace\] PASS" || fail "callrace reached no PASS"
echo "PASS: every register call reached its server under the caller's own badge"
exit 0
