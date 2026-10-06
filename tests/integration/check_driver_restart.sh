#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The driver restart witness (docs/design-m10-target.md, section 8): the system of
# tests/integration/driver_witness under <composition>, built against the build's installed
# package, whose catalogue carries tests/drivers, and run under QEMU. Three packaged drivers, each
# restarted once through its descriptor:
#   exits  serves five requests per instance, then its entry thread exits;
#   traps  serves five from a thread that is not its entry thread, which then traps;
#   fails  whose start fails before its first spawn.
# The capture passes, for exits and traps, on five answers, five answers of the restarted
# instance and the client's refusal, in that order, each death after its instance's fourth answer,
# and for traps on the worker's fault report in each instance after its fourth answer and before
# its death; for fails, on each failed start told to the watcher, then its client, which the init
# never starts, marked dependency-down. With `depth`, root's stack is filled and scanned while the
# init runs, and the run passes only with KICKOS_MIN_STACK_SIZE left above its thread-local block.
#
#   check_driver_restart.sh <kickos-build> <kickos-source> <cmake> <composition> [depth]
#
# Above one core user lines can reach the wire shuffled, so a positive is matched whole first and
# then through wire_has across two adjacent lines (after, tests/lib/gate.sh).
#
# With KOS_POLL_REPLAY=<log>, <log> is judged as the console the poll stopped at, and the witness
# exits 3 where the poll would not stop (poll_image, tests/lib/gate.sh).

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

USAGE="usage: check_driver_restart.sh <kickos-build> <kickos-source> <cmake> <composition> [depth]"
KICKOS_BUILD="${1:?$USAGE}"
KICKOS_SRC="${2:?$USAGE}"
CMAKE="${3:?$USAGE}"
SYSTEM="${4:?$USAGE}"
DEPTH="${5:-}"
if [ -z "${KOS_POLL_REPLAY:-}" ]; then
    need_qemu_machine
fi
[ -f "$SYSTEM" ] || fail "no composition at $SYSTEM"

BOARD_CFG="$KICKOS_BUILD/generated/include/kickos/board_config.h"
CORES="$(knob KICKOS_KERNEL_CORES)"

scratch_dir

set -- -DKICKOS_WITNESS_SYSTEM="$SYSTEM"
if [ "$DEPTH" = depth ]; then
    set -- "$@" -DKICKOS_WITNESS_ROOT_DEPTH=ON
elif [ -n "$DEPTH" ]; then
    fail "$USAGE"
fi
IMAGE=""
if [ -z "${KOS_POLL_REPLAY:-}" ]; then
    echo "== building the driver witness against the installed package =="
    package_image "$KICKOS_BUILD" "$CMAKE" "$KICKOS_SRC/tests/integration/driver_witness" driver_witness "$@"
    # Its composition names packaged drivers, so its system links the init's driver path.
    "$(dirname "$0")/check_no_driver_path.sh" --control "$IMAGE.map" \
        || fail "the driver witness's system does not link the init's driver path alone"
fi

# A driver's second-death line may follow its client's refusal.
all_ended() { # <log>
    log_holds "$1" 'exits: refused' 'traps: refused' 'watch: exits deaths 2 left 0 gone' \
        'watch: traps deaths 2 left 0 gone' 'watch: fails_client deaths 0 left 0 down'
}
KOS_POLL_UNTIL=all_ended
echo "== running the driver witness =="
poll_image "$IMAGE"
assert_no_panic "a panic in the driver witness"
if [ "$POLL_UNTIL_OK" -ne 1 ]; then
    fail "the drivers did not all end in ${QEMU_TIMEOUT}s (alive at the end: $POLL_ALIVE)"
fi

# The fifth answer may follow the death, and the worker's fault report follows the fourth answer.
in_order() { # <who>
    _io_at=1
    for _io_death in "deaths 1 left 0 alive" "deaths 2 left 0 gone"; do
        _io_k=1
        while [ "$_io_k" -le 5 ]; do
            after "$_io_at" "$1: served $_io_k by /svc/$1" "answer $_io_k of $1 before its $_io_death"
            if [ "$_io_k" -eq 4 ]; then
                _io_fourth=$AT
            fi
            _io_at=$AT
            _io_k=$((_io_k + 1))
        done
        after "$_io_fourth" "watch: $1 $_io_death" "$1's $_io_death"
        _io_death_at=$AT
        if [ "$1" = traps ]; then
            after "$_io_fourth" "$(thread_fault_re worker)" "the worker's fault report before its $_io_death" part
            [ "$AT" -le "$_io_death_at" ] || fail "the worker's fault report follows its $_io_death"
        fi
    done
    after "$_io_at" "$1: refused" "$1's client refused"
}
in_order exits
in_order traps
# Each answer once per instance: the chain above finds every one in order. Two copies of one
# number are legal, so a line of one instance repeated where the other's is missing is not seen.
numbered_matcher_control
for _who in exits traps; do
    require_numbered "$_who: served ([0-9]+) by /svc/$_who" "answer of $_who" 1 1 2 2 3 3 4 4 5 5
done
after 1 'watch: fails deaths 1 left 0 alive' "the watcher told of the first failed start"
after "$AT" 'watch: fails deaths 2 left 0 gone' "the watcher told of the second, for good"
after "$AT" 'watch: fails_client deaths 0 left 0 down' "the client of fails marked dependency-down"
echo "PASS: each driver died, restarted through its descriptor, died for good and was refused"

if [ "$DEPTH" = depth ]; then
    require_root_depth
fi
