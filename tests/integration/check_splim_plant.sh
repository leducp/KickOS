#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The stack limits in PRIVILEGED code, which no application can reach: a test-only build
# (-DKICKOS_SPLIM_PLANT) recurses off the main stack at boot (`msp`), or off a thread's kernel
# block inside its first syscall (`kblock`). Each must end in the panic dump with STKOF on the
# stack it overflowed, never in a thread kill. The main-stack case is the naked handler moving
# the report to an array of its own, MSPLIM at its base; the kernel-block case is the premise
# the SVC guard's 184 rests on, that a violation in privileged code is the kernel's.
#
# The main-stack case also lays a canary over the arena's top, just below that stack's base, and
# the report checks it from its array before it overflows that array too: the canary must be
# intact, and the second overflow must end the system with no second dump.
#
# Each case runs twice, the second time with the plant lifting the live limit first: that run
# must FAIL the same verdict, or the verdict holds without the limit and witnesses nothing.
#
# Each plant is a build-wide option, so each run configures its own tree (as
# check_sim_drvdeath.sh does). The option is refused without the marker only this script
# passes, and the control below holds that refusal.
#
# usage: QEMU_MACHINE=<machine> check_splim_plant.sh <kickos-source-dir> <cmake> <preset>
set -u
. "$(dirname "$0")/../lib/gate.sh"
KICKOS_SRC="$1"
CMAKE="$2"
PRESET="$3"
need_qemu_machine
scratch_dir
MARKER=check_splim_plant.sh

if ( cd "$KICKOS_SRC" && "$CMAKE" --preset "$PRESET" -S "$KICKOS_SRC" -B "$TMP/refused" \
       -DKICKOS_SPLIM_PLANT=msp >"$TMP/refused.log" 2>&1 ); then
    fail "a configure with KICKOS_SPLIM_PLANT and no marker was accepted"
fi
if ! grep -q 'Only tests/integration/check_splim_plant.sh configures' "$TMP/refused.log"; then
    sed -n '1,30p' "$TMP/refused.log" >&2
    fail "the unmarked configure failed, but not on the refusal"
fi

# <plant> <unlimited ON|OFF>: boots hello from a tree built with the plant, into OUT and RC.
boot_plant() {
    _b="$TMP/$1-$2"
    ( cd "$KICKOS_SRC" && "$CMAKE" --preset "$PRESET" -S "$KICKOS_SRC" -B "$_b" \
        -DKICKOS_SPLIM_PLANT="$1" -DKICKOS_SPLIM_PLANT_UNLIMITED="$2" \
        -DKICKOS_SPLIM_PLANT_SCRATCH="$MARKER" >"$_b.cfg.log" 2>&1 ) \
      || fail "$1: configure failed, see $_b.cfg.log"
    "$CMAKE" --build "$_b" --target hello >"$_b.build.log" 2>&1 \
      || fail "$1: build failed, see $_b.build.log"
    run_image "$_b/user/apps/common/hello/hello"
}

# <stack>: empty when the output is a STKOF panic on <stack>, the first refused clause otherwise.
verdict() {
    if ! has "=== USAGE FAULT ==="; then
        echo "no USAGE FAULT dump"
        return
    fi
    _cfsr="$(printf '%s\n' "$OUT" | sed -n 's/.*CFSR=0x\([0-9a-fA-F]*\).*/\1/p' | head -n1)"
    if [ -z "$_cfsr" ] || [ $(( 0x$_cfsr & 0x100000 )) -eq 0 ]; then
        echo "the dump carries no STKOF (CFSR=0x$_cfsr)"
        return
    fi
    if ! has "($1)"; then
        echo "the dump does not name $1"
        return
    fi
    if has "=== THREAD FAULT"; then
        echo "a thread was killed for a fault in privileged code"
        return
    fi
    if [ "$1" = MSP ]; then
        if ! has "splim plant: arena canary intact"; then
            echo "the report wrote into the arena below the main stack"
            return
        fi
        if [ "$(printf '%s\n' "$OUT" | grep -c '^=== .*FAULT')" -ne 1 ]; then
            echo "the report's own overflow did not end the system at once"
            return
        fi
    fi
    if [ "$RC" -ne 132 ]; then
        echo "the system exited $RC, not 132"
        return
    fi
}

for plant in msp kblock; do
    stack=MSP
    if [ "$plant" = kblock ]; then
        stack=PSP
    fi
    echo "== $plant =="
    boot_plant "$plant" OFF
    why="$(verdict "$stack")"
    if [ -n "$why" ]; then
        printf '%s\n' "$OUT" | tail -n 20
        fail "$plant: $why"
    fi
    echo "== $plant, the limit lifted (red control) =="
    boot_plant "$plant" ON
    if [ -z "$(verdict "$stack")" ]; then
        printf '%s\n' "$OUT" | tail -n 20
        fail "$plant: the verdict holds with the limit lifted, so it witnesses nothing"
    fi
done
echo "PASS: both privileged overflows end in a STKOF panic on their own stack, the main stack's with the arena intact, and neither does without its limit"
exit 0
