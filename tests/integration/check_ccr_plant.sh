#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# What a boot ROM leaves in CCR does not reach a thread: a test-only build (-DKICKOS_CCR_PLANT)
# sets UNALIGN_TRP and USERSETMPEND and clears DIV_0_TRP before bring-up (`boot`), and main,
# unprivileged, then probes each: an unaligned halfword store to its stack must complete, a write
# to the STIR must fault rather than pend a line, and a hardware divide by zero must fault as one
# (UFSR.DIVBYZERO, CFSR bit 25) rather than return 0.
#
# The `kept` plant inverts them again after bring-up, as a kernel that wrote none would run: there
# each probe must FAIL its verdict, or the verdict holds without the write and witnesses nothing.
#
# The plant is a build-wide option, so each posture configures its own tree. The option is
# refused without the marker only this script passes, and the control below holds that refusal.
#
# usage: QEMU_MACHINE=<machine> check_ccr_plant.sh <kickos-source-dir> <cmake> <preset>
set -u
. "$(dirname "$0")/../lib/gate.sh"
KICKOS_SRC="$1"
CMAKE="$2"
PRESET="$3"
need_qemu_machine
scratch_dir
MARKER=check_ccr_plant.sh

if ( cd "$KICKOS_SRC" && "$CMAKE" --preset "$PRESET" -S "$KICKOS_SRC" -B "$TMP/refused" \
       -DKICKOS_CCR_PLANT=boot -DKICKOS_BUILD_TESTS=OFF >"$TMP/refused.log" 2>&1 ); then
    fail "a configure with KICKOS_CCR_PLANT and no marker was accepted"
fi
if ! grep -q 'CCR_PLANT_REFUSED' "$TMP/refused.log"; then
    sed -n '1,30p' "$TMP/refused.log" >&2
    fail "the unmarked configure failed, but not on the refusal"
fi

# <plant>: builds both probes in a tree of their own.
build_plant() {
    _b="$TMP/$1"
    ( cd "$KICKOS_SRC" && "$CMAKE" --preset "$PRESET" -S "$KICKOS_SRC" -B "$_b" \
        -DKICKOS_CCR_PLANT="$1" -DKICKOS_CCR_PLANT_SCRATCH="$MARKER" \
        -DKICKOS_BUILD_TESTS=OFF >"$_b.cfg.log" 2>&1 ) \
      || fail "$1: configure failed, see $_b.cfg.log"
    "$CMAKE" --build "$_b" --target ccrprobe_unaligned ccrprobe_stir ccrprobe_div0 \
        >"$_b.build.log" 2>&1 \
      || fail "$1: build failed, see $_b.build.log"
}

# <probe>: empty when OUT is that probe's verdict met, the first refused clause otherwise.
verdict() {
    if ! has_f "[ccrprobe] npriv 1"; then
        echo "main did not run unprivileged"
        return
    fi
    if [ "$1" = unaligned ]; then
        if ! has_f "[ccrprobe] unaligned ok 0x3020"; then
            echo "the unaligned store did not complete"
            return
        fi
        if has_f "=== THREAD FAULT" || [ "$RC" -ne 0 ]; then
            echo "main did not end cleanly (exit $RC)"
            return
        fi
        return
    fi
    if [ "$1" = div0 ]; then
        if ! has_f "[ccrprobe] div0"; then
            echo "main never reached the divide"
            return
        fi
        if has_f "[ccrprobe] div0 returned"; then
            echo "the divide by zero returned a value"
            return
        fi
        _cfsr="$(printf '%s\n' "$OUT" | sed -n 's/.*CFSR=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
        if ! has_f "=== THREAD FAULT === thread 'main' killed" || [ -z "$_cfsr" ] \
           || [ $(( 0x$_cfsr & 0x2000000 )) -eq 0 ]; then
            echo "the divide did not end main in a UsageFault DIVBYZERO (CFSR=0x$_cfsr)"
            return
        fi
        return
    fi
    if ! has_f "[ccrprobe] stir write"; then
        echo "main never reached the STIR write"
        return
    fi
    if has_f "[ccrprobe] stir landed"; then
        echo "the unprivileged STIR write landed"
        return
    fi
    _cfsr="$(printf '%s\n' "$OUT" | sed -n 's/.*CFSR=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
    if ! has_f "=== THREAD FAULT === thread 'main' killed" || [ -z "$_cfsr" ] \
       || [ $(( 0x$_cfsr & 0xFF00 )) -eq 0 ]; then
        echo "the STIR write did not end main in a BusFault (CFSR=0x$_cfsr)"
        return
    fi
}

for plant in boot kept; do
    build_plant "$plant"
    for probe in unaligned stir div0; do
        echo "== $plant, $probe =="
        run_image "$TMP/$plant/user/apps/common/ccrprobe/ccrprobe_$probe" >/dev/null
        why="$(verdict "$probe")"
        if [ "$plant" = boot ] && [ -n "$why" ]; then
            printf '%s\n' "$OUT" | tail -n 20
            fail "$probe: $why"
        fi
        if [ "$plant" = kept ] && [ -z "$why" ]; then
            printf '%s\n' "$OUT" | tail -n 20
            fail "$probe: the verdict holds with the plant kept, so it witnesses nothing"
        fi
    done
done
echo "PASS: after a bring-up that found CCR inverted, an unprivileged unaligned store completes, an unprivileged STIR write faults and a hardware divide by zero faults as one, and none does with the plant kept"
exit 0
