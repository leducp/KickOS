#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Later passes of one exception against the frame the core stands on. A test-only build
# (-DKICKOS_WITNESS_REPASS) adds two things to a doorbell pass that took a core off the thread it
# interrupted, each once: off a slain thread, a second pass; off the core's idle, a switch booked
# straight back onto it. This gate boots `slaypeer` from that kernel: its victim's spawn onto an
# idle peer core takes the second, and its slay while it spins there at the unprivileged level
# takes the first. The victim's slay exit prints the plant's line once both have fired.
#
# The run must end as slaypeer's own gate requires. Each of two faults turns it red:
#   - a second pass that re-picks the victim and rebuilds the frame the handler stands on: the
#     trap returns into the rebuild and the kernel panics;
#   - an exit that sees the switch back as no switch and so never ends the lock span: the idle
#     core keeps the kernel lock, the slayer's core spins on it, and the slay times out or the
#     run hangs.
# Both are x86_64's: elsewhere every exit ends the span for any booked switch, and a rebuilt
# unprivileged victim's interrupt frame happens to resume cleanly.
#
# The knob is refused without the marker only this script passes. The control below holds that
# refusal.
#
# usage: check_repass.sh <kickos-source-dir> <cmake> <preset> <arch> <scratch-dir>
set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

_usage="usage: check_repass.sh <kickos-source-dir> <cmake> <preset> <arch> <scratch-dir>"
SRC="${1:?$_usage}"
CMAKE="${2:?$_usage}"
PRESET="${3:?$_usage}"
ARCH="${4:?$_usage}"
SCRATCH="${5:?$_usage}"
need_qemu_machine
scratch_dir
MARKER=check_repass.sh

if "$CMAKE" -S "$SRC" -B "$TMP/refused" --preset "$PRESET" -DKICKOS_WITNESS_REPASS=ON \
       -DKICKOS_BUILD_TESTS=OFF > "$TMP/refused.log" 2>&1; then
    fail "a configure with KICKOS_WITNESS_REPASS and no marker was accepted"
fi
if ! grep -q 'WITNESS_REPASS_REFUSED' "$TMP/refused.log"; then
    sed -n '1,30p' "$TMP/refused.log" >&2
    fail "the unmarked configure failed, but not on the refusal"
fi

# Kept under the caller's build directory, with tests on: slaypeer is a test image. A cache that names another source tree, or lacks the
# knob, is not this witness's kernel.
_cache="$SCRATCH/CMakeCache.txt"
if ! { [ -f "$_cache" ] && grep -q '^KICKOS_WITNESS_REPASS:BOOL=ON$' "$_cache" \
       && grep -q '^KICKOS_BUILD_TESTS:BOOL=ON$' "$_cache" \
       && grep -qxF -e "KICKOS_WITNESS_REPASS_SCRATCH:UNINITIALIZED=$MARKER" "$_cache" \
       && grep -qxF -e "CMAKE_HOME_DIRECTORY:INTERNAL=$SRC" "$_cache"; }; then
    rm -rf "$SCRATCH"
    "$CMAKE" -S "$SRC" -B "$SCRATCH" --preset "$PRESET" -DKICKOS_WITNESS_REPASS=ON \
        -DKICKOS_WITNESS_REPASS_SCRATCH="$MARKER" > "$TMP/cfg.log" 2>&1 \
        || { sed -n '1,40p' "$TMP/cfg.log" >&2; fail "configure of the witness kernel failed for $PRESET"; }
fi
"$CMAKE" --build "$SCRATCH" --target slaypeer > "$TMP/build.log" 2>&1 \
    || { grep -E 'error' "$TMP/build.log" | sed -n '1,20p' >&2; fail "build of slaypeer failed in $SCRATCH"; }

# x86_64 links a PE32+ application, which carries its suffix.
_suffix=""
if [ "$ARCH" = x86_64 ]; then
    _suffix=.efi
fi
run_image "$SCRATCH/user/apps/common/slaypeer/slaypeer$_suffix"

if has "ERROR"; then
    printf '%s\n' "$OUT" | tail -n 20
    fail "slaypeer reported a failed setup"
fi
assert_no_panic "the exception's later pass rebuilt the frame its handler stands on"
require_on_wire "[repass] claimed after a second pass and a switch back" \
    "the plant did not fire twice before the victim was claimed (a run that hangs here is a kernel lock the switch back never released)"
require_on_wire "slay rc: 0" "the slay answered something other than 0, -110 being a kernel lock the switch back kept (or the run never reached it)"
require_on_wire "SLAYPEER PASS" "the run never reached PASS"
status_clause "the system exited 0 once main returned past PASS" 0
echo "PASS: a second pass left the live frame alone and a switch back ended the lock span"
exit 0
