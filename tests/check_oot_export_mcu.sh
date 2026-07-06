#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for the MCU out-of-tree packaging surface (the sim gate,
# check_oot_export.sh, cannot exercise it): install a bare-metal KickOS package,
# then configure + build a standalone app against it with the SHIPPED cross
# toolchain via find_package(KickOS). Build-only -- an MCU ELF can't run on the
# host -- so it asserts an ARM ELF is produced. Also asserts the single-board
# guard rejects a cross-board request (the failure mode the board.cmake refactor
# had to preserve).
#
# usage: check_oot_export_mcu.sh <kickos-build-dir> <kickos-source-dir> <cmake> <generator> [readelf]

set -eu

KICKOS_BUILD="$1"
KICKOS_SRC="$2"
CMAKE="${3:-cmake}"
GEN="${4:-Ninja}"
READELF="${5:-readelf}"

fail() { echo "FAIL: $1"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "== installing MCU KickOS package to $TMP/prefix =="
"$CMAKE" --install "$KICKOS_BUILD" --prefix "$TMP/prefix" >/dev/null \
  || fail "cmake --install failed"

TC="$TMP/prefix/lib/cmake/KickOS/toolchain-arm-none-eabi.cmake"
[ -f "$TC" ] || fail "shipped ARM toolchain file missing from package"
[ -f "$TMP/prefix/lib/cmake/KickOS/board.cmake" ] \
  || fail "shipped board descriptor (board.cmake) missing from package"

echo "== configuring out-of-tree app with the shipped toolchain (no -DKICKOS_BOARD) =="
"$CMAKE" -S "$KICKOS_SRC/examples/oot-app" -B "$TMP/build" -G "$GEN" \
  -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_PREFIX_PATH="$TMP/prefix" >/dev/null \
  || fail "out-of-tree MCU configure (find_package) failed"

echo "== building out-of-tree app =="
"$CMAKE" --build "$TMP/build" >/dev/null || fail "out-of-tree MCU build failed"

APP="$TMP/build/oot_app"
[ -f "$APP" ] || fail "out-of-tree app ELF not produced"
"$READELF" -h "$APP" | grep -q 'Machine:.*ARM' \
  || fail "out-of-tree app is not an ARM ELF (wrong -mcpu/arch resolved)"

echo "== single-board guard: a cross-board request must be rejected =="
if "$CMAKE" -S "$KICKOS_SRC/examples/oot-app" -B "$TMP/mismatch" -G "$GEN" \
     -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_PREFIX_PATH="$TMP/prefix" \
     -DKICKOS_BOARD=picopi >/dev/null 2>&1; then
  fail "a mismatched -DKICKOS_BOARD=picopi was accepted (single-board guard gone)"
fi

echo "PASS: out-of-tree MCU find_package(KickOS) built an ARM ELF; cross-board request rejected"
