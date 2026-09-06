#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Runtime kernel-block depth gate. Boots the two stackdepth images and reads the high-water
# the panic reporter prints off each.
#
# usage: check_stackdepth.sh <stackdepth0.elf> <stackdepth1.elf> <floor-bytes>
#
# The figure is the deepest word written on ONE thread's kernel block by the paths these two
# images execute. It is NOT a bound: a path the images do not drive contributes nothing, and
# the static walk in tests/static covers chains no image can drive at all.
#
# The reporter prints both what it reached and what the block holds above its canary, from the
# kernel side, so this gate divides two numbers it was handed and re-derives neither.
#
# Mode 0 panics and nothing else; mode 1 drives a grant-carrying spawn first. The block
# records a maximum over the slot, so mode 1 minus mode 0 is what the spawn arm reached.

set -u
. "$(dirname "$0")/../lib/gate.sh"

elf0="${1:?usage: check_stackdepth.sh <stackdepth0.elf> <stackdepth1.elf> <floor-bytes>}"
elf1="${2:?usage: check_stackdepth.sh <stackdepth0.elf> <stackdepth1.elf> <floor-bytes>}"
FLOOR="${3:?usage: check_stackdepth.sh <stackdepth0.elf> <stackdepth1.elf> <floor-bytes>}"

case "$FLOOR" in
    ''|*[!0-9]*) fail "the margin floor must be a plain integer, got '$FLOOR'" ;;
esac

echo "1..3"

# Sets USED and CAPACITY from one image's reporter line, or fails.
#
# The reporter line alone does not witness the arm: the panic path prints it whatever the arm
# reached, so an arm whose kos_thread_create was REFUSED still prints one. The image says which
# happened, and this refuses the refused case rather than measuring it.
SD_RAN='[stackdepth] spawn arm ran'
SD_NOT_RUN='[stackdepth] SPAWN ARM DID NOT RUN'

read_reading() { # <elf> <label> [line the arm must have printed to be believed]
    run_image "$1"
    if printf '%s\n' "$OUT" | grep -qF "$SD_NOT_RUN"; then
        echo "not ok - $2 reports that it could not run, so its reporter line measures a" >&2
        echo "         path that was never taken" >&2
        printf '%s\n' "$OUT" | grep -F '[stackdepth]' >&2
        return 1
    fi
    if [ "$#" -ge 3 ]; then
        if ! printf '%s\n' "$OUT" | grep -qF "$3"; then
            echo "not ok - $2 never printed '$3', so nothing says the arm ran" >&2
            printf '%s\n' "$OUT" | tail -n 5 >&2
            return 1
        fi
    fi
    _line="$(printf '%s\n' "$OUT" | sed -n 's/.*KSTACK HIGH WATER: \([0-9]\{1,\}\) of \([0-9]\{1,\}\) .*/\1 \2/p' \
             | head -n1)"
    if [ -z "$_line" ]; then
        echo "not ok - $2 printed no reporter line" >&2
        printf '%s\n' "$OUT" | tail -n 5 >&2
        return 1
    fi
    USED="${_line% *}"
    CAPACITY="${_line#* }"
    return 0
}

rc=0

if read_reading "$elf0" "mode 0"; then
    U0="$USED"
    C0="$CAPACITY"
    echo "ok 1 - kos_panic from root reached $U0 of $C0 bytes on one thread's block"
else
    echo "not ok 1 - panic arm produced no reading"
    rc=1
fi

if read_reading "$elf1" "mode 1" "$SD_RAN"; then
    U1="$USED"
    C1="$CAPACITY"
    echo "ok 2 - grant-carrying kos_thread_create then kos_panic reached $U1 of $C1 bytes"
else
    echo "not ok 2 - the spawn arm did not run, so it produced no reading of that path"
    rc=1
fi

if [ "$rc" -ne 0 ]; then
    echo "# a reading is missing, so no margin is reported"
    exit 1
fi

# The deeper reading may be either image's, so one capacity has to describe both: a margin
# taking one image's usage from the other's capacity is a subtraction across two blocks.
if [ "$C0" != "$C1" ]; then
    echo "not ok 3 - mode 0 reports a $C0 byte block and mode 1 a $C1 byte one, so no single"
    echo "#          margin describes both readings"
    echo "# The two images are built from one KICKOS_KERNEL_STACK_SIZE and must agree. A"
    echo "# disagreement means the readings came from two different builds."
    exit 1
fi
CAP="$C0"

# The deeper of the two, which is what the block actually has to hold for these paths.
DEEPEST="$U0"
ARM="panic"
if [ "$U1" -gt "$U0" ]; then
    DEEPEST="$U1"
    ARM="spawn"
fi
MARGIN=$((CAP - DEEPEST))

echo "# arms driven, and NOTHING else was: kos_panic from root; a grant-carrying"
echo "# kos_thread_create followed by kos_panic. Both on root's own kernel block."
echo "# deepest arm: $ARM, $DEEPEST bytes of $CAP above the canary"
echo "# spawn arm minus panic arm: $((U1 - U0)) bytes"
echo "# NOT DRIVEN by either arm, so absent from the figure below: every interrupt and"
echo "# fault class, every other thread's block, and the deepest chain the static walk"
echo "# covers, which stacks the panic tail on thread_create's frame through an assert no"
echo "# syscall parameter can trip."
echo "# $MARGIN bytes were left over the two arms above. This is slack over THOSE arms,"
echo "# not headroom against the trap paths. Floor $FLOOR."

if [ "$MARGIN" -lt "$FLOOR" ]; then
    echo "not ok 3 - only $MARGIN bytes left over the two arms driven, floor $FLOOR"
    echo "# The block held these paths with $MARGIN bytes to spare. That is a measurement over"
    echo "# what ran, not a bound: an undriven path can be deeper. Raise"
    echo "# KICKOS_KERNEL_STACK_SIZE rather than lowering this floor."
    exit 1
fi

echo "ok 3 - $MARGIN bytes left over the two arms driven, at or above the floor $FLOOR"
echo "# all tests passed"
echo "PASS: stackdepth: the two arms above reached $DEEPEST of $CAP bytes on one block,"
echo "PASS: leaving $MARGIN. A measurement over those two arms, not a bound on the block."
exit 0
