#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The in-task driver witness: the system of tests/integration/driver_witness under <composition>,
# whose one task lists testlate under `drivers`, built against the build's installed package and
# run under QEMU. testlate settles past a call's timeout before its first receive, runs below the
# task, serves five requests and exits. The task's entry calls the driver's endpoint five times
# with no retry, then a sixth time. The capture passes on five answers in order and an end with
# KOS_EXIT_FAULT before the sixth call returns: the task's entry thread brought the driver up as
# threads of its own task, the driver served before the entry ran, and the exit of the driver's
# thread failed the task it runs in.
#
#   check_driver_intask.sh <kickos-build> <kickos-source> <cmake> <composition>

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

USAGE="usage: check_driver_intask.sh <kickos-build> <kickos-source> <cmake> <composition>"
KICKOS_BUILD="${1:?$USAGE}"
KICKOS_SRC="${2:?$USAGE}"
CMAKE="${3:?$USAGE}"
SYSTEM="${4:?$USAGE}"
need_qemu_machine
[ -f "$SYSTEM" ] || fail "no composition at $SYSTEM"

scratch_dir

echo "== building the in-task driver witness against the installed package =="
package_image "$KICKOS_BUILD" "$CMAKE" "$KICKOS_SRC/tests/integration/driver_witness" driver_witness \
    -DKICKOS_WITNESS_SYSTEM="$SYSTEM" -DKICKOS_WITNESS_MAIN=intask
echo "== running the in-task driver witness =="
run_image "$IMAGE"
assert_no_panic "a panic in the in-task driver witness"
if has 'not served'; then
    fail "a call was not served: the driver was not up when the task's entry ran"
fi
AT=1
for _k in 1 2 3 4 5; do
    after "$AT" "intask: served $_k by /svc/late" "answer $_k of testlate"
done
numbered_matcher_control
require_numbered "intask: served ([0-9]+) by /svc/late" "answer of testlate" 1 2 3 4 5
if has 'intask: call 6 answered'; then
    fail "the task outlived the exit of its driver's thread"
fi
# KOS_EXIT_FAULT, restated rather than computed.
status_clause "the exit of the driver's thread failed its task with KOS_EXIT_FAULT (139)" 139
echo "PASS: the driver ran as threads of its task, served it from its first call and failed it on its exit"
