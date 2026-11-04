#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for a packaged driver's STARTUP BARRIER: build uartloop, whose composition names the
# packaged simuart with one restart, in two trees of its own (KICKOS_SIMUART_READY).
#
#   late   the IRQ thread sets its readiness latch 50 ms after the service thread waits on it.
#          Passes on the publish waking that parked waiter, then the loopback.
#   wedge  the first instance's IRQ thread never sets its latch. Its service thread's wait
#          expires and its trap ends the whole task, its entry included; the init restarts it,
#          which takes the line and the declaration back, and the second instance serves.
#          Passes on the wedge, one fault, one death, and the loopback.
#
# usage: check_sim_drvready.sh <kickos-source-dir> <cmake>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

KICKOS_SRC="$1"
CMAKE="${2:-cmake}"

count_of() { printf '%s\n' "$OUT" | grep -c "$1" || true; }
PASS_LINE='\[uartloop\] PASS (loopback in order; sustained output past a full ring)'
DEATH_LINE='init: `simuart` is dead and released'

scratch_dir

run_case() { # <knob>
    ( cd "$KICKOS_SRC" && "$CMAKE" --preset sim -B "$TMP/$1" -DKICKOS_SIMUART_READY="$1" >/dev/null ) \
      || fail "$1: configure failed"
    "$CMAKE" --build "$TMP/$1" --target uartloop >/dev/null || fail "$1: uartloop build failed"
    APP="$TMP/$1/user/apps/common/uartloop/uartloop"
    [ -x "$APP" ] || fail "$1: uartloop binary not produced at $APP"
    set +e
    OUT="$(timeout "${SIM_TIMEOUT:-30}" "$APP" 2>&1)"
    RC=$?
    set -e
    printf '%s\n' "$OUT"
}

echo "== late: the receiver waits on the latch before its IRQ thread sets it =="
run_case late
has '\[simuart\] ready woke its parked receiver' \
  || fail "late: the publish woke no parked receiver, so the barrier was never waited on or never woken"
[ "$(count_of "$DEATH_LINE")" -eq 0 ] || fail "late: the driver died: its receiver's wait expired"
has "$PASS_LINE" || fail "late: the loopback did not pass behind the barrier"
[ "$RC" -eq 0 ] || fail "late: expected a clean exit 0, got $RC"

echo "== wedge: the first instance never sets its latch =="
run_case wedge
[ "$(count_of '\[simuart\] wedge: ready never set')" -eq 1 ] \
  || fail "wedge: the first instance did not wedge exactly once"
[ "$(count_of "$(thread_fault_re simuart)")" -eq 1 ] \
  || fail "wedge: the expired wait did not end in exactly one trap"
[ "$(count_of "$DEATH_LINE")" -eq 1 ] \
  || fail "wedge: the trap did not end the whole task exactly once"
[ "$(count_of '\[simuart\] UART service up')" -eq 1 ] \
  || fail "wedge: the restarted instance never came up: its line or its declaration was kept"
has "$PASS_LINE" || fail "wedge: the restarted instance did not serve the loopback"
[ "$RC" -eq 0 ] || fail "wedge: expected a clean exit 0, got $RC"

echo "PASS: a receiver parked on the readiness latch is woken by its publish, and an expired wait ends its task, which restarts"
