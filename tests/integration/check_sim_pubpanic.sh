#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for TERMINAL REPORTING on a published console: run the two pubpanic images, whose
# composition names the packaged simcon as stdout (a userspace driver owns the "wire"; see
# system/driver/sim/simcon/simcon.cc), and require both terminal reports to still reach the
# wire:
#   pubpanic1  kos_panic  -> "KERNEL PANIC: [pubpanic] banner after handover"
#   pubpanic2  ud2/SIGILL -> "=== SIM FAULT (illegal instruction)", exactly once
#
# Case 2 inverts on a backend with fault isolation: main's illegal instruction ends main's task,
# which ends the system through the init. See the case-2 block.
#
# The sim is the fleet's one platform that is both BUFFERED and hardware-free, so a buffered
# terminal report is witnessed here; every other panic gate runs semihosted and unbuffered.
# Case 2 covers the f302nucleo defect shape: a fault reporter that produces no dump.
#
# The load-bearing anti-vacuity assertion is the NEGATIVE one: the app's raw kernel console
# witness must be ABSENT. console_emit drops kernel-console writes while the console is
# USER_OWNED, so its absence is what proves the handover really happened. Without it a
# regression that skipped the publish entirely would still pass here.
#
# usage: check_sim_pubpanic.sh <pubpanic1> <pubpanic2> [panic|thread-kill]

set -eu
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_sim_pubpanic.sh <pubpanic1> <pubpanic2> [panic|thread-kill]"
APP1="${1:?$_usage}"
APP2="${2:?$_usage}"
# What a user-thread fault DOES is a property of the backend, so the caller passes it in;
# case 2's illegal instruction is executed by main.
OUTCOME="${3:-panic}"

FAULT_STATUS=132 # kfault_terminate -> arch_shutdown(132) on the host
# KOS_EXIT_FAULT, the status the init ends the system with once main's task faulted. Restated
# rather than computed, so the gate asserts the number the runtime reports.
# A host SIGSEGV of the sim also exits 139; the kill record tells the two apart.
TASK_FAULT_STATUS=139

# grep -c exits 1 on zero matches, so without `|| true` set -e kills the script before its
# fail message prints.
count_of() { printf '%s\n' "$OUT" | grep -c "$1" || true; }

common_asserts() {
    # First: a missing publish also strands the driver, so reporting that would name the
    # symptom and not the cause.
    if has '\[pubpanic\] kernel-console witness'; then
        fail "$1: the kernel debug console is STILL live: no handover, this gate proved nothing"
    fi
    has '\[simcon\] driver up (host fd 1)' \
      || fail "$1: the console driver never reached the wire (service bring-up failed?)"
    has '\[pubpanic\] published route live' \
      || fail "$1: the app's marker never took the published endpoint route"
    if has '\[pubpanic\] ERROR'; then
        fail "$1: the terminal path returned instead of ending the system"
    fi
}

echo "== case 1: kos_panic on a published console =="
APP="$APP1"
[ -x "$APP" ] || fail "no pubpanic1 image at $APP"
set +e
OUT="$(timeout "${SIM_TIMEOUT:-30}" "$APP" 2>&1)"
RC=$?
set -e
printf '%s\n' "$OUT"
common_asserts "case 1"
COUNT="$(count_of 'KERNEL PANIC: \[pubpanic\] banner after handover')"
[ "$COUNT" -ne 0 ] \
  || fail "case 1: the panic banner never reached the wire (reclaim/polled route lost it)"
[ "$COUNT" -eq 1 ] \
  || fail "case 1: the panic banner appeared $COUNT times (ring re-pushed?)"
[ "$RC" -eq "$FAULT_STATUS" ] \
  || fail "case 1: expected exit $FAULT_STATUS (kfault_terminate), got $RC"

echo "== case 2: illegal instruction on a published console =="
APP="$APP2"
[ -x "$APP" ] || fail "no pubpanic2 image at $APP"

if [ "$OUTCOME" = panic ]; then
    set +e
    OUT="$(timeout "${SIM_TIMEOUT:-30}" "$APP" 2>&1)"
    RC=$?
    set -e
    printf '%s\n' "$OUT"
    common_asserts "case 2"
    COUNT="$(count_of '=== SIM FAULT (illegal instruction)')"
    [ "$COUNT" -ne 0 ] \
      || fail "case 2: the fault dump never reached the wire (the f302nucleo defect shape)"
    [ "$COUNT" -eq 1 ] \
      || fail "case 2: the fault dump appeared $COUNT times (ring re-pushed?)"
    [ "$RC" -eq "$FAULT_STATUS" ] \
      || fail "case 2: expected exit $FAULT_STATUS (kfault_terminate), got $RC"
    echo "PASS: both terminal reports reach the wire over a published userspace console"
    exit 0
fi

# thread-kill: the illegal instruction is main's own fault, so it is no longer terminal for the
# kernel. It ends main's task, and the task the composition `ends` on ending ends the system
# through the init with KOS_EXIT_FAULT. Both halves are asserted positively: the kill record
# reaches the wire over the DRIVER, and the end is the init's shutdown, never the panic path.
#
# The record arrives over the driver, not over the kernel chip path, which console_emit drops
# while the console is USER_OWNED (kernel/init/console.cc). The record is held whole for the
# published endpoint's driver instead (krecord_end, cap_console_deliver), because the thread-kill
# path may not call kpanic_enter. A status of 139 rather than 132 is what separates the init's
# ending from kfault_terminate.
set +e
OUT="$(timeout "${SIM_TIMEOUT:-30}" "$APP" 2>&1)"
RC=$?
set -e
printf '%s\n' "$OUT"
common_asserts "case 2"
if has 'KERNEL PANIC'; then
    fail "case 2: a user-thread fault reached the panic path"
fi
if has '=== SIM FAULT'; then
    fail "case 2: the fault reporter ran; the thread kill should have claimed this fault"
fi
# The record names the dead thread, so the name is matched too: a banner naming another
# thread means the record was misattributed and not merely routed.
COUNT="$(count_of "THREAD FAULT === thread 'main' killed")"
[ "$COUNT" -ne 0 ] \
  || fail "case 2: the kill record never reached the wire; the published console swallowed it"
[ "$COUNT" -eq 1 ] \
  || fail "case 2: the kill record appeared $COUNT times (routed AND emitted by the kernel?)"
[ "$RC" -ne 124 ] \
  || fail "case 2: the system never ended: the init did not act on main's fault"
[ "$RC" -eq "$TASK_FAULT_STATUS" ] \
  || fail "case 2: expected exit $TASK_FAULT_STATUS (KOS_EXIT_FAULT through the init), got $RC"

echo "PASS: case 1 reaches the wire; case 2 ends main's task, its record reaches the wire over the driver, and the init ends the system with KOS_EXIT_FAULT"
