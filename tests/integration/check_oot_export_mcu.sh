#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for the MCU out-of-tree packaging surface: install a bare-metal KickOS package,
# then configure + build a standalone app against it with the SHIPPED cross toolchain via
# find_package(KickOS) + plain add_executable. Build-only, so every assertion is on what
# the build produced.
#
# A bare-metal package is the only one carrying a linker script, a reset vector and a
# flashable image, so this is where the whole bare-metal recipe is proven:
#   - the plain path links at all (the exported target carries the whole recipe);
#   - an edited linker script RELINKS (INTERFACE_LINK_DEPENDS). Passing the script
#     as a -T driver option alone does not create that edge, and the failure is
#     silent: a stale image gets flashed;
#   - kickos_emit_image() gives the plain path its deliverable;
#   - no warning flag reaches the consumer's own TUs on either path (our hygiene
#     policy is not part of the interface);
#   - every installed header compiles standalone with the package's OWN cross
#     compiler and its OWN definitions (check_public_headers.sh);
#   - the single-board guard rejects a cross-board request at find_package time.
#
# NOTHING HERE NAMES AN ARCH. The gate registers on one board per KICKOS_ARCH
# (tests/integration/oot_arch_boards.txt), so the toolchain file, the ELF machine and the
# cross-board name are all read out of the package under test.
#
# TWO DELIVERABLE KINDS, and which one this package has is read off what the build produced,
# not off a name: an ELF the toolchain's own linker wrote, or a PE32+ UEFI application ld
# wrote from objects because a compiler driver has no PE+ emulation. Exactly one of the two
# must appear, so a build that produced neither fails here rather than skipping the arms that
# would have read it.
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

# The provisioning THIS build resolved, read from the header it generated, and handed to the
# child configure so the example can static_assert the installed headers state the same. A
# deleted board_config.h install rule otherwise leaves config/system.h's fleet defaults
# standing and the app compiles against a geometry the libraries do not have.
BOARD_CFG="$KICKOS_BUILD/generated/include/kickos/board_config.h"
[ -f "$BOARD_CFG" ] || fail "no generated board_config.h at $BOARD_CFG"
knob() {
  awk -v k="$1" '$1 == "#define" && $2 == k { print $3; exit }' "$BOARD_CFG"
}
EXPECT_MAX_THREADS="$(knob KICKOS_MAX_THREADS)"
EXPECT_USER_STACK_SIZE="$(knob KICKOS_USER_STACK_SIZE)"
# An empty value passes as a -D nothing defines, and the example's #ifdef then skips the
# assertion in silence.
[ -n "$EXPECT_MAX_THREADS" ] || fail "$BOARD_CFG states no KICKOS_MAX_THREADS"
[ -n "$EXPECT_USER_STACK_SIZE" ] || fail "$BOARD_CFG states no KICKOS_USER_STACK_SIZE"

echo "== installing MCU KickOS package to $TMP/prefix =="
"$CMAKE" --install "$KICKOS_BUILD" --prefix "$TMP/prefix" >/dev/null \
  || fail "cmake --install failed"

# The cross toolchain file, whichever family this package was built for. A package also
# ships the fragments that file include()s, and they are toolchain-*.cmake too; the one a
# consumer names is the one no other shipped file includes. Derived, so a new fragment does
# not have to be named here as well.
is_tc_fragment() { # <path>; 0 when another shipped toolchain file includes it
  _f="$1"
  _bn="$(basename "$_f")"
  for _o in "$TMP"/prefix/lib/cmake/KickOS/toolchain-*.cmake; do
    if [ "$_o" != "$_f" ] && grep -Fq '${CMAKE_CURRENT_LIST_DIR}/'"$_bn" "$_o"; then
      return 0
    fi
  done
  return 1
}
TC=""
for _tc in "$TMP"/prefix/lib/cmake/KickOS/toolchain-*.cmake; do
  [ -f "$_tc" ] || continue
  if is_tc_fragment "$_tc"; then
    continue
  fi
  if [ -n "$TC" ]; then
    fail "package ships several cross toolchain files ($(basename "$TC"), \
$(basename "$_tc")); a consumer cannot tell which one configures it"
  fi
  TC="$_tc"
done
[ -n "$TC" ] || fail "shipped cross toolchain file missing from package"

DESC="$TMP/prefix/lib/cmake/KickOS/board.cmake"
[ -f "$DESC" ] || fail "shipped board descriptor (board.cmake) missing from package"
PKG_BOARD="$(sed -n 's/^set(KICKOS_BOARD_ID *"\{0,1\}\([A-Za-z0-9_-]*\)"\{0,1\}).*/\1/p' \
                 "$DESC" | head -1)"
[ -n "$PKG_BOARD" ] || fail "the shipped board descriptor states no KICKOS_BOARD_ID"

# The arch archive fixes the ISA the libraries were built for, and the app must land on the
# same one. A package that ships the WRONG family's toolchain file resolves a compiler that
# builds a foreign ELF, and every other assertion here still passes.
ARCH_AR=""
for _ar in "$TMP"/prefix/lib/libkickos_arch_*.a; do
  [ -f "$_ar" ] || continue
  if [ -n "$ARCH_AR" ]; then
    fail "package ships several arch archives ($(basename "$ARCH_AR"), $(basename "$_ar"))"
  fi
  ARCH_AR="$_ar"
done
[ -n "$ARCH_AR" ] || fail "no libkickos_arch_*.a in the package: nothing states its ISA"
tool_out "$TMP/ar_hdr" 'Machine:' "$READELF" -h "$ARCH_AR"
WANT_MACHINE="$(sed -n 's/^ *Machine: *//p' "$TMP/ar_hdr" | sort -u)"
[ "$(printf '%s\n' "$WANT_MACHINE" | wc -l | tr -d ' ')" = "1" ] \
  || fail "$(basename "$ARCH_AR") holds members of more than one machine: $WANT_MACHINE"

# Board-agnostic: the package ships exactly one linker script. More than one and the relink
# probe below would patch an arbitrary one, which is not necessarily the one that links. Both
# shipped locations are searched: a script the exported target names as a -T option lands
# beside the archives, and one a build recipe reads list-dir-relative lands beside that recipe.
LD=""
for _ld in "$TMP"/prefix/lib/kickos/*.ld "$TMP"/prefix/lib/cmake/KickOS/*.ld; do
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

CDB="$TMP/build/compile_commands.json"
[ -f "$CDB" ] || fail "no compile_commands.json: neither the consumer's flag posture nor the \
object its own TU became can be read"

# WHICH DELIVERABLE, read off what the build produced. Exactly one of the two, so a build that
# reached no link at all fails here instead of skipping the arms below in silence.
APP="$TMP/build/oot_mcu_app"
EFI="$APP.efi"
KIND=""
if [ -f "$APP" ]; then
  KIND=elf
fi
if [ -f "$EFI" ]; then
  if [ -n "$KIND" ]; then
    fail "the consumer's build produced BOTH $(basename "$APP") and $(basename "$EFI"), so \
which one a board flashes is undecided and the arms below would read an arbitrary one"
  fi
  KIND=pe
fi
[ -n "$KIND" ] || fail "the out-of-tree app produced neither $(basename "$APP") nor \
$(basename "$EFI"): the consumer's build reached no link, so every assertion below would be \
about a package nobody linked against"

# The consumer's OWN compiled object, whatever the deliverable is made of. A package that ships
# the WRONG family's toolchain file resolves a compiler that builds a foreign object, and every
# other assertion here still passes.
APP_OBJ="$(sed -E -n 's/.*"output"[[:space:]]*:[[:space:]]*"([^"]*)".*/\1/p' "$CDB" | head -1)"
[ -n "$APP_OBJ" ] || fail "no \"output\" in $CDB, so the object the consumer's TU became \
cannot be found and the ISA check below would have no subject"
case "$APP_OBJ" in
  /*) ;;
  *) APP_OBJ="$TMP/build/$APP_OBJ" ;;
esac
[ -f "$APP_OBJ" ] || fail "$CDB names $APP_OBJ, which does not exist"
tool_out "$TMP/obj_hdr" 'Machine:' "$READELF" -h "$APP_OBJ"
GOT_MACHINE="$(sed -n 's/^ *Machine: *//p' "$TMP/obj_hdr")"
[ "$GOT_MACHINE" = "$WANT_MACHINE" ] \
  || fail "the out-of-tree app compiled to a '$GOT_MACHINE' object while the packaged \
libraries are '$WANT_MACHINE': the shipped toolchain file resolved a compiler for another \
family"

# Little-endian scalar out of a binary, byte by byte: od is the only reader here, and its
# native-endian -tu forms would read a PE image wrong on a big-endian host.
le_num() { # <file> <byte offset> <width>
  _bytes="$(od -An -tx1 -j "$2" -N "$3" -v "$1" | tr -d ' \n')"
  [ "${#_bytes}" -eq "$(( $3 * 2 ))" ] || fail "od read ${#_bytes} hex digit(s) at offset $2 \
of $1, not $(( $3 * 2 )): the file is shorter than the header this reads"
  _val=0
  _i=0
  while [ "$_i" -lt "$3" ]; do
    _at=$(( _i * 2 + 1 ))
    _val=$(( _val + 0x$(printf '%s' "$_bytes" | cut -c "$_at-$(( _at + 1 ))") * (1 << (8 * _i)) ))
    _i=$(( _i + 1 ))
  done
  printf '%s' "$_val"
}
le_ascii() { # <file> <byte offset> <width>
  od -An -c -j "$2" -N "$3" -v "$1" | tr -d ' \n'
}

echo "== the plain path emits its deliverable =="
if [ "$KIND" = elf ]; then
  [ -f "$TMP/build/oot_mcu_app.bin" ] || fail "kickos_emit_image produced no .bin"
  [ -f "$TMP/build/oot_mcu_app.hex" ] || fail "kickos_emit_image produced no .hex"
  # The linked image, not just the object: a driver that linked the wrong family would still
  # have compiled the right one.
  tool_out "$TMP/app_hdr" 'Machine:' "$READELF" -h "$APP"
  LINKED_MACHINE="$(sed -n 's/^ *Machine: *//p' "$TMP/app_hdr")"
  [ "$LINKED_MACHINE" = "$WANT_MACHINE" ] \
    || fail "the out-of-tree app is a '$LINKED_MACHINE' ELF while the packaged libraries are \
'$WANT_MACHINE'"
else
  # READ THE ARTEFACT, not the build's exit status: a configure that never reached the link is
  # the failure mode this whole gate exists to catch, and a zero-length or truncated file has
  # an exit status of zero behind it. The three fields are what firmware loads the image on.
  [ -s "$EFI" ] || fail "kickos_emit_image produced an empty $EFI"
  [ "$(le_ascii "$EFI" 0 2)" = "MZ" ] || fail "$EFI does not start with MZ, so it is no PE \
image at all"
  PE_AT="$(le_num "$EFI" 60 4)"
  [ "$(le_ascii "$EFI" "$PE_AT" 4)" = 'PE\0\0' ] \
    || fail "$EFI carries no PE signature at the offset its DOS header names ($PE_AT)"
  PE_MAGIC="$(le_num "$EFI" "$(( PE_AT + 24 ))" 2)"
  [ "$PE_MAGIC" -eq 523 ] || fail "$EFI has optional-header magic $PE_MAGIC, not 523 (0x20b): \
it is not a PE32+ image and 64-bit firmware will not load it"
  PE_SUBSYS="$(le_num "$EFI" "$(( PE_AT + 92 ))" 2)"
  [ "$PE_SUBSYS" -eq 10 ] || fail "$EFI declares subsystem $PE_SUBSYS, not 10: firmware takes \
it for something other than an EFI application"
  PE_ENTRY="$(le_num "$EFI" "$(( PE_AT + 40 ))" 4)"
  [ "$PE_ENTRY" -ne 0 ] || fail "$EFI names entry point 0, so firmware would call the image \
base rather than the entry symbol"
  echo "   $(basename "$EFI"): PE32+ EFI application, entry +0x$(printf '%x' "$PE_ENTRY")"
fi

echo "== our warning policy must not reach the consumer's TUs =="
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

echo "== the installed headers compile with the package's own compiler and definitions =="
APP_CXX="$(sed -n 's/^CMAKE_CXX_COMPILER:[^=]*=//p' "$TMP/build/CMakeCache.txt" | head -1)"
APP_CC="$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "$TMP/build/CMakeCache.txt" | head -1)"
[ -x "$APP_CXX" ] || fail "the app's cache names no usable C++ compiler ($APP_CXX)"
[ -x "$APP_CC" ] || fail "the app's cache names no usable C compiler ($APP_CC)"
package_defs "$CDB" "$TMP/defs"
"$(dirname "$0")/../static/check_public_headers.sh" "$TMP/prefix" "$APP_CXX" c++17 "$APP_CC" \
  "$TMP/defs" \
  || fail "the installed headers do not compile at the level the package advertises"

# -T reaches the linker as an opaque driver option and creates no dependency edge on its
# own, so an edited script does not relink and a stale image gets flashed. The probe is a
# top-level absolute symbol, legal anywhere and never changing the layout.
echo "== an edited linker script must relink (not leave a stale image) =="
# The claim is one; where the probe becomes visible is per deliverable.
#   ELF: the image's own symbol table. The readelf run goes through tool_out, so one that read
#        nothing reports the probe absent, which is the expected answer before the edit and
#        the assertion's failure after it.
#   PE:  the -Map file. ld writes no symbol table into a PE32+ image that readelf walks, and
#        the map comes out of the same command as the image, so a link that did not re-run
#        leaves the old map beside the old image.
probe_seen() { # 0 when what the linker last wrote names the probe
  if [ "$KIND" = elf ]; then
    tool_out "$TMP/probe_out" "$READELF_SYM_RE" "$READELF" -sW "$APP"
  else
    [ -s "$EFI.map" ] || fail "no link map beside $EFI, so nothing here can tell a relink \
from a stale image"
    cp "$EFI.map" "$TMP/probe_out"
  fi
  grep -q 'kickos_relink_probe' "$TMP/probe_out"
}
if probe_seen; then
  fail "probe symbol already present before the edit: the check proves nothing"
fi
printf '\n_kickos_relink_probe = 0xDEADBEEF;\n' >> "$LD"
"$CMAKE" --build "$TMP/build" >/dev/null || fail "rebuild after the linker-script edit failed"
probe_seen \
  || fail "an edited linker script did NOT relink the out-of-tree app: a stale \
image would be flashed (INTERFACE_LINK_DEPENDS on the exported target, or a DEPENDS on the \
script in the image command, missing?)"

# Another REAL board, so the refusal is about the mismatch and not about a name no
# descriptor anywhere would resolve.
OTHER_BOARD=picopi
if [ "$PKG_BOARD" = "$OTHER_BOARD" ]; then
  OTHER_BOARD=qemu
fi
echo "== single-board guard: a cross-board request must be rejected =="
if "$CMAKE" -S "$KICKOS_SRC/examples/oot-mcu-app" -B "$TMP/mismatch" -G "$GEN" \
     -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_PREFIX_PATH="$TMP/prefix" \
     -DKICKOS_BOARD="$OTHER_BOARD" >"$TMP/mismatch.log" 2>&1; then
  fail "a mismatched -DKICKOS_BOARD=$OTHER_BOARD was accepted (single-board guard gone)"
fi
# A configure can fail for a dozen reasons. One of the plausible ones, a toolchain file that
# resolves no CPU baseline, names the REQUESTED board too; only the guard names BOTH, so that
# is the discriminator, and it survives a rewording of the message.
if ! grep -q "$PKG_BOARD" "$TMP/mismatch.log" || ! grep -q "$OTHER_BOARD" "$TMP/mismatch.log"; then
  fail "the cross-board configure failed without naming both the packaged board \
($PKG_BOARD) and the requested one ($OTHER_BOARD), so it failed for some other reason than \
the single-board guard and this arm witnesses nothing: \
$(sed -n '1,3p' "$TMP/mismatch.log" | tr '\n' ' ')"
fi

echo "PASS: out-of-tree MCU app built for $WANT_MACHINE, imaged as $KIND and relinked; \
headers compile; no flags leaked; cross-board rejected"
