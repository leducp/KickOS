#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for the dependency-inversion acceptance criterion: install the
# KickOS sim package, then configure + build + run a standalone out-of-tree
# app against it via find_package(KickOS) + kickos_add_application().
#
# usage: check_oot_export.sh <kickos-build-dir> <kickos-source-dir> <cmake> <generator>

set -eu

KICKOS_BUILD="$1"
KICKOS_SRC="$2"
CMAKE="${3:-cmake}"
GEN="${4:-Ninja}"

fail() { echo "FAIL: $1"; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "== installing KickOS package to $TMP/prefix =="
"$CMAKE" --install "$KICKOS_BUILD" --prefix "$TMP/prefix" >/dev/null \
  || fail "cmake --install failed"

echo "== configuring out-of-tree app via find_package(KickOS) =="
"$CMAKE" -S "$KICKOS_SRC/examples/oot-app" -B "$TMP/build" -G "$GEN" \
  -DCMAKE_PREFIX_PATH="$TMP/prefix" >/dev/null \
  || fail "out-of-tree configure (find_package) failed"

echo "== building out-of-tree app =="
"$CMAKE" --build "$TMP/build" >/dev/null || fail "out-of-tree build failed"

APP="$TMP/build/oot_app"
[ -x "$APP" ] || fail "out-of-tree app binary not produced"

echo "== running out-of-tree app =="
OUT="$("$APP")" || fail "out-of-tree app exited non-zero"
echo "$OUT"
echo "$OUT" | grep -q '\[oot\] hello from an out-of-tree KickOS app' \
  || fail "out-of-tree app did not run correctly"

echo "PASS: out-of-tree find_package(KickOS) build ran"
