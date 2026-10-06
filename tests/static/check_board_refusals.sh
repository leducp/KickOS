#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The configure refuses a board with no board file and a board with no default composition: the
# tracked tree is copied, one file is removed from the sim board, and the configure must fail
# naming what the board lacks. The same copy with nothing removed must configure, so a refusal is
# the missing file's and not the copy's.
#
#   check_board_refusals.sh <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

[ "$#" -eq 1 ] || fail "usage: check_board_refusals.sh <cmake>"
CMAKE="$1"
require_repo_root
scratch_dir
corpus "$TMP/files" "tracked file" .

copy_tree() { # <dir>
    mkdir -p "$1"
    tar -cf - -T "$TMP/files" | tar -xf - -C "$1" || fail "the tracked tree could not be copied"
}
configure_sim() { # <dir> <log>
    "$CMAKE" -S "$1" -B "$1/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$1/cmake/toolchain-host.cmake" \
        -DKICKOS_BOARD=sim -DKICKOS_CONFIG_VARIANT=base > "$2" 2>&1
}

copy_tree "$TMP/control"
configure_sim "$TMP/control" "$TMP/control.log" || {
    sed -n '/CMake Error/,$p' "$TMP/control.log" | sed -n '1,15p' >&2
    fail "the copied tree with nothing removed did not configure, so no refusal below is its file's"
}
rm -rf "$TMP/control"

arms=0
while IFS='|' read -r dropped want; do
    src="$TMP/src$arms"
    copy_tree "$src"
    rm -f "$src/$dropped"
    if configure_sim "$src" "$TMP/configure$arms.log"; then
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
