#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# NESTED-TRAP witness (rv32imac): an interrupt taken while the kernel runs a thread's syscall
# dispatch must not put its frame on that thread's stack, nor may the dispatch itself run there.
# Every thread of the app makes its syscalls from a parked sp over a poisoned band of its own
# stack, the band exactly what a regressed entry would spend; deeper than that a frame lands
# below the band and goes unseen. Main prints each band's verdict.
#
# On QEMU the interrupt log is the positive control: every raise the worker makes from inside a
# dispatch must be taken with its pc inside syscall_dispatch or kickos::irq_raise, whichever the
# compiler left it in. An intact band with nothing nested is what a deleted raise or a dispatch
# that never reopens interrupts looks like. A silicon capture carries no log.
#
#   check_trapnest.sh <elf> <interrupt-kernel-descent-bytes> <band-words> <nm>
#   KOS_CAPTURE=<log> check_trapnest.sh <board-build> <kickos-source> <cmake> <descent> <band-words>

set -u
. "$(dirname "$0")/../lib/gate.sh"

# QEMU's name for the interrupt the virt machine's injected lines arrive as.
NEST_DESC=s_software
THREADS="main ticker worker"

_usage="usage: check_trapnest.sh <elf> <interrupt-kernel-descent-bytes> <band-words> <nm>"
if judging_capture; then
    shift 3
    descent="${1:?$_usage}"
    words="${2:?$_usage}"
else
    elf="${1:?$_usage}"
    descent="${2:?$_usage}"
    words="${3:?$_usage}"
    nm="${4:?$_usage}"
fi
require_number "$descent" "the descent argument"
require_number "$words" "the band size in words"

if judging_capture; then
    judge_capture trapnest
else
    [ -x "$nm" ] || fail "no nm at $nm; the dispatcher's extent cannot be read out of the image"
    scratch_dir
    LOG="$TMP/int.log"
    QEMU_EXTRA="${QEMU_EXTRA:-} -d int -D $LOG"
    run_image "$elf"
fi

if has "\[trapnest\] ERROR"; then
    cfail error "the app reported its own failure"
fi
if ! has "\[trapnest\] worker done"; then
    cfail worker "the worker never finished its raises"
fi
if ! has "\[trapnest\] main ran after the worker"; then
    cfail join "main never ran again, so the join never returned"
fi
assert_no_panic "the arm ended in a panic"

corrupt="$(printf '%s\n' "$OUT" | grep -E '^\[trapnest\] [a-z]+ band CORRUPTED: ' | head -n1)"
if [ -n "$corrupt" ]; then
    why="$corrupt"
    room="$(printf '%s\n' "$corrupt" \
        | sed -n 's/.*the lowest \([0-9]\{1,\}\) bytes above the band.s base$/\1/p')"
    if [ -n "$room" ] && [ "$room" -lt "$descent" ]; then
        why="$why, against a worst-case interrupt descent of $descent below it"
    fi
    cfail band "NESTED FRAME ON A THREAD STACK: the kernel wrote below a parked sp: $why"
fi
for t in $THREADS; do
    if ! has "\[trapnest\] $t band intact: 0 of $words words written below the parked sp"; then
        cfail band "no intact verdict over a $words-word band for the $t thread, so nothing here
    read that stack back"
    fi
done

if judging_capture; then
    cnot_evaluated "an injected interrupt was taken inside the syscall dispatch"
    echo "PASS: the band below every thread's parked sp is intact"
    exit 0
fi

"$nm" -S -C --defined-only "$elf" > "$TMP/nm" 2>/dev/null || fail "$nm could not read $elf"
spans="$(awk 'NF >= 4 && ($4 == "syscall_dispatch" || index($4, "kickos::irq_raise(") == 1) {
              print $1, $2 }' "$TMP/nm")"
[ -n "$spans" ] || fail "neither syscall_dispatch nor kickos::irq_raise has a size in $elf, so
    no pc can be placed inside the raise's dispatch"
require_nonempty "$LOG" "QEMU wrote no interrupt log"
taken=0
nested=0
for pc in $(sed -n "s/.*async:1, .*epc:0x\([0-9a-f]*\),.*desc=$NEST_DESC\$/\1/p" "$LOG"); do
    taken=$((taken + 1))
    # QEMU prints some of these sign-extended to 64 bits; the address is the low 32.
    if [ "${#pc}" -gt 8 ]; then
        pc="${pc#"${pc%????????}"}"
    fi
    v=$(printf '%d' "0x$pc")
    inside="$(printf '%s\n' "$spans" | while read -r at size; do
        lo=$(printf '%d' "0x$at")
        if [ "$v" -ge "$lo" ] && [ "$v" -lt $((lo + $(printf '%d' "0x$size"))) ]; then
            echo yes
        fi
    done)"
    if [ -n "$inside" ]; then
        nested=$((nested + 1))
    fi
done
if [ "$taken" -eq 0 ]; then
    fail "the interrupt log holds no '$NEST_DESC' interrupt in a format this gate recognises; its
    first line: $(grep -m1 'riscv_cpu_do_interrupt' "$LOG" || echo none)"
fi
if [ "$nested" -lt "$taken" ]; then
    fail "only $nested of the $taken injected interrupt(s) were taken inside the raise's dispatch,
    so the rest nested nowhere and the intact bands did not witness them"
fi

echo "PASS: $nested of $taken injected interrupts nested inside the raise's dispatch, none on a
  thread's stack"
exit 0
