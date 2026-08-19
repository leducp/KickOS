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
. "$(dirname "$0")/../lib/gate.sh"

KICKOS_BUILD="$1"
KICKOS_SRC="$2"
CMAKE="${3:-cmake}"
GEN="${4:-Ninja}"

scratch_dir

# The provisioning THIS build resolved, read from the header it generated. Handed to the
# child configure so the example can static_assert the installed headers state the same:
# without that, a deleted board_config.h install rule leaves config/system.h's fleet
# defaults standing and the app compiles against a geometry the libraries do not have.
BOARD_CFG="$KICKOS_BUILD/generated/include/kickos/board_config.h"
[ -f "$BOARD_CFG" ] || fail "no generated board_config.h at $BOARD_CFG"
knob() {
  awk -v k="$1" '$1 == "#define" && $2 == k { print $3; exit }' "$BOARD_CFG"
}
EXPECT_MAX_THREADS="$(knob KICKOS_MAX_THREADS)"
EXPECT_USER_STACK_SIZE="$(knob KICKOS_USER_STACK_SIZE)"
# An empty value would be passed as a -D nothing defines, and the example's #ifdef would
# then skip the assertion in silence.
[ -n "$EXPECT_MAX_THREADS" ] || fail "$BOARD_CFG states no KICKOS_MAX_THREADS"
[ -n "$EXPECT_USER_STACK_SIZE" ] || fail "$BOARD_CFG states no KICKOS_USER_STACK_SIZE"

echo "== installing KickOS package to $TMP/prefix =="
"$CMAKE" --install "$KICKOS_BUILD" --prefix "$TMP/prefix" >/dev/null \
  || fail "cmake --install failed"

# The package ADVERTISES cxx_std_17 while the kernel is built at C++20, so every header
# it ships has to compile at the advertised level. The fourth argument is the C compiler for
# the C-facing half of the same package.
"$(dirname "$0")/../static/check_public_headers.sh" "$TMP/prefix" "${CXX:-g++}" c++17 "${CC:-gcc}" \
  || fail "the installed headers do not compile at the level the package advertises"


echo "== configuring out-of-tree app via find_package(KickOS) =="
"$CMAKE" -S "$KICKOS_SRC/examples/oot-app" -B "$TMP/build" -G "$GEN" \
  -DCMAKE_PREFIX_PATH="$TMP/prefix" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DKICKOS_EXPECT_MAX_THREADS="$EXPECT_MAX_THREADS" \
  -DKICKOS_EXPECT_USER_STACK_SIZE="$EXPECT_USER_STACK_SIZE" >/dev/null \
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
  fail "no compile_commands.json: cannot check the consumer's flag posture"
fi

echo "== running out-of-tree app =="
OUT="$("$APP")" || fail "out-of-tree app exited non-zero"
echo "$OUT"
echo "$OUT" | grep -q '\[oot\] hello from an out-of-tree KickOS app' \
  || fail "out-of-tree app did not run correctly"

echo "PASS: out-of-tree find_package(KickOS) build ran"
