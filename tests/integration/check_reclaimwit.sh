#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CONSOLE RECLAIM ON DRIVER DEATH and the TERMINATE DRAIN, on one boot of reclaimwit
# (natively for the sim, on QEMU when QEMU_MACHINE is set).
#
# The verdict is a pair from the SAME kos_print call site, plus the sink's silence:
#   MUTE line : ZERO times   the publish took, so USER_OWNED dropped that kernel write
#   LIVE line : EXACTLY once the reclaimed polled route carries the same call site
#   sink line : ZERO times   the driver is a pure sink, so no byte is its work
# Either presence half alone passes on a build where the publish never took. The sink's
# silence is what makes the LIVE line attributable to arch_console_reclaim and not to a
# driver that was still serving.
#
# EVERY MATCH HERE IS grep -F ON A WHOLE EMISSION LINE, and that is the whole design. The
# app prints a reading key that spells the word MUTE four times, so `grep MUTE` matches the
# key on a CORRECT run and reports a broken publish that did not happen.
#
# usage: check_reclaimwit.sh <reclaimwit.elf> <park|drain>
#   park  the app never returns: polled to its last line, then killed. A poll that runs
#         out is a FAILURE, never a pass.
#   drain main returns into kickos_terminate: the app's LAST line must be the sentinel line
#         intact, which is what a drain cut short by arch_shutdown cannot produce.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=20}"
: "${SIM_TIMEOUT:=20}"

elf="${1:?usage: check_reclaimwit.sh <reclaimwit.elf> <park|drain>}"
arm="${2:?usage: check_reclaimwit.sh <reclaimwit.elf> <park|drain>}"

KEY_LINE='[reclaimwit] HOW TO READ THIS CAPTURE:'
MUTE_LINE='[reclaimwit] MUTE kernel console while the driver holds it'
LIVE_LINE='[reclaimwit] LIVE kernel console after the driver died'
SINK_LINE='[reclaimwit] routed through the driver, which discards it'
PASS_LINE='[reclaimwit] PASS reclaim fired'
PARK_LINE='[reclaimwit] park arm: the system stays up'
TAIL_LINE='[reclaimwit] DRAINTAIL 0123456789abcdef0123456789abcdef <<<DRAIN-END>>>'

# grep -c exits 1 on zero matches, which would kill a `set -e` caller before its fail
# message printed.
n_of() { printf '%s\n' "$OUT" | grep -cF -- "$1" || true; }

case "$arm" in
    park)
        # The last line of each terminal path, as one ERE: the park banner on a run that
        # reached the end, the run-time refusal, and print_rc's failure prefix. Without the
        # last two an early failure burns the whole poll and reports no-progress instead of
        # its cause.
        poll_image "$elf" '\[reclaimwit\] (park arm: the system stays up|REFUSE: this image already publishes|  FAIL )'
        if [ "$POLL_OK" -ne 1 ]; then
            fail "no terminal line reached the wire: the image did not boot, or the console never came back after the driver died"
        fi
        ;;
    drain)
        run_image "$elf"
        if [ "$RC" -eq 124 ]; then
            fail "the drain arm never ended the system (timed out in kickos_terminate?)"
        fi
        ;;
    *)
        fail "<arm> is 'park' or 'drain', not '$arm'"
        ;;
esac

assert_no_panic "the image panicked, so any reclaim cannot be credited to the driver's death"

# Premise, checked first: a capture that lost its head would make every absence assertion
# below vacuous.
if [ "$(n_of "$KEY_LINE")" -eq 0 ]; then
    fail "the app's reading key never reached the wire, so this capture witnesses nothing"
fi
if has "\[reclaimwit\] REFUSE:"; then
    fail "the app refused: this image already publishes a userspace console, so it cannot kill the console's own driver. Build with -DKICKOS_SERVICE_LIST=kickos_services_none"
fi

# The wrong binary greps green on almost everything here, and the two arms are built from
# one source file, so each one names the other's terminal line as its own refusal.
if [ "$arm" = park ] && [ "$(n_of "$TAIL_LINE")" -ne 0 ]; then
    fail "the park gate was handed the drain binary"
fi
if [ "$arm" = drain ] && [ "$(n_of "$PARK_LINE")" -ne 0 ]; then
    fail "the drain gate was handed the park binary"
fi

# THE ANTI-VACUITY HALF. This write runs while the driver owns the console, so USER_OWNED
# must drop it. On the wire it means the publish never took, and the LIVE line below then
# reaches the wire with no reclaim involved.
if [ "$(n_of "$MUTE_LINE")" -ne 0 ]; then
    fail "the kernel console was STILL live while the driver held it: the publish never took, and every assertion here is void"
fi

# The driver is a pure sink. A byte of its own on the wire means the console was not
# USER_OWNED, or the driver outlived the slay.
if [ "$(n_of "$SINK_LINE")" -ne 0 ]; then
    fail "the console driver WROTE to a console, so a post-death byte no longer separates a fired reclaim from a driver that never died"
fi

# THE POSITIVE HALF: the same call site as the MUTE line, now carried by the reclaimed
# polled route.
LIVE_N="$(n_of "$LIVE_LINE")"
if [ "$LIVE_N" -eq 0 ]; then
    fail "the console stayed DARK after the driver died: arch_console_reclaim did not fire"
fi
if [ "$LIVE_N" -ne 1 ]; then
    fail "the post-reclaim line appeared $LIVE_N times (double-routed?)"
fi

if [ "$(n_of "$PASS_LINE")" -eq 0 ]; then
    fail "the app did not report PASS; read its rc lines in the capture above"
fi

if [ "$arm" = drain ]; then
    # The sentinel is 48 characters past the line's start, so a drain that stopped when
    # arch_shutdown did leaves a SHORT line rather than a missing one. Equality is what
    # catches that; presence alone does not.
    #
    # The app's LAST line, not the capture's: a telemetry build prints its ring summary from
    # the kernel after main returns, so the drain tail is not the final byte on the wire.
    LAST="$(printf '%s\n' "$OUT" | grep '^\[reclaimwit\]' | tail -n 1)"
    if [ "$LAST" != "$TAIL_LINE" ]; then
        fail "the app's last line is not the drain sentinel intact; it is: $LAST"
    fi
    if [ "$RC" -ne 0 ]; then
        fail "the drain arm shut down with status $RC, not 0"
    fi
fi

echo "PASS: the console came back to the kernel on the driver's death ($arm arm)"
exit 0
