#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The terse diagnostic column ON THE WIRE. Configures a tree of its own with
# KICKOS_DIAG_TERSE forced on, builds the two `panicgate` images whose text the kernel
# supplies, and requires the short code with the prose absent.
#
# THE EXPECTED LINE ARRIVES AS A LITERAL AND IS NEVER DERIVED FROM THE KNOB. A tree where
# the knob reaches neither the compiler nor CMake resolves the prose on both sides and so
# agrees with itself: an expectation computed from the knob passes there, over the very
# posture it was written to witness.
#
# The boards whose flash budget selects this posture carry no emulator, so the witness rides
# the emulated part with the same core; the caller's board predicate says which one.
#
# usage: check_qemu_diag_terse.sh <elf, unused> <cmake> <src> <generator> <toolchain>
#                                 <build-type> <board> <variant> <terse-line> <prose-line>

set -u
here="$(dirname "$0")"
. "$here/../lib/gate.sh"

# kickos_add_qemu_test hands every gate the image THIS tree built; the images read below are
# the ones built under the forced knob, so the argument is accepted and dropped.
_unused_elf="${1:?no image argument; see the usage line above}"
CMAKE="${2:?no cmake}"
SRC="${3:?no source dir}"
GEN="${4:?no generator}"
TC="${5:?no toolchain file}"
BUILD_TYPE="${6:?no build type}"
BOARD="${7:?no board}"
VARIANT="${8:?no variant}"
TERSE="${9:?no terse line}"
shift 9
PROSE="${1:?no prose line}"
: "$_unused_elf"

# Both spellings have to be real and different, or the absence leg below is asserting nothing.
[ -n "$TERSE" ] || fail "the terse line is empty"
[ -n "$PROSE" ] || fail "the prose line is empty"
[ "$TERSE" != "$PROSE" ] || fail "both columns spell '$TERSE', so nothing here discriminates"

# Before the cross build, not after: a box with no emulator owes no configure.
need_qemu_machine
need_qemu

scratch_dir

echo "== configuring $BOARD/$VARIANT with KICKOS_DIAG_TERSE=1 =="
"$CMAKE" -S "$SRC" -B "$TMP/build" -G "$GEN" \
    -DCMAKE_TOOLCHAIN_FILE="$TC" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DKICKOS_BOARD="$BOARD" \
    -DKICKOS_CONFIG_VARIANT="$VARIANT" \
    -DKICKOS_DIAG_TERSE=1 >"$TMP/cfg.log" 2>&1 \
  || { sed -n '1,40p' "$TMP/cfg.log" >&2
       fail "configure with -DKICKOS_DIAG_TERSE=1 was refused (the cross toolchain comes from the environment)"; }

# Cases 2 and 3 are the two whose text the KERNEL supplies, so the two the knob rewrites.
for _case in 2 3; do
    echo "== building panicgate$_case under the forced knob =="
    "$CMAKE" --build "$TMP/build" --target "panicgate$_case" >"$TMP/build$_case.log" 2>&1 \
      || { sed -n '1,40p' "$TMP/build$_case.log" >&2
           fail "panicgate$_case did not build under KICKOS_DIAG_TERSE=1"; }
done

for _case in 2 3; do
    _img="$TMP/build/user/apps/common/panicgate/panicgate$_case"
    [ -x "$_img" ] || fail "no panicgate$_case image at $_img"
    echo "== booting panicgate$_case =="
    # The same reader the board's own cases ride, with the columns swapped: the short code is
    # the expected line and the prose is the literal that must not appear. Its status travels
    # out whole, so a QEMU-less box still reports 77 as a skip rather than as a verdict.
    "$here/check_qemu_panicgate.sh" "$_img" "$TERSE" "$PROSE"
    _rc=$?
    if [ "$_rc" -ne 0 ]; then
        echo "FAIL: panicgate$_case under KICKOS_DIAG_TERSE=1 did not read as the terse column" >&2
        exit "$_rc"
    fi
done

echo "PASS: '$TERSE' on panicgate 2 and 3 with '$PROSE' absent, under a forced KICKOS_DIAG_TERSE=1"
exit 0
