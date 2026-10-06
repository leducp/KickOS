#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Host gate for the link group's provider names: a user-named pin map with no KickOS:: name is
# refused at configure, naming kickos_alias_target, and the same pin map given that name with
# kickos_alias_target configures.
#
# usage: check_provider_alias.sh <kickos-source-dir> <cmake>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

KICKOS_SRC="$1"
CMAKE="${2:-cmake}"

scratch_dir

FIXTURE="$KICKOS_SRC/tests/integration/provider_fixture.cmake"
configure() { # <build dir> [<cmake arg>...]
    _c_dir="$1"
    shift
    ( cd "$KICKOS_SRC" && "$CMAKE" --preset sim -B "$_c_dir" \
        -DCMAKE_PROJECT_INCLUDE="$FIXTURE" -DKICKOS_BOARD_PINMAP=kickos_pinmap_fixture "$@" ) \
        >"$_c_dir.log" 2>&1
}

echo "== a pin map with no KickOS:: name is refused =="
if configure "$TMP/bare"; then
    fail "a pin map with no KickOS:: name configured"
fi
tr -s ' \n' '  ' <"$TMP/bare.log" >"$TMP/bare.flat"
grep -q 'names kickos_pinmap_fixture as KickOS::kickos_pinmap_fixture, which is no target' \
    "$TMP/bare.flat" && grep -q 'kickos_alias_target' "$TMP/bare.flat" \
    || fail "the configure failed without the provider refusal: \
$(sed -n '/CMake Error/,+6p' "$TMP/bare.log" | tr '\n' ' ')"

echo "== the same pin map named with kickos_alias_target configures =="
configure "$TMP/alias" -DKICKOS_FIXTURE_ALIAS=ON || {
    sed -n '/CMake Error/,$p' "$TMP/alias.log" | sed -n '1,20p' >&2
    fail "a pin map named with kickos_alias_target was refused (see above)"
}

echo "PASS: an unnamed provider is refused, a named one configures"
