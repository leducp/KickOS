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
# Why this needs its own build: KICKOS_SERVICE_LIST selects one provider per image, so
# the published posture cannot coexist with the default one in a single tree.
#
# Why it exists: every other panic/fault gate in the fleet runs on an UNBUFFERED console
# (mps2/microbit/virt semihosting); the sim is the only platform that is both buffered
# and hardware-free. Case 2 covers the f302nucleo defect shape: a fault reporter that
# produces no dump.
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

FAULT_STATUS=132 # kfault_terminate -> arch_shutdown(132) on the host

fail() { echo "FAIL: $1"; exit 1; }
# grep as a predicate, with `set -e` kept out of the way.
has() { printf '%s\n' "$OUT" | grep -q "$1"; }
# grep -c exits 1 on zero matches, which under `set -e` would kill the script before its
# fail message ever prints: red for the right reason, but with no diagnostic.
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

# Assertions every case shares: the publish happened, the endpoint route carried the
# app's marker, and the kernel debug console is dark.
common_asserts() {
    # Checked FIRST: a missing publish also strands the driver, and reporting that
    # instead would name a symptom rather than the cause.
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
