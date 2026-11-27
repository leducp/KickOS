#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CONSOLE RECLAIM ON DRIVER DEATH and the TERMINATE DRAIN, on one boot of reclaimwit
# (natively for the sim, on QEMU when QEMU_MACHINE is set).
#
# The verdict is a pair on the kernel console, plus the sink's silence:
#   MUTE lines : ZERO times   the publish took, so the kernel console refused those raw user
#                             writes, before the driver's death and after it
#   LIVE line  : EXACTLY once the kernel's record of main's own task panic, carrying the verdict,
#                which only the console the kernel took back writes
#   sink line  : ZERO times   the driver is a pure sink, so no byte is its work
# Either presence half alone passes on a build where the publish never took. The sink's
# silence is what makes the LIVE line attributable to arch_console_reclaim and not to a
# driver that was still serving.
#
# EVERY MATCH HERE IS grep -F ON A WHOLE EMISSION LINE. The app prints a reading key that
# spells the word MUTE four times, so `grep MUTE` matches the key on a CORRECT run and
# reports a broken publish that did not happen.
#
#   check_reclaimwit.sh <reclaimwit.elf> <park|drain>
#   KOS_CAPTURE=<log> check_reclaimwit.sh <board-build> <kickos-source> <cmake> <park|drain>
#   park  polled to the LIVE record, then killed. A poll that runs out is a FAILURE, never a
#         pass.
#   drain main's task panic ends the system: the app's LAST line must be the LIVE record's
#         sentinel line intact, which is what a drain cut short by arch_shutdown cannot produce.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=20}"
: "${SIM_TIMEOUT:=20}"

if judging_capture; then
    shift 3
    arm="${1:?usage: check_reclaimwit.sh <board-build> <kickos-source> <cmake> <park|drain>}"
else
    elf="${1:?usage: check_reclaimwit.sh <reclaimwit.elf> <park|drain>}"
    arm="${2:?usage: check_reclaimwit.sh <reclaimwit.elf> <park|drain>}"
fi

KEY_LINE='[reclaimwit] HOW TO READ THIS CAPTURE:'
KEY_END='[reclaimwit]     both absent == the reclaim did not fire, console still dark.'
MUTE_LINE='[reclaimwit] MUTE kernel console while the driver holds it'
MUTE_AFTER_LINE='[reclaimwit] MUTE kernel console after the driver died'
LIVE_LINE='  [reclaimwit] LIVE '
SINK_LINE='[reclaimwit] routed through the driver, which discards it'
RECLAIMED_LINE='kernel: console driver task ended, console reclaimed'
PASS_LINE='  [reclaimwit] LIVE PASS '
PARK_LINE='[reclaimwit] park arm:'
DRAIN_LINE='[reclaimwit] drain arm:'
TAIL_LINE='  [reclaimwit] LIVE PASS 0123456789abcdef <<<DRAIN-END>>>'

# grep -c exits 1 on zero matches, which kills a `set -e` caller before its fail message
# prints.
n_of() { printf '%s\n' "$OUT" | grep -cF -- "$1" || true; }

if judging_capture; then
    case "$arm" in
        park|drain)
            judge_capture reclaimwit
            ;;
        *)
            fail "<arm> is 'park' or 'drain', not '$arm'"
            ;;
    esac
else
    case "$arm" in
        park)
            # The last line of each terminal path, as one ERE. print_rc's failure prefix is in it
            # because an early failure otherwise burns the whole poll and reports no-progress
            # instead of its cause.
            poll_image "$elf" '(  \[reclaimwit\] LIVE |\[reclaimwit\]   FAIL )'
            if [ "$POLL_OK" -ne 1 ]; then
                fail "no terminal line reached the wire: the image did not boot, or the console never came back after the driver died"
            fi
            ;;
        drain)
            run_image "$elf"
            ;;
        *)
            fail "<arm> is 'park' or 'drain', not '$arm'"
            ;;
    esac
fi

assert_no_panic "the image panicked, so any reclaim cannot be credited to the driver's death"

# Premise, first: a capture that lost its head makes every absence assertion below vacuous.
if [ "$(n_of "$KEY_LINE")" -eq 0 ]; then
    cfail key "the app's reading key never reached the wire, so this capture witnesses nothing"
fi
# Every key line was printed while the kernel owned the console, so one missing is a line the
# kernel console lost before the publish, never the publish's doing.
if [ "$(n_of "$KEY_END")" -eq 0 ]; then
    cfail key "the app's reading key lost its tail: a key line never reached the wire before the publish"
fi

# The two arms are built from one source file and the wrong binary greps green on almost
# everything here, so each arm names the other's own line as its refusal.
if [ "$arm" = park ] && [ "$(n_of "$DRAIN_LINE")" -ne 0 ]; then
    cfail arm "the park gate was handed the drain binary"
fi
if [ "$arm" = drain ] && [ "$(n_of "$PARK_LINE")" -ne 0 ]; then
    cfail arm "the drain gate was handed the park binary"
fi

# THE ANTI-VACUITY HALF. This write runs while the driver owns the console, so the kernel
# console must refuse it. On the wire it means the publish never took, and the LIVE line below
# then reaches the wire with no reclaim involved.
if [ "$(n_of "$MUTE_LINE")" -ne 0 ]; then
    cfail mute "the kernel console was STILL live while the driver held it: the publish never took, and every assertion here is void"
fi
# The reclaimed console is the kernel's alone.
if [ "$(n_of "$MUTE_AFTER_LINE")" -ne 0 ]; then
    cfail mute "a user write reached the console the kernel took back from the dead driver"
fi

# The driver is a pure sink. A byte of its own on the wire means the console was not
# USER_OWNED, or the driver outlived the slay.
if [ "$(n_of "$SINK_LINE")" -ne 0 ]; then
    cfail sink "the console driver WROTE to a console, so a post-death byte no longer separates a fired reclaim from a driver that never died"
fi

# The kernel's own line on the slain driver's death, which only the reclaim writes.
if [ "$(n_of "$RECLAIMED_LINE")" -ne 1 ]; then
    cfail reclaimed "the kernel's line on the driver's death did not reach the wire exactly once"
fi

# THE POSITIVE HALF: the kernel's record of main's task panic, carried by the reclaimed polled
# route. Held for a driver that never comes back, it would reach no wire.
LIVE_N="$(n_of "$LIVE_LINE")"
if [ "$LIVE_N" -eq 0 ]; then
    cfail live "the console stayed DARK after the driver died: arch_console_reclaim did not fire"
fi
if [ "$LIVE_N" -ne 1 ]; then
    cfail live "the post-reclaim line appeared $LIVE_N times (double-routed?)"
fi

if [ "$(n_of "$PASS_LINE")" -eq 0 ]; then
    cfail verdict "the app did not report PASS; its LIVE FAIL record names close, serve, slay and post-death send answers"
fi

if [ "$arm" = drain ]; then
    # The sentinel is 40 characters past the line's start, so a drain that stopped when
    # arch_shutdown did leaves a SHORT line rather than a missing one. Equality is what
    # catches that; presence alone does not.
    #
    # The app's LAST line, not the capture's: a telemetry build prints its ring summary from
    # the kernel after main ends, so the drain tail is not the final byte on the wire.
    LAST="$(printf '%s\n' "$OUT" | grep -F '[reclaimwit]' | tail -n 1)"
    if [ "$LAST" != "$TAIL_LINE" ]; then
        cfail drain-tail "the app's last line is not the drain sentinel intact; it is: $LAST"
    fi
    status_clause "the drain arm shut the system down with main's panic status, 134" 134
fi

echo "PASS: the console came back to the kernel on the driver's death ($arm arm)"
exit 0
