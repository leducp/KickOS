#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The composition witnesses (docs/design-m10-target.md, section 8): a system of
# tests/integration/composition_witness under <composition>, built against the build's installed
# package as a consumer builds one, and run under QEMU.
#
#   check_composition_witness.sh <kickos-build> <kickos-source> <cmake> restart <composition> [depth]
#   check_composition_witness.sh <kickos-build> <kickos-source> <cmake> line <composition>
#   check_composition_witness.sh <kickos-build> <kickos-source> <cmake> priority <composition>
#
#   restart  The test sensor serves five readings per instance and returns, under a copy of the
#            board's golden composition giving it two restarts, beside the witness's own app and
#            health checker. Passes on the sensor's refusals of a thread above its priority and,
#            above one core, of one on another core, then on readings 1 to 4, 6 to 9 and 11 to
#            14 in order, each instance's last read before health's report of its death with the
#            fifth reading of the instance last in /shm/history, of its restart with one and then
#            none left, and of its end for good, and before the app's `sensor: restarting`, which
#            comes ahead of the next instance's first reading, or its `sensor: gone for good`.
#            `sensor: restarting` is a call answered -KOS_EAGAIN while a restarted instance has
#            not yet first received. Health's lines are ordered against the app's only where the
#            death orders them, since above one core the two print at once.
#            With `depth`, root's stack is filled and scanned while the init runs, and the run
#            passes on the init's high-water mark leaving at least KICKOS_MIN_STACK_SIZE of
#            KICKOS_ROOT_STACK_SIZE free above the thread-local block.
#   line     A user task holding the PL031's window and its alarm line on core 1 arms the match
#            interrupt and prints its arrival; its end ends the system with status 0.
#   priority On one core, a composition stating the init's priority between two tasks' that
#            watch a pulse it restarts once: the task above the init reads no death of the pulse
#            while it spins across one, and the task below reads one arrive, the init having
#            preempted it to report it; the latter's end ends the system with status 0.
#
# With KOS_CAPTURE=<log>, the restart witness's last boot in a silicon capture of it is judged
# instead of a build and a run under QEMU. With WITNESS_LINK_ONLY=1, on a board no emulator runs,
# the restart witness stops once it has linked.
#
# Above one core user lines can reach the wire shuffled, so a positive is matched whole first
# and then through wire_has across two adjacent lines (after, tests/lib/gate.sh). Every absence
# checked below is also a positive above that its defect breaks: a panic or a fault stops the run
# short of `sensor: gone for good`; a widened thread admitted is a refusal line missing; a reading
# lost, frozen or died holding is a reading of 1 to 15 missing or out of order, or `last reading`
# not the fifth of its instance; a missing history window is no reading at all.

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

USAGE="usage: check_composition_witness.sh <kickos-build> <kickos-source> <cmake> restart <composition> [depth] | line <composition> | priority <composition>"
KICKOS_BUILD="${1:?$USAGE}"
KICKOS_SRC="${2:?$USAGE}"
CMAKE="${3:?$USAGE}"
WITNESS="${4:?$USAGE}"
SYSTEM="${5:?$USAGE}"
DEPTH="${6:-}"
: "${WITNESS_LINK_ONLY:=0}"
if [ "$WITNESS" != restart ] || { [ -z "${KOS_CAPTURE:-}" ] && [ "$WITNESS_LINK_ONLY" -ne 1 ]; }; then
    need_qemu_machine
fi
[ -f "$SYSTEM" ] || fail "no composition at $SYSTEM"

PROJECT="$KICKOS_SRC/tests/integration/composition_witness"
BOARD_CFG="$KICKOS_BUILD/generated/include/kickos/board_config.h"
CORES="$(knob KICKOS_KERNEL_CORES)"

scratch_dir

if [ "$WITNESS" = line ]; then
    echo "== building the line taker against the installed package =="
    package_image "$KICKOS_BUILD" "$CMAKE" "$PROJECT" witness -DKICKOS_WITNESS=line \
        -DKICKOS_WITNESS_SYSTEM="$SYSTEM"
    run_image "$IMAGE"
    assert_no_panic "a panic in the line taker's system"
    if has '^=== THREAD FAULT ==='; then
        fail "a thread faulted"
    fi
    LINE="$(printf '%s\n' "$OUT" | sed -n 's/^\(alarm: the match interrupt arrived at [0-9][0-9]*, armed for [0-9][0-9]*\)$/\1/p' | head -n 1)"
    [ -n "$LINE" ] || fail "the alarm task never printed the match interrupt's arrival"
    if [ "$RC" -eq 124 ]; then
        fail "the system never ended (timed out)"
    fi
    if [ "$RC" -ne 0 ]; then
        fail "the system ended with status $RC, not 0"
    fi
    echo "PASS: the PL031's match interrupt reached the user task holding its line on core 1 ($LINE)"
    exit 0
fi
if [ "$WITNESS" = priority ]; then
    [ "$CORES" -eq 1 ] || fail "the priority witness reads one core's order, and this build schedules $CORES"
    echo "== building the priority witness against the installed package =="
    package_image "$KICKOS_BUILD" "$CMAKE" "$PROJECT" witness -DKICKOS_WITNESS=priority \
        -DKICKOS_WITNESS_SYSTEM="$SYSTEM"
    run_image "$IMAGE"
    assert_no_panic "a panic in the priority witness's system"
    if has '^=== THREAD FAULT ==='; then
        fail "a thread faulted"
    fi
    has '^prio: the init stayed below a task above it$' \
        || fail "the task above the init did not report the init staying below it"
    has '^prio: the init preempted a task below it$' \
        || fail "the task below the init did not report the init preempting it"
    if [ "$RC" -eq 124 ]; then
        fail "the system never ended (timed out)"
    fi
    if [ "$RC" -ne 0 ]; then
        fail "the system ended with status $RC, not 0"
    fi
    echo "PASS: the init ran between the priorities of the tasks above and below it"
    exit 0
fi
[ "$WITNESS" = restart ] || fail "$USAGE"

set -- -DKICKOS_WITNESS=restart -DKICKOS_WITNESS_SYSTEM="$SYSTEM"
if [ "$DEPTH" = depth ]; then
    set -- "$@" -DKICKOS_WITNESS_ROOT_DEPTH=ON
elif [ -n "$DEPTH" ]; then
    fail "$USAGE"
fi
if [ -n "${KOS_CAPTURE:-}" ]; then
    echo "== reading the restart witness's capture $KOS_CAPTURE =="
    capture_out "$KOS_CAPTURE"
    printf '%s\n' "$OUT"
else
    echo "== building the restart witness against the installed package =="
    package_image "$KICKOS_BUILD" "$CMAKE" "$PROJECT" witness "$@"
    if [ "$WITNESS_LINK_ONLY" -eq 1 ]; then
        echo "PASS: the restart witness links against the package"
        exit 0
    fi

    echo "== running the restart witness =="
    gone_for_good() { # <log>
        log_holds "$1" 'sensor: gone for good'
    }
    KOS_POLL_UNTIL=gone_for_good
    poll_image "$IMAGE"
fi
require_console_whole

after 1 'sensor: a thread above its priority was refused' "refusal of a thread above the sensor's priority"
REFUSED=$AT
if [ "$CORES" -gt 1 ]; then
    after 1 'sensor: a thread on another core was refused' "refusal of a thread on another core"
    if [ "$AT" -gt "$REFUSED" ]; then
        REFUSED=$AT
    fi
fi

# One instance: its readings <first> to <first> + 3 from line <from> on, then health's report of
# its death with reading <first> + 4 last in the history. Sets LAST, the line of its last reading
# the app printed, and DIED.
instance() { # <from> <first>
    _in_at="$1"
    _in_k="$2"
    while [ "$_in_k" -le $(($2 + 3)) ]; do
        after "$_in_at" "sensor: $_in_k from /svc/sensor" "reading $_in_k"
        _in_at=$AT
        _in_k=$((_in_k + 1))
    done
    LAST=$AT
    after "$LAST" "health: sensor died, last reading $(($2 + 4)) in /shm/history" "death at reading $(($2 + 4))"
    DIED=$AT
}
# A restart: health's line after its report of the death, and the app's `sensor: restarting`
# after its last reading of the dead instance and before reading <first>, the first the restarted
# instance serves. Sets NEXT, the line of `sensor: restarting`.
restart() { # <deaths> <left> <first>
    after "$DIED" "health: sensor restarted ($1 deaths, $2 restarts left)" "restart after $1 death(s)"
    after "$LAST" 'sensor: restarting' "'sensor: restarting' after restart $1"
    NEXT=$AT
    after "$NEXT" "sensor: $3 from /svc/sensor" "reading $3 after 'sensor: restarting'"
}

instance "$REFUSED" 1
restart 1 1 6
instance "$NEXT" 6
restart 2 0 11
instance "$NEXT" 11
after "$DIED" 'health: sensor is down for good, running degraded' "end for good"
after "$LAST" 'sensor: gone for good' "'sensor: gone for good' from the app"

# Over the whole capture, once the run has ended, each corroborated by a positive above.
assert_no_panic "a panic in the restart witness"
for _line in '^=== THREAD FAULT ===' '^sensor: no reading' '^sensor: died holding the request' \
             '^sensor: measurement frozen' '^sensor: a thread .* was not refused' \
             '^sensor: no /shm/history window'; do
    if has "$_line"; then
        fail "the capture holds a '$_line' line"
    fi
done
echo "PASS: the sensor was refused its widened threads, died three times with its readings kept, \
restarted twice with its caller told -KOS_EAGAIN until it received, and was then gone for good"

if [ "$DEPTH" = depth ]; then
    require_root_depth
fi
