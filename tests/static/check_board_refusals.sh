#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The configure refuses a board with no board file and a board with no default composition: the
# tracked tree is copied, one file is removed from the sim board, and the configure must fail
# naming what the board lacks.
#
#   check_board_refusals.sh <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

[ "$#" -eq 1 ] || fail "usage: check_board_refusals.sh <cmake>"
CMAKE="$1"
require_repo_root
scratch_dir
corpus "$TMP/files" "tracked file" .

arms=0
while IFS='|' read -r dropped want; do
    src="$TMP/src$arms"
    mkdir -p "$src"
    tar -cf - -T "$TMP/files" | tar -xf - -C "$src" || fail "the tracked tree could not be copied"
    rm -f "$src/$dropped"
    if "$CMAKE" -S "$src" -B "$src/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$src/cmake/toolchain-host.cmake" \
            -DKICKOS_BOARD=sim -DKICKOS_CONFIG_VARIANT=base > "$TMP/configure$arms.log" 2>&1; then
        fail "the configure of the sim with no $dropped succeeded"
    fi
    tr '\n' ' ' < "$TMP/configure$arms.log" | tr -s ' ' | grep -qF "$want" || {
        sed -n '/CMake Error/,$p' "$TMP/configure$arms.log" | sed -n '1,15p' >&2
        fail "the configure of the sim with no $dropped failed without saying '$want'"
    }
    arms=$((arms + 1))
done <<ARMS
platform/sim/sim.yaml|has no board file platform/sim/sim.yaml
boards/sim/composition.yaml|has no default composition boards/sim/composition.yaml
ARMS
echo "PASS: board_refusals ($arms configure(s) refused)"
