#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The packaged-driver line witness: the system of tests/integration/driver_witness under
# <composition>, built against the build's installed package, whose catalogue carries
# tests/drivers, and run under QEMU. testline's descriptor numbers its two lines as none a claim
# takes and states another index for its line 0; the composition binds line 0 to a line at index
# 2 of its device. Its IRQ thread prints the index its spawn handed it; its raiser, holding a
# SIGNAL-only copy of the line the driver claimed as line 0, raises it until the IRQ thread sees
# it; its client calls until the driver answers that it did. The run passes on index 2, the
# driver's report of its line, the client's, and the system ending with status 0.
#
#   check_driver_line.sh <kickos-build> <kickos-source> <cmake> <composition>

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

USAGE="usage: check_driver_line.sh <kickos-build> <kickos-source> <cmake> <composition>"
KICKOS_BUILD="${1:?$USAGE}"
KICKOS_SRC="${2:?$USAGE}"
CMAKE="${3:?$USAGE}"
SYSTEM="${4:?$USAGE}"
[ "$#" -eq 4 ] || fail "$USAGE"
need_qemu_machine
[ -f "$SYSTEM" ] || fail "no composition at $SYSTEM"

BOARD_CFG="$KICKOS_BUILD/generated/include/kickos/board_config.h"

scratch_dir

echo "== building the line witness against the installed package =="
package_image "$KICKOS_BUILD" "$CMAKE" "$KICKOS_SRC/tests/integration/driver_witness" driver_witness \
    -DKICKOS_WITNESS_SYSTEM="$SYSTEM"

echo "== running the line witness =="
run_image "$IMAGE"
assert_no_panic "a panic in the line witness"
if has '^=== THREAD FAULT ==='; then
    fail "a thread faulted"
fi
require_on_wire 'testline: line 0 is index 2 of its device' "testline's IRQ thread was not handed line 0's index, 2"
require_on_wire 'testline: line 0 was raised' "testline never saw the line its composition binds"
require_on_wire 'line: testline reports line 0 raised' \
    "the client never heard the driver report its line"
if [ "$RC" -eq 124 ]; then
    fail "the system never ended (timed out)"
fi
if [ "$RC" -ne 0 ]; then
    fail "the system ended with status $RC, not 0"
fi
echo "PASS: testline claimed the lines its composition binds and was handed line 0's index"
