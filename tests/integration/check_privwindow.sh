#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# PRIVILEGE WINDOW gate: boot a `privwindow` image under a debugger that stops the thread inside
# a kernel window, pends a switch there, and once the context is saved rewrites its stacked PC to
# the app's payload wherever the frame rests in the thread's own stack, as a task-mate could.
# The payload must never run privileged.
#
#   0  the syscall return between its stack move and its privilege drop. The context is saved
#      on the thread's stack there, so the rewrite MUST happen, and the payload must report
#      that it ran unprivileged.
#   1  the first instruction a contained fault's return lands on: the old relocating stub where
#      the image has one, the exit itself otherwise. The thread must die by its fault without
#      the payload running privileged.
#   2  SVC_Handler's entry on a KOS_SYS_CALL_REG, where PENDSVSET is set as a late interrupt
#      would, so the fastpath refuses before its leaf. The call must answer as the dispatch.
#   3  mode 0's window with nothing rewritten: the context saved demoted on its own stack must
#      resume there unprivileged with the syscall's result.
#
# usage: check_privwindow.sh <privwindow.elf> <mode> <gdb> <nm>

set -u
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_privwindow.sh <privwindow.elf> <mode> <gdb> <nm>"
elf="${1:?$_usage}"
mode="${2:?$_usage}"
gdb="${3:?$_usage}"
nm="${4:?$_usage}"

need_qemu_machine
if ! command -v "$gdb" >/dev/null 2>&1; then
    echo "SKIP: $gdb not found"
    exit 77
fi
scratch_dir

case "$mode" in
    0 | 3) stop=kickos_armv7m_svc_drop ;;
    2) stop=SVC_Handler ;;
    1)
        stop=kickos_thread_fault_exit
        if "$nm" "$elf" | grep -q ' kickos_armv7m_fault_stack_reset$'; then
            stop=kickos_armv7m_fault_stack_reset
        fi
        ;;
    *) fail "unknown mode '$mode'" ;;
esac
"$nm" "$elf" | grep -q " $stop\$" || fail "mode=$mode: the image has no '$stop' to stop at"

# The frame's own word 0 is the syscall number, so another thread's trap does not stop mode 2.
cond="g_pw_armed == 1"
if [ "$mode" = 2 ]; then
    cond="g_pw_armed == 2 && *(unsigned int*)\$psp == 56"
fi
rewrite="set {unsigned int}(\$psp + 24) = ((unsigned int)&pw_payload) & ~1"
said="REWROTE"
at_window=""
if [ "$mode" = 3 ]; then
    rewrite=""
    said="SAVED"
    at_window=" && *(unsigned int*)(\$psp + 24) == (((unsigned int)&kickos_armv7m_svc_drop) & ~1)"
fi
{
    cat <<GDB
set pagination off
set confirm off
file $elf
target remote | sh -c 'cat $TMP/gdb.out & exec cat > $TMP/gdb.in'
break $stop if $cond
continue
delete
printf "STOPPED at %s\n", "$stop"
set {unsigned int}&g_arch_next = *(unsigned int*)&g_arch_current
set {unsigned int}0xE000ED04 = 0x10000000
GDB
    if [ "$mode" != 2 ]; then
        cat <<GDB
break kickos_arch_mpu_commit
continue
delete
set \$c = *(unsigned int*)&g_arch_current
set \$lo = *(unsigned int*)(\$c + 12)
set \$hi = *(unsigned int*)(\$c + 16)
if \$psp >= \$lo && \$psp < \$hi$at_window
  $rewrite
  printf "$said frame at 0x%x\n", \$psp
else
  printf "KERNEL FRAME at 0x%x\n", \$psp
end
GDB
    fi
    echo detach
} > "$TMP/cmds.gdb"

mkfifo "$TMP/gdb.in" "$TMP/gdb.out" || fail "mkfifo failed in $TMP"
timeout "${QEMU_TIMEOUT:-20}" "$gdb" -nx -batch -x "$TMP/cmds.gdb" > "$TMP/gdb.log" 2>&1 &
QEMU_EXTRA="${QEMU_EXTRA:-} -chardev pipe,id=kosgdb,path=$TMP/gdb -gdb chardev:kosgdb -S"
run_image "$elf"
wait

if ! grep -q "^STOPPED at $stop" "$TMP/gdb.log"; then
    fail "mode=$mode: the debugger never stopped at $stop:
$(tail -n 8 "$TMP/gdb.log")"
fi
if has "\[privwindow\] ERROR"; then
    fail "mode=$mode: the app reported a failed arm"
fi
if has "\[privwindow\] payload: privileged"; then
    fail "mode=$mode: a context resumed PRIVILEGED at a PC rewritten in its own stack"
fi
assert_no_panic "mode=$mode: the window ended the system"

if [ "$mode" = 2 ]; then
    has "\[privwindow\] call_reg: answered by the dispatch" \
        || fail "mode=2: the register call entered with PendSV pended never answered"
    echo "PASS: mode=2 answered the register call refused for a pended PendSV"
    exit 0
fi
if [ "$mode" = 3 ]; then
    grep -q "^SAVED" "$TMP/gdb.log" \
        || fail "mode=3: the syscall return's context was not saved on its own stack at the drop, so nothing here was witnessed:
$(tail -n 4 "$TMP/gdb.log")"
    has "\[privwindow\] resumed: unprivileged, with the syscall's result" \
        || fail "mode=3: the demoted syscall return never finished"
    echo "PASS: mode=3 resumed the demoted syscall return unprivileged with its result"
    exit 0
fi
if [ "$mode" = 0 ]; then
    grep -q "^REWROTE" "$TMP/gdb.log" \
        || fail "mode=0: the syscall return's context was not on its own stack, so nothing here was witnessed:
$(tail -n 4 "$TMP/gdb.log")"
    has "\[privwindow\] payload: unprivileged" \
        || fail "mode=0: the rewritten context never reached the payload"
    echo "PASS: mode=0 resumed the rewritten syscall return unprivileged"
    exit 0
fi
has "$(thread_fault_re main)" || fail "mode=1: main was not killed by its fault"
if grep -q "^REWROTE" "$TMP/gdb.log"; then
    has "\[privwindow\] payload: unprivileged" \
        || fail "mode=1: the rewritten exit never reached the payload"
    echo "PASS: mode=1 resumed the rewritten fault exit unprivileged"
    exit 0
fi
grep -q "^KERNEL FRAME" "$TMP/gdb.log" || fail "mode=1: the debugger read no frame"
echo "PASS: mode=1 rested the fault exit's frame in kernel memory"
exit 0
