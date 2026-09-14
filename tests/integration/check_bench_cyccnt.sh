#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The bench cycle source, witnessed at runtime. Boots the bench image and refuses a counter
# that reads zero or never moves.
#
# usage: check_bench_cyccnt.sh <bench image>
#
# An arch with no cycle source still BUILDS and still prints a full report: bench_cyccnt
# returns 0 there, so every delta is 0 and the table is a plausible-looking wall of zeros.
# A counter frozen at any other constant reads the same way. That is what this refuses.
#
# The two readings are fed by different code and both are required, or half the instrument
# can rot unseen: the switch line comes from the bracket in the arch's switch.S, which reads
# the counter in ASSEMBLY, and the phase table from kernel/include/kickos/bench.h, which
# reads it in C. Neither one's failure shows up in the other.
#
# What this does NOT claim: nothing here says the counter counts CYCLES, or counts at any
# particular rate. Under an emulator it usually does neither (kernel/bench/bench.cc, and the
# KICKOS_CHIP_CYCCNT_HZ of the chip). This gate is about a live counter, not a calibrated one.

set -u
. "$(dirname "$0")/../lib/gate.sh"

# The switch line prints after the whole call/reply sweep AND the first throughput window,
# and a round trip costs about ten times more on four emulated cores than on one, so
# gate.sh's 20 s default expires mid-sweep there and the run reads as an image that printed
# no switch line at all.
: "${QEMU_TIMEOUT:=240}"

elf="${1:?usage: check_bench_cyccnt.sh <bench image>}"

echo "1..4"

run_image "$elf"
OUT_ALL="$OUT"

# The LAST report of the run: each one covers its own window, and the last is the one whose
# accumulator saw every switch before it.
#
# The label is PADDED to the width of the widest distribution name (kernel/bench/bench.cc), so
# the run of spaces after the colon changes whenever a longer name joins the table. Anchoring
# on a single space reads as an image that printed no switch line at all.
SW="$(printf '%s\n' "$OUT_ALL" \
      | sed -n 's|.*switch: \{1,\}\([0-9]\{1,\}\)/\([0-9]\{1,\}\)/\([0-9]\{1,\}\) cyc.*n=\([0-9]\{1,\}\)).*|\1 \2 \3 \4|p' \
      | tail -n1)"

if [ -z "$SW" ]; then
    echo "not ok 1 - the run printed no switch line, so nothing reports the bracket at all"
    printf '%s\n' "$OUT_ALL" | tail -n 15 | sed 's/^/# /'
    exit 1
fi
echo "ok 1 - the run printed a switch line"

P50="${SW%% *}"
REST="${SW#* }"
P99="${REST%% *}"
REST="${REST#* }"
MAX="${REST%% *}"
N="${REST#* }"

rc=0

# The hook itself. A counter can be perfect and still be read from nowhere: on an arch whose
# switch.S carries no bracket the accumulator is never fed and n stays 0, while every other
# line of the report still prints.
if [ "$N" -eq 0 ]; then
    echo "not ok 2 - the switch accumulator took no sample, so the arch switch path feeds it"
    echo "#          nothing (n=0)"
    rc=1
else
    echo "ok 2 - the arch switch path fed the accumulator $N sample(s)"
fi

# The counter moves. Zero is what a missing source reads, and a frozen one reads the same.
if [ "$P50" -eq 0 ]; then
    echo "not ok 3 - the median switch is 0 cycles, so the counter did not advance across a"
    echo "#          whole switch: it is absent, frozen, or stuck at a constant"
    rc=1
elif [ "$MAX" -le "$P50" ]; then
    echo "not ok 3 - every switch measured the same $P50 cycles (max=$MAX), which no real"
    echo "#          distribution does: the reading is constant rather than measured"
    rc=1
else
    echo "ok 3 - the switch reading moves: p50=$P50 p99=$P99 max=$MAX cyc"
fi

# The C-side reader, which the switch line says nothing about. A row with samples but a zero
# maximum is a bracket that ran and measured nothing.
ROWS="$(printf '%s\n' "$OUT_ALL" | sed -n 's|^ *[A-Z][A-Z_]* \{1,\}[0-9]\{1,\}/\([0-9]\{1,\}\) \{1,\}min=.*|\1|p')"
if [ -z "$ROWS" ]; then
    echo "not ok 4 - the run printed no phase table, so the C-side reader is unwitnessed"
    rc=1
else
    LIVE="$(printf '%s\n' "$ROWS" | grep -c -v '^0$')"
    if [ "$LIVE" -eq 0 ]; then
        echo "not ok 4 - every phase row has a maximum of 0, so the brackets in"
        echo "#          kernel/include/kickos/bench.h read a counter that does not advance"
        rc=1
    else
        echo "ok 4 - $LIVE phase row(s) carry a non-zero maximum"
    fi
fi

if [ "$rc" -ne 0 ]; then
    printf '%s\n' "$OUT_ALL" | grep -E 'cycle counter|switch:' | sed 's/^/# /'
fi

# A COMPLETE REPORT IS NOT A SURVIVED RUN. Every arm above reads text the image printed, so an
# image that prints the whole report and then faults answers all four of them. This is weak
# above one core, where an interleaved line can hide the marker from grep, and the positive
# assertion it is owed is arm 1: the switch line it takes is the LAST of the run, which the
# sweep reaches only by not faulting inside it.
assert_no_panic "the bench image panicked while the sweep ran"
exit "$rc"
