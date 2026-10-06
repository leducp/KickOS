#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The packaged-driver line witness: the system of tests/integration/driver_witness under
# <composition>, built against the build's installed package, whose catalogue carries
# tests/drivers, and run under QEMU. testline's descriptor numbers its line as one no claim
# takes, and the composition binds its role to a real one; its client raises the composition's
# line until the driver, which answers again only once that line reached it, answers. The run
# passes on the driver's report of its line, the client's answer and the system ending with
# status 0.
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
require_on_wire 'testline: its line was raised' "testline never saw the line its composition binds"
LINE="$(printf '%s\n' "$OUT" | sed -n 's/^\(line: served [0-9][0-9]* once line [0-9][0-9]* was raised\)$/\1/p' | head -n 1)"
[ -n "$LINE" ] || fail "the client was never served"
if [ "$RC" -eq 124 ]; then
    fail "the system never ended (timed out)"
fi
if [ "$RC" -ne 0 ]; then
    fail "the system ended with status $RC, not 0"
fi
echo "PASS: testline claimed the line its composition binds, not the one its descriptor numbers ($LINE)"
