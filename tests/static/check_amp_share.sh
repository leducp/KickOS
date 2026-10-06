#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Gate on the configure-time refusals of an AMP partition's user share (cmake/amp_share.cmake): a
# share past the region every node writes, an uncached share where the kernel's view has no type
# of its own, and a share no one region describes, each refusal PAIRED WITH A CONTROL.
#
# The module is driven in SCRIPT MODE against the SAME functions the build calls.
#
#   tests/static/check_amp_share.sh <src-dir> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

if [ "$#" -ne 2 ]; then
    fail "usage: check_amp_share.sh <src-dir> <cmake>"
fi
SRC="$(cd "$1" && pwd)" || fail "no source tree at $1"
CMAKE="$2"

MODULE="$SRC/cmake/amp_share.cmake"
[ -f "$MODULE" ] || fail "no module at $MODULE, so every arm below would test nothing"
command -v "$CMAKE" >/dev/null 2>&1 || [ -x "$CMAKE" ] || fail "cmake not executable: $CMAKE"

grep -q 'kickos_amp_share_check' "$SRC/cmake/amp_partition.cmake" \
    || fail "cmake/amp_partition.cmake does not call kickos_amp_share_check, so the refusals
    this gate exercises are wired to nothing"
grep -q 'kickos_amp_share_granule_check' "$SRC/arch/CMakeLists.txt" \
    || fail "arch/CMakeLists.txt does not call kickos_amp_share_granule_check, so the granule
    refusal this gate exercises is wired to nothing"

scratch_dir

DRIVER="$TMP/drive.cmake"
cat >"$DRIVER" <<EOF
list(APPEND CMAKE_MODULE_PATH "$SRC/cmake")
include(amp_share)
kickos_amp_share_check(SHARE "\${AS_SHARE}" WINDOW "\${AS_WINDOW}" UNCACHED "\${AS_UNCACHED}"
                       TRANSLATES "\${AS_TRANSLATES}" ORIGIN "the fixture")
kickos_amp_share_granule_check(SHARE "\${AS_SHARE}" BASE "\${AS_BASE}" MIN_REGION "\${AS_MIN}"
                               POW2 "\${AS_POW2}" ORIGIN "the fixture")
message(STATUS "SHARE-ACCEPTED")
EOF

# <share> <window> <uncached> <translates> <base> <min_region> <pow2>
rc_of() {
    "$CMAKE" "-DAS_SHARE=$1" "-DAS_WINDOW=$2" "-DAS_UNCACHED=$3" "-DAS_TRANSLATES=$4" \
        "-DAS_BASE=$5" "-DAS_MIN=$6" "-DAS_POW2=$7" -P "$DRIVER" >"$TMP/out" 2>&1
}

# <label> <phrase the refusal must name> <rc_of arguments...>
expect_refusal() {
    _label="$1"
    _phrase="$2"
    shift 2
    if rc_of "$@"; then
        fail "$_label: ($*) was ACCEPTED. $(cat "$TMP/out")"
    fi
    if grep -q 'SHARE-ACCEPTED' "$TMP/out"; then
        fail "$_label: the module reached its accept marker despite failing"
    fi
    # cmake wraps a message, so the phrase is matched across its lines.
    tr -s ' \n' '  ' <"$TMP/out" >"$TMP/flat"
    grep -q "$_phrase" "$TMP/flat" && grep -q 'the fixture' "$TMP/flat" \
        || fail "$_label: refused, but not saying why or naming the configuration to fix. Got:
    $(cat "$TMP/out")"
    echo "amp_share: REFUSED $_label ($*)"
}

# <label> <rc_of arguments...>
expect_accept() {
    _label="$1"
    shift
    if ! rc_of "$@"; then
        fail "$_label: ($*) was REFUSED and should not be. $(cat "$TMP/out")"
    fi
    grep -q 'SHARE-ACCEPTED' "$TMP/out" \
        || fail "$_label: exited 0 without reaching the accept marker, so this control asserts
    nothing"
    echo "amp_share: accepted $_label ($*)"
}

NOTHING='leaves the kernel nothing'
expect_refusal "one byte past the window" "$NOTHING" 0x200001 0x200000 OFF ON 0x48000000 0 0
expect_refusal "the whole window" "$NOTHING" 0x200000 0x200000 OFF ON 0x48000000 0 0
# The control: one byte inside, which is the one clause that differs.
expect_accept "one byte short" 0x1FFFFF 0x200000 OFF ON 0x48000001 0 0
expect_accept "the top half" 0x100000 0x200000 OFF ON 0x48100000 0 0
# No share, the default, at any window.
expect_accept "no share" 0x0 0x8000 OFF OFF 0x2007C000 32 0

# The kernel's own view takes the share's type only where it translates.
expect_refusal "uncached on a region unit" 'does not translate' 0x4000 0x8000 ON OFF 0x2007C000 32 0
expect_accept "uncached, translating" 0x200000 0x400000 ON ON 0x48200000 0 0
expect_accept "cached on a region unit" 0x4000 0x8000 OFF OFF 0x2007C000 32 0

# One region of the unit: a granule multiple on a base+limit unit, a power of two on its own
# alignment on a pow2 one.
REGION='no one region'
expect_refusal "a part granule" "$REGION" 0x4010 0x8000 OFF OFF 0x2007BFF0 32 0
expect_accept "whole granules" 0x4020 0x8000 OFF OFF 0x2007BFE0 32 0
expect_refusal "pow2 unit, not a power" "$REGION" 0x3000 0x8000 OFF OFF 0x4087D000 8 1
expect_refusal "pow2 unit, misaligned" "$REGION" 0x4000 0x8000 OFF OFF 0x4087A000 8 1
expect_accept "pow2 unit, aligned power" 0x4000 0x8000 OFF OFF 0x4087C000 8 1

echo "amp_share: OK"
