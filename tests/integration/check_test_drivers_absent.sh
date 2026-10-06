#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A build that leaves KICKOS_TEST_DRIVERS off ships none of tests/drivers: its manifest's
# catalogue names none of them, and its install carries none of their archives or exports.
# The same three matchers must first hit the build's first catalogue driver, or, in a build
# whose catalogue is empty, the kernel's archive and export.
#
#   check_test_drivers_absent.sh <kickos-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

USAGE="usage: check_test_drivers_absent.sh <kickos-build> <kickos-source> <cmake>"
KICKOS_BUILD="${1:?$USAGE}"
KICKOS_SRC="${2:?$USAGE}"
CMAKE="${3:?$USAGE}"

NAMES="$(sed -n 's/^ *kickos_add_driver(\([a-z0-9_]*\) .*/\1/p' "$KICKOS_SRC/tests/drivers/CMakeLists.txt")"
[ -n "$NAMES" ] || fail "tests/drivers/CMakeLists.txt declares no driver"
MANIFEST="$KICKOS_BUILD/export/manifest.yaml"
[ -f "$MANIFEST" ] || fail "no manifest at $MANIFEST"

scratch_dir
"$CMAKE" --install "$KICKOS_BUILD" --prefix "$TMP/prefix" >/dev/null || fail "cmake --install failed"
[ -d "$TMP/prefix/lib/cmake/KickOS" ] || fail "the install carries no KickOS package"

catalogued() { # <name>
    sed -n '/^drivers:/,/^[^ ]/p' "$MANIFEST" | grep -q "^  $1:"
}
archived() { # <name>
    [ -n "$(find "$TMP/prefix" -name "libkickos_$1.a" -print)" ]
}
exported() { # <name>
    grep -rqw "kickos_$1" "$TMP/prefix/lib/cmake/KickOS"
}

CONTROL="$(sed -n '/^drivers:/,/^[^ ]/s/^  \([a-z0-9_]*\):$/\1/p' "$MANIFEST" | head -n 1)"
if [ -n "$CONTROL" ]; then
    catalogued "$CONTROL" || fail "the catalogue matcher misses catalogue driver $CONTROL"
else
    CONTROL=kernel
fi
archived "$CONTROL" || fail "the archive matcher misses libkickos_$CONTROL.a"
exported "$CONTROL" || fail "the export matcher misses kickos_$CONTROL"

for _n in $NAMES; do
    if catalogued "$_n"; then
        fail "the manifest's catalogue names test driver $_n"
    fi
    if archived "$_n"; then
        fail "the install carries test driver $_n's archive"
    fi
    if exported "$_n"; then
        fail "the installed package exports test driver $_n"
    fi
done
echo "PASS: neither the manifest nor the install carries a test driver ($(printf '%s\n' "$NAMES" | paste -sd ' ' -)), \
and the matchers hit $CONTROL"
