#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for the dependency-inversion acceptance criterion: install the
# KickOS sim package, then configure + build + run a standalone out-of-tree app
# against it via find_package(KickOS) + plain add_executable, linking the
# exported `kickos` usage target. No KickOS-specific macro is involved, which is
# the point: that is the supported downstream shape, so it is what gets gated.
# (The MCU half of the same criterion is check_oot_export_mcu.sh, which covers
# the bare-metal machinery this package has none of.)
#
# usage: check_oot_export.sh <kickos-build-dir> <kickos-source-dir> <cmake> <generator>

set -eu
. "$(dirname "$0")/lib/gate.sh"

KICKOS_BUILD="$1"
KICKOS_SRC="$2"
CMAKE="${3:-cmake}"
GEN="${4:-Ninja}"

scratch_dir

echo "== installing KickOS package to $TMP/prefix =="
"$CMAKE" --install "$KICKOS_BUILD" --prefix "$TMP/prefix" >/dev/null \
  || fail "cmake --install failed"

echo "== configuring out-of-tree app via find_package(KickOS) =="
"$CMAKE" -S "$KICKOS_SRC/examples/oot-app" -B "$TMP/build" -G "$GEN" \
  -DCMAKE_PREFIX_PATH="$TMP/prefix" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null \
  || fail "out-of-tree configure (find_package) failed"

echo "== building out-of-tree app =="
"$CMAKE" --build "$TMP/build" >/dev/null || fail "out-of-tree build failed"

APP="$TMP/build/oot_app"
[ -x "$APP" ] || fail "out-of-tree app binary not produced"

# Our warning flags are this project's hygiene policy, not part of the interface:
# a consumer's own diagnostics are their call, and ours can contradict theirs or
# simply break their build. Nothing on the exported targets may carry one.
echo "== our warning policy must not reach the consumer's TUs =="
CDB="$TMP/build/compile_commands.json"
if [ -f "$CDB" ]; then
  # Anchored on a leading space so -Wl,... link options do not false-trip.
  if grep -qE ' -W(all|extra|shadow|undef|error)\b' "$CDB"; then
    grep -oE ' -W(all|extra|shadow|undef|error)\b' "$CDB" | sort -u
    fail "KickOS warning flags leaked onto an out-of-tree consumer's compile line"
  fi
else
  fail "no compile_commands.json -- cannot check the consumer's flag posture"
fi

echo "== running out-of-tree app =="
OUT="$("$APP")" || fail "out-of-tree app exited non-zero"
echo "$OUT"
echo "$OUT" | grep -q '\[oot\] hello from an out-of-tree KickOS app' \
  || fail "out-of-tree app did not run correctly"

echo "PASS: out-of-tree find_package(KickOS) build ran"
