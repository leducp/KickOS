#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for CONSOLE RECLAIM ON DRIVER DEATH: build the sim with the publishing service
# list, bound its console driver to TWO served messages (-DKICKOS_SIMCON_EXIT_AFTER=2),
# and require the kernel console comes BACK when that driver exits. TWO, not one:
# console_handover_finish probes the route with a zero-length rendezvous before any
# client runs, so EXIT_AFTER=1 would exit the driver during bring-up and the app would
# never see a published console at all.
#
# What it defends: while a userspace driver owns the console, console_emit DROPS every
# kernel write (USER_OWNED). Without the reclaim hook a driver that exits leaves the
# system permanently mute (no panic banner, no fault dump, no kprintf). The hook is
# console_on_driver_death, run by exit_current AFTER cap_teardown so every IRQ cap is
# dropped and every line masked before the device is re-initialised.
#
# The assertion is a PAIR from the SAME kos_print call site:
#   BEFORE the death: absent  (dropped; proves the handover really happened)
#   AFTER  the death: present (the reclaimed polled route carries it)
# Either half alone is passable by a regression. The app also requires -KOS_EPIPE from a
# send, so no timing assumption stands in for proof the driver is gone.
#
# Not proven here: the sim's "device" is host fd 1, which has no register state a dead
# driver could garble, so arch_console_reclaim is legitimately the no-op fallback. This
# witnesses the OWNERSHIP STATE MACHINE, not a per-chip reclaim body; those stay
# silicon-gated on mk64f and xmc4800.
#
# usage: check_sim_drvdeath.sh <kickos-source-dir> <cmake>

set -eu

KICKOS_SRC="$1"
CMAKE="${2:-cmake}"

fail() { echo "FAIL: $1"; exit 1; }
# grep as a predicate, with `set -e` kept out of the way.
has() { printf '%s\n' "$OUT" | grep -q "$1"; }
# grep -c exits 1 on zero matches, which under `set -e` would kill the script before its
# fail message ever prints: red for the right reason, but with no diagnostic.
count_of() { printf '%s\n' "$OUT" | grep -c "$1" || true; }
# First matching line number, empty when absent: two markers written by different threads
# both land on fd 1 unbuffered, so their relative order on the wire is program order.
line_of() { printf '%s\n' "$OUT" | grep -n "$1" | head -1 | cut -d: -f1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "== configuring the sim: publishing service list, driver bounded to 2 messages =="
( cd "$KICKOS_SRC" && "$CMAKE" --preset sim -B "$TMP/build" \
    -DKICKOS_SERVICE_LIST=kickos_services_sim \
    -DKICKOS_SIMCON_EXIT_AFTER=2 >/dev/null ) \
  || fail "configure with kickos_services_sim failed"

echo "== building drvdeath =="
"$CMAKE" --build "$TMP/build" --target drvdeath >/dev/null \
  || fail "drvdeath build failed"

APP="$TMP/build/user/apps/common/drvdeath/drvdeath"
[ -x "$APP" ] || fail "drvdeath binary not produced at $APP"

set +e
OUT="$(timeout "${SIM_TIMEOUT:-30}" "$APP" 2>&1)"
RC=$?
set -e
printf '%s\n' "$OUT"

# Checked FIRST: a missing publish also makes the BEFORE marker appear, and reporting the
# marker instead would name a symptom rather than the cause.
has '\[simcon\] driver up (host fd 1)' \
  || fail "the console driver never reached the wire (service bring-up failed?)"
has '\[drvdeath\] published route live' \
  || fail "the app's marker never took the published endpoint route"

# The anti-vacuity half.
if has '\[drvdeath\] kernel console BEFORE death'; then
    fail "the kernel console was STILL live before the death: no handover, this gate proved nothing"
fi

if has '\[drvdeath\] ERROR: driver still alive'; then
    fail "the driver outlived its bounded serve, so the reclaim was never reached"
fi

has '\[simcon\] driver exiting (bounded serve)' \
  || fail "the driver never announced its exit (bounded-serve knob not applied?)"

# The positive half: the same call site, now carried by the reclaimed polled route.
COUNT="$(count_of '\[drvdeath\] kernel console AFTER death (reclaimed)')"
[ "$COUNT" -ne 0 ] \
  || fail "the console stayed DARK after the driver died: reclaim-on-death is not working"
[ "$COUNT" -eq 1 ] \
  || fail "the post-reclaim marker appeared $COUNT times (double-routed?)"

[ "$RC" -eq 0 ] || fail "expected a clean exit 0, got $RC"

# ---------------------------------------------------------------------------------
# Case 2: the driver dies BEFORE it ever receives, i.e. bring-up fails. Three things
# must hold:
#   - the probe notices (a rendezvous on a receiver-less endpoint is -KOS_EPIPE),
#   - the service can REPORT it, because the death gave the console back, and
#   - boot fails LOUDLY: init returns nonzero, so no app runs on a dark console.
# Without the probe, the service returns 0 and the app runs against a console
# nothing is serving.
echo "== case 2: the driver dies during bring-up =="
( cd "$KICKOS_SRC" && "$CMAKE" --preset sim -B "$TMP/build2" \
    -DKICKOS_SERVICE_LIST=kickos_services_sim \
    -DKICKOS_SIMCON_DIE_AT_BRINGUP=1 >/dev/null ) \
  || fail "case 2: configure failed"
"$CMAKE" --build "$TMP/build2" --target drvdeath >/dev/null \
  || fail "case 2: drvdeath build failed"

APP2="$TMP/build2/user/apps/common/drvdeath/drvdeath"
[ -x "$APP2" ] || fail "case 2: drvdeath binary not produced at $APP2"

set +e
OUT="$(timeout "${SIM_TIMEOUT:-30}" "$APP2" 2>&1)"
RC=$?
set -e
printf '%s\n' "$OUT"

has '\[simcon\] driver dying during bring-up' \
  || fail "case 2: the driver never reached its bring-up death"
has '\[simcon\] ERROR: driver died during bring-up' \
  || fail "case 2: the failed handover was NOT reported -- either the probe missed the dead driver, or the console never came back to report on"
# The app must not have run at all: a nonzero service-list result aborts init before it.
if has '\[drvdeath\]'; then
    fail "case 2: the app ran anyway, on a console with no driver -- the failure was not loud"
fi
[ "$RC" -ne 0 ] || fail "case 2: expected a nonzero exit from a failed bring-up, got 0"

# ---------------------------------------------------------------------------------
# Case 3: a TWO-THREAD driver, which is the shape every silicon console driver has. A
# service thread receives; a second thread holds the register window and parks in
# kos_irq_wait. Cases 1 and 2 above cannot see this at all: their driver is one thread
# with no window, so the reclaim's device precondition never even executes.
#
# The three markers are ONE assertion, not three:
#   BEFORE the service thread dies : absent  (the handover really happened)
#   AFTER it dies, window HELD     : absent  (the reclaim WAITED for the register owner)
#   AFTER the window is released   : present (and only cancelling the holder got us here)
# The middle marker is the whole point. Keying the reclaim on the endpoint's last
# RECEIVER makes it appear (the console comes back while a live thread still owns the
# UART), so deleting the dev_window_free guard in kernel/init/console.cc turns this red.
# It is also self-anti-vacuous: if the second thread never took the window, the middle
# marker appears too.
#
# The third marker is what proves the KILL primitive: without it the window thread parks
# in kos_irq_wait forever, nothing releases the window, and the console never returns.
echo "== case 3: a two-thread driver, the register window outliving the receiver =="
( cd "$KICKOS_SRC" && "$CMAKE" --preset sim -B "$TMP/build3" \
    -DKICKOS_SERVICE_LIST=kickos_services_sim \
    -DKICKOS_SIMCON_EXIT_AFTER=2 \
    -DKICKOS_SIMCON_WINDOW_THREAD=1 >/dev/null ) \
  || fail "case 3: configure failed"
"$CMAKE" --build "$TMP/build3" --target drvdeath >/dev/null \
  || fail "case 3: drvdeath build failed"

APP3="$TMP/build3/user/apps/common/drvdeath/drvdeath"
[ -x "$APP3" ] || fail "case 3: drvdeath binary not produced at $APP3"

set +e
OUT="$(timeout "${SIM_TIMEOUT:-30}" "$APP3" 2>&1)"
RC=$?
set -e
printf '%s\n' "$OUT"

# Premise first, so a broken bring-up is never reported as a broken reclaim.
has '\[simcon\] window thread holding the console registers' \
  || fail "case 3: the window thread never took the DEV window (no candidate base mapped?)"
has '\[simcon\] driver up (host fd 1)' \
  || fail "case 3: the console driver never reached the wire"
has '\[drvdeath\] published route live' \
  || fail "case 3: the app's marker never took the published endpoint route"
if has '\[drvdeath\] kernel console BEFORE death'; then
    fail "case 3: the kernel console was STILL live before the death: no handover"
fi
has '\[simcon\] driver exiting (bounded serve)' \
  || fail "case 3: the service thread never announced its exit"
if has '\[drvdeath\] ERROR'; then
    fail "case 3: the app reported an error (see its line above)"
fi

# THE new assertion. Absent == the reclaim deferred to the register owner.
if has '\[drvdeath\] kernel console AFTER death, window HELD'; then
    fail "case 3: the console came BACK while a live thread still held the UART register window -- reclaim is keyed on the last receiver, not on the device"
fi

# The kill primitive did its job: cancelled, exited, window released.
has '\[simcon\] window thread cancelled, releasing the registers' \
  || fail "case 3: the window thread was never cancelled out of kos_irq_wait"
has '\[drvdeath\] kill gate: EBADF/EPERM refused, spawner accepted' \
  || fail "case 3: the thread_kill gate matrix did not pass"

COUNT="$(count_of '\[drvdeath\] kernel console AFTER death (reclaimed)')"
[ "$COUNT" -ne 0 ] \
  || fail "case 3: the console stayed DARK after the window was released: the deferred reclaim never ran"
[ "$COUNT" -eq 1 ] \
  || fail "case 3: the post-reclaim marker appeared $COUNT times (double-routed?)"

[ "$RC" -eq 0 ] || fail "case 3: expected a clean exit 0, got $RC"

# ---------------------------------------------------------------------------------
# Case 4: the READY TIMEOUT. Every silicon console driver waits for its IRQ thread's
# bring-up with a bounded loop, and no test on any board had ever executed that loop's
# expiry. KICKOS_SIMCON_IRQ_WEDGE gives the sim an IRQ thread that takes the register
# window and never sets `ready`, with a bring-up ordered like the silicon drivers
# (publish, claim, IRQ thread, wait, service thread).
#
# The ORDER is what this defends, on three counts:
#   - the wait precedes the SERVICE spawn: root is still E's only receiver holder, so
#     closing E takes recv_holders to 0 and notes the console dead. rpusb waited after
#     both spawns (fixed in 3a77013), where the service thread holds a WAIT cap on E and
#     that close reclaims nothing.
#   - kos_handle_close(ep) precedes kos_thread_kill: the note must be set before the
#     cancelled thread's exit re-runs the reclaim.
#   - the kill is not optional: the note alone leaves the console USER_OWNED because the
#     wedged thread still holds the window (dev_window_free in kernel/init/console.cc).
#
# The wedge parks IN kos_irq_wait, the one shape thread_kill can cancel; wedged before
# that first wait it would be marked and not die, the window would never be released and
# the tag would be legitimately lost (documented in xmcuartirq.cc).
#
# The assertion is a PAIR from the SAME kos::print mechanism, as in case 1:
#   after the publish, before the timeout : absent  (USER_OWNED drops it)
#   the timeout tag itself               : present (only a reclaim can carry it)
# Absent-then-present is what proves a reclaim happened in between. Either half alone
# passes on a build where the publish never took, and then nothing is being tested.
echo "== case 4: the IRQ thread never reaches its loop, so root's ready-wait expires =="
( cd "$KICKOS_SRC" && "$CMAKE" --preset sim -B "$TMP/build4" \
    -DKICKOS_SERVICE_LIST=kickos_services_sim \
    -DKICKOS_SIMCON_IRQ_WEDGE=1 >/dev/null ) \
  || fail "case 4: configure failed"
"$CMAKE" --build "$TMP/build4" --target drvdeath >/dev/null \
  || fail "case 4: drvdeath build failed"

APP4="$TMP/build4/user/apps/common/drvdeath/drvdeath"
[ -x "$APP4" ] || fail "case 4: drvdeath binary not produced at $APP4"

set +e
OUT="$(timeout "${SIM_TIMEOUT:-30}" "$APP4" 2>&1)"
RC=$?
set -e
printf '%s\n' "$OUT"

# Premise: the injection staged. Parked means the thread holds the window AND is sitting in
# kos_irq_wait, so the cancel below is possible and the timeout is not a spawn failure.
has '\[simcon\] wedge irq thread parked, ready never set' \
  || fail "case 4: the wedge irq thread never parked (no DEV window, or no line?)"

# THE ANTI-VACUITY HALF. This kernel-path write runs after the publish and before the
# timeout, so it must be DROPPED. On the wire it means the console was never USER_OWNED,
# and then the tag below reaches the wire with no reclaim involved and proves nothing.
if has '\[simcon\] wedge: post-publish kernel write'; then
    fail "case 4: a post-publish kernel write REACHED the wire, so the console was never published: every assertion below would be vacuous"
fi

# A panic also reclaims, from any state (kpanic_enter). If one ran, the tag below would be
# carried by that and not by the driver-death path.
if has 'KERNEL PANIC'; then
    fail "case 4: the system panicked, so the reclaim cannot be attributed to the timeout path"
fi

# The wait ran BEFORE the service spawn: no service thread was ever created.
if has '\[simcon\] driver up (host fd 1)'; then
    fail "case 4: the service thread was spawned before the ready-wait expired, so root is not E's only receiver holder, closing E reclaims nothing and the timeout is unreportable (the rpusb bug)"
fi

# THE POSITIVE HALF: the diagnostic reached the wire, which only a reclaim allows.
COUNT="$(count_of '\[simcon\] ERROR: IRQ thread never reached its loop')"
[ "$COUNT" -ne 0 ] \
  || fail "case 4: the timeout tag never reached the wire: the console was not given back, so the failure is silent"
[ "$COUNT" -eq 1 ] \
  || fail "case 4: the timeout tag appeared $COUNT times (double-routed?)"

# The cancel is observable, and it must PRECEDE the tag: releasing the window is what lets
# the already-noted death reclaim the console.
has '\[simcon\] wedge irq thread cancelled, releasing the registers' \
  || fail "case 4: the wedged irq thread was never cancelled out of kos_irq_wait, so nothing released the register window"
CANCEL_AT="$(line_of 'wedge irq thread cancelled')"
TAG_AT="$(line_of 'ERROR: IRQ thread never reached its loop')"
{ [ -n "$CANCEL_AT" ] && [ -n "$TAG_AT" ] && [ "$CANCEL_AT" -lt "$TAG_AT" ]; } \
  || fail "case 4: the timeout tag reached the wire BEFORE the window was released, so the reclaim was not what carried it"

# Bounded: root returned. 124 is the outer timeout, i.e. a hang.
[ "$RC" -ne 124 ] \
  || fail "case 4: root never returned from its ready-wait, so the bound did not hold"
[ "$RC" -ne 0 ] \
  || fail "case 4: a console bring-up that timed out still exited 0"

# No app may run on a console nothing is serving.
if has '\[drvdeath\]'; then
    fail "case 4: the app ran anyway, on a dark console"
fi

echo "PASS: the console returns to the kernel on driver death, a failed handover is loud, a two-thread driver's console waits for its register owner, and a ready-timeout reports itself"
