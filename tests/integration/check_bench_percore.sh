#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Gate on the microbench accumulators being indexed by kernel core, on an image built above one
# of them. It reads the per-core block kernel/bench/bench.cc prints under the phase table.
#
# THE ASSERTION IS THE SAMPLE COUNT AND NEVER A CYCLE FIGURE. A backend with no bench_cyccnt
# and no switch stamping reports every delta as 0, which is expected and says nothing about
# where a sample was accumulated; n says which core accumulated it.
#
# Two arms, and the first alone would pass an image that indexed core 0 everywhere:
#   spread     more than one core reported samples. This is what a constant subscript breaks.
#   totals     the per-core counts add up to what the aggregated table reports. This is what a
#              report reading one row instead of summing them breaks.
#
# usage: check_bench_percore.sh <elf> <kernel cores>

set -u
. "$(dirname "$0")/../lib/gate.sh"
# The call/reply sweep ahead of the phase table is a quarter of a million round trips, so the
# library's eight seconds would kill the image before it printed anything this gate reads.
: "${QEMU_TIMEOUT:=150}"

_usage="usage: check_bench_percore.sh <elf> <kernel cores>"
elf="${1:?$_usage}"
want="${2:?$_usage}"

require_number "$want" "the kernel core count"
if [ "$want" -le 1 ]; then
    fail "kernel core count is $want. At one core the bench prints no per-core block at all,
  so this gate belongs only on a preset whose kernel core count exceeds one"
fi
[ -f "$elf" ] || fail "no image at $elf"
need_qemu_machine

# No `$` anchor: the poll greps the RAW log, and a console under CONFIG_CONSOLE_CRLF leaves a
# carriage return before every newline. The awk pass below reads $OUT, which poll_image has
# already stripped, so it anchors both ends.
poll_image "$elf" "^  per-core phase samples:" > /dev/null

if [ "$POLL_OK" -ne 1 ]; then
    fail "no per-core phase block within ${QEMU_TIMEOUT}s; the image printed no phase table"
fi

rc=0

# ONE PASS OVER BOTH BLOCKS, so the two totals compared below are read out of the same capture.
# The phase table's rows are the AGGREGATE over cores and the per-core block's rows are the
# total over phases, so the two sums are the same number by construction and disagree only if
# the report stopped summing.
#
# The per-core row shape is `core <n>  n=<count>`, with no colon: the switch table prints a
# `core <n>: ...` line of its own and it must not be read here.
#
# A phase row carries a trailing SAT column when it dropped a span too wide to state, so the
# count is cut at the first non-digit rather than read off the end of the line. The saturated
# samples are in neither total, which is what keeps the two comparable.
read -r prows ptot crows ctot spread <<EOF
$(printf '%s\n' "$OUT" | awk '
    /^  phase table/              { in_phase = 1; next }
    /^  per-core phase samples:$/ { in_phase = 0; in_rows = 1; next }
    in_phase && /  n=[0-9]+/      { n = $0; sub(/.*  n=/, "", n); sub(/[^0-9].*$/, "", n)
                                    ptot += n; prows++; next }
    in_rows && /^ +core [0-9]+  n=[0-9]+$/ {
        split($0, f, "n=")
        ctot += f[2]
        if (f[2] + 0 > 0) { spread++ }
        crows++
        next
    }
    in_rows                       { in_rows = 0 }
    END { printf "%d %d %d %d %d\n", prows + 0, ptot + 0, crows + 0, ctot + 0, spread + 0 }')
EOF

# A capture that yielded no rows leaves every comparison below vacuously satisfied.
if [ "$prows" -eq 0 ]; then
    fail "the phase table carried no rows, so its totals say nothing"
fi
if [ "$crows" -ne "$want" ]; then
    bad "the per-core block carried $crows row(s) for a $want-core kernel"
fi
if [ "$spread" -lt 2 ]; then
    bad "$spread core(s) of $want reported phase samples; the accumulators are not indexed by
  the running core, or one core alone ever entered the kernel"
fi
if [ "$ptot" -ne "$ctot" ]; then
    bad "the aggregated table totals $ptot samples and the per-core rows total $ctot; the
  report is not summing the rows it prints"
fi

assert_no_panic "the bench image panicked while the sweep ran"

if [ "$rc" -ne 0 ]; then
    exit 1
fi
echo "PASS: $spread of $want cores accumulated, $ptot samples aggregated"
exit 0
