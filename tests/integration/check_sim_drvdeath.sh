#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for CONSOLE RECLAIM ON DRIVER DEATH: build drvdeath, whose composition names the
# packaged simcon as stdout with no restart, with the console driver bounded to TWO served
# messages (-DKICKOS_SIMCON_EXIT_AFTER=2), and require the kernel console comes BACK when that
# driver exits, the init reporting the death on it. TWO, not one: console_handover_finish
# probes the route with a zero-length rendezvous before any client runs, so EXIT_AFTER=1
# would exit the driver during its start and the app would never see a published console.
#
# What it defends: while a userspace driver owns the console, console_emit DROPS every
# kernel write (USER_OWNED). Without the reclaim hook a driver that exits leaves the
# system permanently mute (no panic banner, no fault dump, no kprintf). The hook is
# console_on_driver_death, run by exit_current AFTER cap_teardown so every IRQ cap is
# dropped and every line masked before the device is re-initialised.
#
# The assertion is a PAIR from the SAME raw kernel console write:
#   BEFORE the death: absent  (dropped; proves the handover really happened)
#   AFTER  the death: present (the reclaimed polled route carries it)
# Either half alone is passable by a regression. The app waits for the init's report of the
# death on /init/events and then requires -KOS_ECONNREFUSED from a send, so no timing
# assumption stands in for proof the driver is gone.
#
# What this witnesses is the OWNERSHIP STATE MACHINE. The sim's "device" is host fd 1, with
# no register state a dead driver could garble, so arch_console_reclaim is the no-op
# fallback; the per-chip reclaim bodies stay silicon-gated on mk64f and xmc4800.
#
# Each death knob is a build-wide option on the driver, so each case configures its own tree.
#
# usage: check_sim_drvdeath.sh <kickos-source-dir> <cmake>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

KICKOS_SRC="$1"
CMAKE="${2:-cmake}"

# grep -c exits 1 on zero matches, which under `set -e` kills the script before its fail
# message prints: red for the right reason, with no diagnostic.
count_of() { printf '%s\n' "$OUT" | grep -c "$1" || true; }
# The init's one line per death it releases and per start that failed.
DEATH_LINE='init: `simcon` is dead and released'
FAILED_LINE='init: `simcon` failed to start'
# KOS_EXIT_CANCELLED, the status a system ends with when the task it ends on never ran.
CANCELLED_STATUS=130

scratch_dir

echo "== configuring the sim: console driver bounded to 2 messages =="
( cd "$KICKOS_SRC" && "$CMAKE" --preset sim -B "$TMP/build" \
    -DKICKOS_SIMCON_EXIT_AFTER=2 >/dev/null ) \
  || fail "configure with the bounded console driver failed"

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

# First: a missing publish also makes the BEFORE marker appear, so reporting the marker
# would name the symptom and not the cause.
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

[ "$(count_of "$DEATH_LINE")" -eq 1 ] \
  || fail "the init did not report the driver's death exactly once on the reclaimed console"

# The positive half: the same call site, now carried by the reclaimed polled route.
COUNT="$(count_of '\[drvdeath\] kernel console AFTER death (reclaimed)')"
[ "$COUNT" -ne 0 ] \
  || fail "the console stayed DARK after the driver died: reclaim-on-death is not working"
[ "$COUNT" -eq 1 ] \
  || fail "the post-reclaim marker appeared $COUNT times (double-routed?)"

[ "$RC" -eq 0 ] || fail "expected a clean exit 0, got $RC"

# ---------------------------------------------------------------------------------
# Case 2: the driver dies BEFORE it ever receives, i.e. its start fails. Its death ends its
# task, which refuses the probe's rendezvous and gives the console back so the start can
# REPORT it, and the init counts a failed start, reports it and, with no
# restart, marks main, which uses the console, dependency-down: no app runs on a dark console,
# and the system ends with KOS_EXIT_CANCELLED. Without the probe the start returns 0 and the
# app runs against a console nothing is serving.
echo "== case 2: the driver dies during its start =="
( cd "$KICKOS_SRC" && "$CMAKE" --preset sim -B "$TMP/build2" \
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
has '\[simcon\] ERROR: the console handover probe was not taken' \
  || fail "case 2: the failed handover was NOT reported; either the probe missed the dead driver, or the console never came back to report on"
[ "$(count_of "$FAILED_LINE")" -eq 1 ] \
  || fail "case 2: the init did not report the failed start exactly once"
# The app must not have run at all: the server it uses is dead for good.
if has '\[drvdeath\]'; then
    fail "case 2: the app ran anyway, on a console with no driver; the failure was not loud"
fi
[ "$RC" -eq "$CANCELLED_STATUS" ] \
  || fail "case 2: expected KOS_EXIT_CANCELLED ($CANCELLED_STATUS) from a main that never ran, got $RC"

# ---------------------------------------------------------------------------------
# Case 3: a driver whose receiver exits while its task lives, and whose task then ends with a
# thread still holding the register window, the shape every silicon console driver has. The
# task is an entry holding nothing, a window thread at the lowest priority that holds the
# register window and parks in a notification wait, and the service thread that receives. It is
# the only case that reaches the reclaim's device precondition.
#
# The receiver's exit alone is no death: the console stays the driver's, the kernel console
# stays dark, a send of no deadline and a non-blocking write are refused at once, and a
# blocking writer parks, and a thread faulting in a task of its own leaves its record held for
# the driver. The entry's release then ends the task while the window thread still holds the
# registers: the reclaim waits for that thread's exit, writes the held record, and only then
# wakes the parked writer, which outranks the window thread, waited out that dark window, and
# lands its line exactly once, after the record. Without the dark window's wake the line never
# lands; keying the reclaim on the last receiver puts the window-HELD marker on the wire.
#
# The kill gate is the app's own: no thread of it can kill root, which runs the init, a
# stranger is refused, a spawner may cancel its child, and a second kill answers EBADF.
echo "== case 3: a receiver-less live driver, then its task's end with the window held =="
( cd "$KICKOS_SRC" && "$CMAKE" --preset sim -B "$TMP/build3" \
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

# Absent == the receiver's exit alone left the console with its driver.
if has '\[drvdeath\] kernel console AFTER death, window HELD'; then
    fail "case 3: the console came BACK after the receiver's exit, with the driver's task alive and a thread of it holding the UART register window"
fi

has '\[simcon\] task entry released, ending the driver'"'"'s task' \
  || fail "case 3: the driver's task entry never ended the task"
[ "$(count_of '\[drvdeath\] blocked line, written once after the reclaim')" -eq 1 ] \
  || fail "case 3: the line parked across the task's end did not land exactly once"
# The faulter's record was held for a driver that no longer receives, so it reaches the wire only
# when the reclaim writes it, and the reclaim writes it before it wakes the parked writer. A line
# ahead of it went out before the reclaim, which merely following the release does not exclude.
after_line() { printf '%s\n' "$OUT" | grep -n -- "$1" | head -n1 | cut -d: -f1; }
[ "$(count_of "$(thread_fault_re dfault)")" -eq 1 ] \
  || fail "case 3: the faulter's record held for the dead driver did not reach the wire exactly once"
_released="$(after_line 'task entry released')"
_record="$(after_line "$(thread_fault_re dfault)")"
_blocked="$(after_line 'blocked line, written once')"
[ -n "$_released" ] && [ -n "$_record" ] && [ "$_record" -gt "$_released" ] \
  || fail "case 3: the held record reached the wire before the driver's task ended"
[ -n "$_blocked" ] && [ "$_blocked" -gt "$_record" ] \
  || fail "case 3: the parked line reached the wire before the reclaim"
has '\[drvdeath\] kill gate: EBADF/EPERM refused, root unkillable, spawner accepted' \
  || fail "case 3: the thread_kill gate matrix did not pass"
[ "$(count_of "$DEATH_LINE")" -eq 1 ] \
  || fail "case 3: the init did not report the driver's death exactly once on the reclaimed console"

COUNT="$(count_of '\[drvdeath\] kernel console AFTER death (reclaimed)')"
[ "$COUNT" -ne 0 ] \
  || fail "case 3: the console stayed DARK after the window was released: the deferred reclaim never ran"
[ "$COUNT" -eq 1 ] \
  || fail "case 3: the post-reclaim marker appeared $COUNT times (double-routed?)"

[ "$RC" -eq 0 ] || fail "case 3: expected a clean exit 0, got $RC"

# ---------------------------------------------------------------------------------
# Case 4: the READY TIMEOUT, the expiry of the bounded loop every silicon console driver
# waits its IRQ thread's start with. KICKOS_SIMCON_IRQ_WEDGE gives the sim an IRQ thread
# that takes the register window and never sets `ready`, with a start ordered like the
# silicon drivers (publish, claim, IRQ thread, wait, service thread).
#
# The init's slay of the failed start is what gives the console back: it ends the driver's
# task, which notes the console dead, and the reclaim waits for the wedged thread, which still
# holds the window (dev_window_free in kernel/init/console.cc). The slay stops that thread in
# its notification wait: it never runs past that wait, and its exit releases the window.
#
# The assertion is a PAIR, as in case 1:
#   after the publish, before the timeout     : absent  (USER_OWNED drops it)
#   the init's report of the failed start     : present (only a reclaim can carry it)
# Absent-then-present is what proves a reclaim happened in between. Either half alone
# passes on a build where the publish never took, and then nothing is being tested.
echo "== case 4: the IRQ thread never reaches its loop, so the start's ready-wait expires =="
( cd "$KICKOS_SRC" && "$CMAKE" --preset sim -B "$TMP/build4" \
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

# Premise. Parked means the thread holds the window AND sits in a notification wait, so the
# timeout is not a spawn failure.
has '\[simcon\] wedge irq thread parked, ready never set' \
  || fail "case 4: the wedge irq thread never parked (no DEV window, or no line?)"

# THE ANTI-VACUITY HALF. This kernel-path write runs after the publish and before the
# timeout, so it must be DROPPED. On the wire it means the console was never USER_OWNED,
# and then the tag below reaches the wire with no reclaim involved and proves nothing.
if has '\[simcon\] wedge: post-publish kernel write'; then
    fail "case 4: a post-publish kernel write REACHED the wire, so the console was never published: every assertion below would be vacuous"
fi

# A panic reclaims from any state (kpanic_enter), so one here would carry the tag below
# instead of the driver-death path.
if has 'KERNEL PANIC'; then
    fail "case 4: the system panicked, so the reclaim cannot be attributed to the timeout path"
fi

# THE POSITIVE HALF: the init's report reached the wire, which only a reclaim allows.
COUNT="$(count_of "$FAILED_LINE")"
[ "$COUNT" -ne 0 ] \
  || fail "case 4: the failed start was never reported on the wire: the console was not given back, so the failure is silent"
[ "$COUNT" -eq 1 ] \
  || fail "case 4: the failed start was reported $COUNT times (double-routed?)"

# The slay stops the wedged thread at once: it never gets back to its own code past the wait.
if has '\[simcon\] wedge irq thread woke from its wait'; then
    fail "case 4: the wedged irq thread ran its own code past its wait, so the slay did not stop it at once"
fi

# Bounded: the start returned. 124 is the outer timeout, i.e. a hang.
[ "$RC" -ne 124 ] \
  || fail "case 4: the start never returned from its ready-wait, so the bound did not hold"
[ "$RC" -eq "$CANCELLED_STATUS" ] \
  || fail "case 4: expected KOS_EXIT_CANCELLED ($CANCELLED_STATUS) from a main that never ran, got $RC"

# No app may run on a console nothing is serving.
if has '\[drvdeath\]'; then
    fail "case 4: the app ran anyway, on a dark console"
fi

echo "PASS: the console returns to the kernel when its driver's task ends, a failed handover is loud, a receiver-less live driver parks its writers, the reclaim waits for its register owner, and a ready-timeout is reported"
