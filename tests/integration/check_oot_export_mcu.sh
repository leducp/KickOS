#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for the MCU out-of-tree packaging surface (the sim gate,
# check_oot_export.sh, cannot exercise it): install a bare-metal KickOS package,
# then configure + build a standalone app against it with the SHIPPED cross
# toolchain via find_package(KickOS) + plain add_executable. Build-only: an MCU
# ELF can't run on the host, so it asserts on what the build produced.
#
# This is where the interesting machinery lives. The sim package has no linker
# script, no reset vector and no flashable image, so everything the bare-metal
# recipe adds is proven here or nowhere:
#   - the plain path links at all (the exported target carries the whole recipe);
#   - an edited linker script RELINKS (INTERFACE_LINK_DEPENDS). Passing the script
#     as a -T driver option alone does not create that edge, and the failure is
#     silent: a stale image gets flashed;
#   - kickos_emit_image() gives the plain path its .bin/.hex;
#   - the optional kickos_add_application() wrapper produces the SAME image, so
#     the sugar never becomes the path that works while the plain one does not;
#   - no warning flag reaches the consumer's own TUs on either path (our hygiene
#     policy is not part of the interface);
#   - the single-board guard rejects a cross-board request at find_package time.
#
# usage: check_oot_export_mcu.sh <kickos-build-dir> <kickos-source-dir> <cmake> <generator> [readelf]

set -eu
. "$(dirname "$0")/../lib/gate.sh"

# readelf's headings ("Machine:") are translated under any other locale, and they are
# parsed below.
export LC_ALL=C

# readelf -sW numbers every symbol-table row; a file it could not read has no row.
READELF_SYM_RE='^ *[0-9]+: '

KICKOS_BUILD="$1"
KICKOS_SRC="$2"
CMAKE="${3:-cmake}"
GEN="${4:-Ninja}"
READELF="${5:-readelf}"

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

echo "== installing MCU KickOS package to $TMP/prefix =="
"$CMAKE" --install "$KICKOS_BUILD" --prefix "$TMP/prefix" >/dev/null \
  || fail "cmake --install failed"

TC="$TMP/prefix/lib/cmake/KickOS/toolchain-arm-none-eabi.cmake"
[ -f "$TC" ] || fail "shipped ARM toolchain file missing from package"
[ -f "$TMP/prefix/lib/cmake/KickOS/board.cmake" ] \
  || fail "shipped board descriptor (board.cmake) missing from package"

# Board-agnostic: the package ships exactly one <chip>.ld. More than one and the relink
# probe below would patch an arbitrary one, which is not necessarily the one that links.
LD=""
for _ld in "$TMP"/prefix/lib/kickos/*.ld; do
  [ -f "$_ld" ] || continue
  if [ -n "$LD" ]; then
    fail "package ships several linker scripts ($(basename "$LD"), $(basename "$_ld")); \
the relink probe cannot tell which one the link uses"
  fi
  LD="$_ld"
done
[ -n "$LD" ] || fail "shipped linker script missing from package"

echo "== configuring out-of-tree MCU app with the shipped toolchain (no -DKICKOS_BOARD) =="
"$CMAKE" -S "$KICKOS_SRC/examples/oot-mcu-app" -B "$TMP/build" -G "$GEN" \
  -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_PREFIX_PATH="$TMP/prefix" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  -DKICKOS_EXPECT_MAX_THREADS="$EXPECT_MAX_THREADS" \
  -DKICKOS_EXPECT_USER_STACK_SIZE="$EXPECT_USER_STACK_SIZE" >/dev/null \
  || fail "out-of-tree MCU configure (find_package) failed"

echo "== building out-of-tree MCU app =="
"$CMAKE" --build "$TMP/build" >/dev/null || fail "out-of-tree MCU build failed"

# The plain add_executable target is the one that must work; the _sugar target is
# the same source through kickos_add_application().
APP="$TMP/build/oot_mcu_app"
[ -f "$APP" ] || fail "plain add_executable target produced no ELF"
"$READELF" -h "$APP" | grep -q 'Machine:.*ARM' \
  || fail "out-of-tree app is not an ARM ELF (wrong -mcpu/arch resolved)"

echo "== the plain path emits a flashable image =="
[ -f "$TMP/build/oot_mcu_app.bin" ] || fail "kickos_emit_image produced no .bin"
[ -f "$TMP/build/oot_mcu_app.hex" ] || fail "kickos_emit_image produced no .hex"

echo "== the optional wrapper produces the same image, not a better one =="
[ -f "$TMP/build/oot_mcu_app_sugar.bin" ] \
  || fail "kickos_add_application() target produced no .bin"
# Compared by size, not byte-for-byte: app.h bakes __DATE__/__TIME__ into every app
# TU, so two targets whose compiles straddle a second boundary differ in those fixed
# -width bytes for reasons that say nothing about the link recipe. The length is
# immune to that and still catches the thing worth catching: the wrapper linking
# in something, or with something, that the plain path does not get.
SZ_PLAIN=$(wc -c < "$TMP/build/oot_mcu_app.bin")
SZ_SUGAR=$(wc -c < "$TMP/build/oot_mcu_app_sugar.bin")
[ "$SZ_PLAIN" = "$SZ_SUGAR" ] \
  || fail "plain add_executable ($SZ_PLAIN B) and kickos_add_application() \
($SZ_SUGAR B) images differ in size; the plain path is missing something the \
wrapper supplies"

echo "== our warning policy must not reach the consumer's TUs =="
CDB="$TMP/build/compile_commands.json"
if [ -f "$CDB" ]; then
  # One argument per line, matched whole: a -W inside a path or attached to a -D value is
  # then not an argument position. POSIX ERE only, because \b is a GNU extension.
  tr '[:space:]' '\n' < "$CDB" > "$TMP/cdb_args"
  LEAK_RE='"?-W(all|extra|shadow|undef|error)(=[^"]*)?"?,?'
  if grep -qxE "$LEAK_RE" "$TMP/cdb_args"; then
    grep -xE "$LEAK_RE" "$TMP/cdb_args" | sort -u
    fail "KickOS warning flags leaked onto an out-of-tree consumer's compile line"
  fi
else
  fail "no compile_commands.json: cannot check the consumer's flag posture"
fi

# The regression gate for the stale-image bug. -T reaches the linker as an opaque
# driver option, so on its own it creates no dependency edge and an edited script
# does not relink. Append a top-level absolute symbol (always legal, never changes
# the layout) and require it to appear in the rebuilt ELF.
echo "== an edited linker script must relink (not leave a stale image) =="
# Through tool_out both times: a readelf that reads nothing reports the probe absent,
# which is the "correct" answer before the edit and the assertion's own failure after it.
tool_out "$TMP/syms_before" "$READELF_SYM_RE" "$READELF" -sW "$APP"
if grep -q 'kickos_relink_probe' "$TMP/syms_before"; then
  fail "probe symbol already present before the edit: the check proves nothing"
fi
printf '\n_kickos_relink_probe = 0xDEADBEEF;\n' >> "$LD"
"$CMAKE" --build "$TMP/build" >/dev/null || fail "rebuild after the .ld edit failed"
tool_out "$TMP/syms_after" "$READELF_SYM_RE" "$READELF" -sW "$APP"
grep -q 'kickos_relink_probe' "$TMP/syms_after" \
  || fail "an edited linker script did NOT relink the out-of-tree app: a stale \
image would be flashed (INTERFACE_LINK_DEPENDS missing from the exported target?)"

echo "== single-board guard: a cross-board request must be rejected =="
if "$CMAKE" -S "$KICKOS_SRC/examples/oot-mcu-app" -B "$TMP/mismatch" -G "$GEN" \
     -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_PREFIX_PATH="$TMP/prefix" \
     -DKICKOS_BOARD=picopi >/dev/null 2>&1; then
  fail "a mismatched -DKICKOS_BOARD=picopi was accepted (single-board guard gone)"
fi

echo "PASS: out-of-tree MCU plain add_executable built, imaged and relinked; wrapper agrees; no flags leaked; cross-board rejected"
