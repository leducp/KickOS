#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for CONSOLE RECLAIM ON DRIVER DEATH: build drvdeath, whose composition names the
# packaged simcon as stdout with no restart, with the console driver bounded to ONE served
# message (-DKICKOS_SIMCON_EXIT_AFTER=1), and require the kernel console comes BACK to the
# KERNEL when that driver exits, and to nobody else.
#
# What it defends: while a userspace driver owns the console, console_emit DROPS every
# kernel write (USER_OWNED). Without the reclaim hook a driver that exits leaves the
# system permanently mute (no panic banner, no fault dump, no kprintf). The hook is
# console_on_driver_death, run by exit_current AFTER cap_teardown so every IRQ cap is
# dropped and every line masked before the device is re-initialised.
#
# The assertion is a PAIR:
#   a user line, raw or through stdout, before and after the death: absent (refused)
#   the kernel's record of a fault after the death: present (the reclaimed route carries it)
# A record held for a driver that never comes back reaches no wire, so its presence is the
# reclaim. The app waits for the init's report of the death on /init/events and then requires
# -KOS_ECONNREFUSED from a send, so no timing assumption stands in for proof the driver is
# gone; every verdict after the death is its exit status. The init's own report of the death is
# a user line too, and absent.
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
# The init's one line per death it releases and per start that failed, user lines the console
# whose driver is gone refuses.
DEATH_LINE='init: `simcon` is dead and released'
FAILED_LINE='init: `simcon` failed to start'
# The kernel's own line on the device its console driver's death gave back.
RECLAIMED_LINE='kernel: console driver task ended, console reclaimed'
# The line number of the first line carrying $1.
after_line() { printf '%s\n' "$OUT" | grep -n -- "$1" | head -n1 | cut -d: -f1; }
# KOS_EXIT_CANCELLED, the status a system ends with when the task it ends on never ran.
CANCELLED_STATUS=130

scratch_dir

echo "== configuring the sim: console driver bounded to 1 message =="
( cd "$KICKOS_SRC" && "$CMAKE" --preset sim -B "$TMP/build" \
    -DKICKOS_SIMCON_EXIT_AFTER=1 >/dev/null ) \
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

[ "$RC" -ne 1 ] || fail "the driver outlived its bounded serve, so the reclaim was never reached"

has '\[simcon\] driver exiting (bounded serve)' \
  || fail "the driver never announced its exit (bounded-serve knob not applied?)"

if has '\[drvdeath\] kernel console AFTER death' || has '\[drvdeath\] stdout AFTER death'; then
    fail "a user line reached the console the kernel took back from the dead driver"
fi
[ "$(count_of "$DEATH_LINE")" -eq 0 ] \
  || fail "the init's report of the death reached a console whose driver is gone"
[ "$(count_of "$RECLAIMED_LINE")" -eq 1 ] \
  || fail "the kernel's line on the driver's death did not reach the reclaimed console exactly once"

# The positive half: the kernel's record of the faulter started after the death.
[ "$(count_of "$(thread_fault_re dfault)")" -eq 1 ] \
  || fail "the kernel's record of a fault after the death did not reach the wire exactly once: the console was not reclaimed"
_exiting="$(after_line 'driver exiting (bounded serve)')"
_record="$(after_line "$(thread_fault_re dfault)")"
[ -n "$_exiting" ] && [ -n "$_record" ] && [ "$_record" -gt "$_exiting" ] \
  || fail "the faulter's record reached the wire before the driver died"
_reclaimed="$(after_line "$RECLAIMED_LINE")"
[ -n "$_reclaimed" ] && [ "$_reclaimed" -gt "$_exiting" ] && [ "$_reclaimed" -lt "$_record" ] \
  || fail "the kernel's line on the driver's death is not between the driver's exit and the later record"

[ "$RC" -eq 0 ] || fail "expected a clean exit 0, got $RC (drvdeath/main.cc names each status)"

# ---------------------------------------------------------------------------------
# Case 2: the driver dies BEFORE it ever receives, i.e. its start fails. Its task ends before it
# is ready, and the init counts a failed start and, with no restart, marks main, which uses the
# console, dependency-down: no app runs on a console nobody serves, and the system ends with
# KOS_EXIT_CANCELLED. The init's report is a user line, refused.
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
[ "$(count_of "$FAILED_LINE")" -eq 0 ] \
  || fail "case 2: the init's report of the failed start reached a console whose driver is gone"
[ "$(count_of "$RECLAIMED_LINE")" -eq 1 ] \
  || fail "case 2: the kernel's line on the driver's death did not reach the reclaimed console exactly once"
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
# The receiver's exit alone is no death: the console stays the driver's, a send of no deadline
# and a non-blocking write are refused at once, a blocking writer parks, and a thread faulting in
# a task of its own leaves its record held for the driver. The entry's release then ends the
# task while the window thread still holds the registers: the parked writer is refused at once,
# its line on no route, and the reclaim waits for that thread's exit and writes the held record.
# Keying the reclaim on the last receiver writes the record before the release.
#
# The kill gate is the app's own: no thread of it can kill root, which runs the init, a
# stranger is refused, a spawner may cancel its child, and a second kill answers EBADF. Its
# verdicts are the app's exit status.
echo "== case 3: a receiver-less live driver, then its task's end with the window held =="
( cd "$KICKOS_SRC" && "$CMAKE" --preset sim -B "$TMP/build3" \
    -DKICKOS_SIMCON_EXIT_AFTER=1 \
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

has '\[simcon\] task entry released, ending the driver'"'"'s task' \
  || fail "case 3: the driver's task entry never ended the task"
if has '\[drvdeath\] kernel console AFTER death' || has '\[drvdeath\] stdout AFTER death' \
   || has '\[drvdeath\] blocked line'; then
    fail "case 3: a user line reached the console the kernel took back from the dead driver"
fi
# The faulter's record was held for a driver that no longer receives, so it reaches the wire only
# when the reclaim writes it. A line ahead of it went out before the reclaim, which merely
# following the release does not exclude.
[ "$(count_of "$(thread_fault_re dfault)")" -eq 1 ] \
  || fail "case 3: the faulter's record held for the dead driver did not reach the wire exactly once"
_released="$(after_line 'task entry released')"
_record="$(after_line "$(thread_fault_re dfault)")"
[ -n "$_released" ] && [ -n "$_record" ] && [ "$_record" -gt "$_released" ] \
  || fail "case 3: the held record reached the wire before the driver's task ended"
# The reclaim writes the records held for the driver, then its own line.
[ "$(count_of "$RECLAIMED_LINE")" -eq 1 ] \
  || fail "case 3: the kernel's line on the driver's death did not reach the reclaimed console exactly once"
_reclaimed="$(after_line "$RECLAIMED_LINE")"
[ -n "$_reclaimed" ] && [ "$_reclaimed" -gt "$_record" ] \
  || fail "case 3: the kernel's line on the driver's death came before the held record it follows"
[ "$(count_of "$DEATH_LINE")" -eq 0 ] \
  || fail "case 3: the init's report of the death reached a console whose driver is gone"

[ "$RC" -eq 0 ] || fail "case 3: expected a clean exit 0, got $RC (drvdeath/main.cc names each status)"

# ---------------------------------------------------------------------------------
# Case 4: the READY TIMEOUT, the expiry of the bounded loop every silicon console driver
# waits its IRQ thread's start with. KICKOS_SIMCON_IRQ_WEDGE gives the sim an IRQ thread
# that takes the register window and never sets `ready`, with a start ordered like the
# silicon drivers (publish, claim, IRQ thread, wait, service thread).
#
# The service thread's trap past that budget is what gives the console back: it ends the
# driver's task, which notes the console dead, and the reclaim waits for the wedged thread,
# which still holds the window (dev_window_free in kernel/init/console.cc). The task's end stops
# that thread in its notification wait: it never runs past that wait, and its exit releases the
# window. The init waits on the task with no deadline of its own, so the driver's budget is the
# only bound.
#
# The assertion is a PAIR, as in case 1:
#   a user write after the publish, before the timeout : absent  (refused once published)
#   the kernel's record of the service thread's trap    : present (held, only a reclaim writes it)
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

# THE ANTI-VACUITY HALF. This raw write runs after the publish and before the timeout, so it
# must be REFUSED. On the wire it means the console was never published, and then the record
# below reaches the wire with no reclaim involved and proves nothing.
if has '\[simcon\] wedge: post-publish kernel write'; then
    fail "case 4: a post-publish kernel console write REACHED the wire, so the console was never published: every assertion below would be vacuous"
fi

# A panic reclaims from any state (kpanic_enter), so one here would carry the tag below
# instead of the driver-death path.
if has 'KERNEL PANIC'; then
    fail "case 4: the system panicked, so the reclaim cannot be attributed to the timeout path"
fi

# THE POSITIVE HALF: the kernel's record of the service thread's trap, held for the driver while
# its task lived, reached the wire, which only the reclaim allows.
COUNT="$(count_of "$(thread_fault_re simcon)")"
[ "$COUNT" -ne 0 ] \
  || fail "case 4: the driver's fault record never reached the wire: the console was not given back"
[ "$COUNT" -eq 1 ] \
  || fail "case 4: the driver's fault record reached the wire $COUNT times (double-routed?)"
[ "$(count_of "$FAILED_LINE")" -eq 0 ] \
  || fail "case 4: the init's report of the failed start reached a console whose driver is gone"
[ "$(count_of "$RECLAIMED_LINE")" -eq 1 ] \
  || fail "case 4: the kernel's line on the driver's death did not reach the reclaimed console exactly once"

# The task's end stops the wedged thread at once: it never gets back to its own code past the wait.
if has '\[simcon\] wedge irq thread woke from its wait'; then
    fail "case 4: the wedged irq thread ran its own code past its wait, so the task's end did not stop it at once"
fi

# Bounded: the start returned. 124 is the outer timeout, i.e. a hang.
[ "$RC" -ne 124 ] \
  || fail "case 4: the start never returned from its ready-wait, so the bound did not hold"
[ "$RC" -eq "$CANCELLED_STATUS" ] \
  || fail "case 4: expected KOS_EXIT_CANCELLED ($CANCELLED_STATUS) from a main that never ran, got $RC"

# No app may run on a console nothing is serving.
if has '\[drvdeath\]'; then
    fail "case 4: the app ran anyway, on a console nothing serves"
fi

echo "PASS: the console returns to the kernel alone when its driver's task ends, a failed handover ends the system, a receiver-less live driver parks its writers, the reclaim waits for its register owner, and a ready-timeout is bounded"
