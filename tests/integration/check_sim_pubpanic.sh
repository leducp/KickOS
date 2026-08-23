#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for TERMINAL REPORTING on a published console: build the sim with the
# publishing service list (kickos_services_sim, where a userspace driver owns the
# "wire"; see system/init/sim/service_list.cc), then require both terminal reports to
# still reach the wire:
#   pubpanic1  kos_panic  -> "KERNEL PANIC: [pubpanic] banner after handover"
#   pubpanic2  ud2/SIGILL -> "=== SIM FAULT (illegal instruction)", exactly once
#
# Case 2 inverts on a backend with fault isolation: root's illegal instruction kills root
# alone, so the claim becomes survival AND reporting together. See the case-2 block.
#
# It needs its own build because KICKOS_SERVICE_LIST selects one provider per image, so the
# published posture cannot coexist with the default one in a single tree.
#
# The sim is the fleet's one platform that is both BUFFERED and hardware-free, so a buffered
# terminal report is witnessed here; every other panic gate runs semihosted and unbuffered.
# Case 2 covers the f302nucleo defect shape: a fault reporter that produces no dump.
#
# The load-bearing anti-vacuity assertion is the NEGATIVE one: the app's kos_print
# witness must be ABSENT. console_emit drops kernel-console writes while the console is
# USER_OWNED, so its absence is what proves the handover really happened. Without it a
# regression that skipped the publish entirely would still pass here.
#
# usage: check_sim_pubpanic.sh <kickos-source-dir> <cmake>

set -eu

KICKOS_SRC="$1"
CMAKE="${2:-cmake}"
# What a user-thread fault DOES is a property of the backend, so the caller passes it in;
# case 2's illegal instruction is executed by root.
OUTCOME="${3:-panic}"

FAULT_STATUS=132 # kfault_terminate -> arch_shutdown(132) on the host

fail() { echo "FAIL: $1"; exit 1; }
# grep as a predicate, with `set -e` kept out of the way.
has() { printf '%s\n' "$OUT" | grep -q "$1"; }
# grep -c exits 1 on zero matches, so without `|| true` set -e kills the script before its
# fail message prints.
count_of() { printf '%s\n' "$OUT" | grep -c "$1" || true; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "== configuring the sim with the publishing service list =="
( cd "$KICKOS_SRC" && "$CMAKE" --preset sim -B "$TMP/build" \
    -DKICKOS_SERVICE_LIST=kickos_services_sim >/dev/null ) \
  || fail "configure with kickos_services_sim failed"

echo "== building pubpanic1 + pubpanic2 =="
"$CMAKE" --build "$TMP/build" --target pubpanic1 pubpanic2 >/dev/null \
  || fail "pubpanic build failed"

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
APP="$TMP/build/user/apps/common/pubpanic/pubpanic1"
[ -x "$APP" ] || fail "pubpanic1 binary not produced at $APP"
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
APP="$TMP/build/user/apps/common/pubpanic/pubpanic2"
[ -x "$APP" ] || fail "pubpanic2 binary not produced at $APP"

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

# thread-kill: the illegal instruction is root's own fault, so it is no longer terminal.
# The claim inverts with it. Two things must hold at once, and each is asserted positively:
# the SYSTEM survives (the process is still alive after the settle, and the driver still
# owns the wire), AND the kill record still reaches that wire.
#
# The record arrives over the DRIVER, not over the kernel chip path, which console_emit
# drops while the console is USER_OWNED (kernel/init/console.cc). kprintf_fault hands it to
# the published endpoint's parked receiver instead (cap_console_deliver), because the
# thread-kill path may not call kpanic_enter: that reclaim is permanent and would take the
# console away from a system that is meant to keep running. See
# design-m4.7.9-fault-isolation.md section 9.5.
#
# Both halves are load-bearing: without the survival assertion a permanent reclaim passes,
# and without the record assertion a swallowed record passes.
LOG="$TMP/case2.log"
"$APP" >"$LOG" 2>&1 &
APID=$!
sleep "${SIM_SETTLE:-3}"
ALIVE=0
if kill -0 "$APID" 2>/dev/null; then
    ALIVE=1
fi
{ kill "$APID"; wait "$APID"; } 2>/dev/null
OUT="$(tr -d '\r' < "$LOG")"
printf '%s\n' "$OUT"
common_asserts "case 2"
[ "$ALIVE" -eq 1 ] \
  || fail "case 2: the image died; root's fault was supposed to kill root alone"
if has 'KERNEL PANIC'; then
    fail "case 2: a user-thread fault reached the panic path"
fi
if has '=== SIM FAULT'; then
    fail "case 2: the fault reporter ran; the thread kill should have claimed this fault"
fi
# The record names the dead thread, so the name is matched too: a banner naming another
# thread means the record was misattributed and not merely routed.
COUNT="$(count_of "THREAD FAULT === thread 'root' killed")"
[ "$COUNT" -ne 0 ] \
  || fail "case 2: the kill record never reached the wire; the published console swallowed it"
[ "$COUNT" -eq 1 ] \
  || fail "case 2: the kill record appeared $COUNT times (routed AND emitted by the kernel?)"

echo "PASS: case 1 reaches the wire; case 2 kills root alone, the driver keeps the console, and the record still reaches the wire"
