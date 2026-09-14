#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Gate on the WHOLE phase table reaching the wire. The kernel console takes a line or refuses
# it, so a table longer than the console ring loses whole rows and the report reads as a
# shorter build rather than as a damaged capture. Nothing else in the tree can see that.
#
# THE EXPECTATION IS IN BAND. kernel/bench/bench.cc prints its row count in the table header,
# so this compares a capture against the build that produced it and cannot drift from PH_COUNT.
#
# Three arms, and none of the first two alone is enough:
#   header     the table announced itself and said how many rows follow. A capture that lost
#              the header has no expectation at all and is refused rather than counted.
#   rows       exactly that many rows arrived.
#   terminator the sat-probe line bench_phase_print ends on arrived. A table whose rows all
#              landed and whose tail did not is still a truncated report.
#
# usage: check_bench_phase_table.sh <elf>
#        BENCH_CAPTURE=<log> check_bench_phase_table.sh    reads a recorded capture instead of
#                                                          booting, for a silicon run

set -u
. "$(dirname "$0")/../lib/gate.sh"
# The call/reply sweep ahead of the phase table is a quarter of a million round trips, so the
# library's eight seconds would kill the image before it printed anything this gate reads.
: "${QEMU_TIMEOUT:=150}"

rc=0

if [ -n "${BENCH_CAPTURE:-}" ]; then
    [ -f "$BENCH_CAPTURE" ] || fail "no capture at $BENCH_CAPTURE"
    # tools/bench/ keeps the CR on purpose, a line ending being evidence there, and an ESP
    # capture opens with a 0xFF that makes some greps call the whole file binary. The parse
    # below is awk under LC_ALL=C for that reason, and reads no line end.
    OUT="$(tr -d '\r' < "$BENCH_CAPTURE")"
    what="$BENCH_CAPTURE"
else
    _usage="usage: check_bench_phase_table.sh <elf>"
    elf="${1:?$_usage}"
    [ -f "$elf" ] || fail "no image at $elf"
    need_qemu_machine
    # No `$` anchor: the poll greps the RAW log, and a console under CONFIG_CONSOLE_CRLF
    # leaves a carriage return before every newline.
    poll_image "$elf" "^  sat-probe:" > /dev/null
    if [ "$POLL_OK" -ne 1 ]; then
        fail "the phase table never reached its sat-probe line within ${QEMU_TIMEOUT}s"
    fi
    what="$elf"
fi

# ONE PASS, so the declared count and the rows counted against it come out of the same
# capture. The per-core block sits between the rows and the terminator, and its rows are
# `    core <n>  n=<count>`: lower case, which is what keeps them out of the row count.
read -r want rows term <<EOF
$(printf '%s\n' "$OUT" | awk '
    /^  phase table \([0-9]+ rows; / {
        want = $0
        sub(/^  phase table \(/, "", want)
        sub(/ rows;.*$/, "", want)
        state = 1
        next
    }
    state == 1 && /^    [A-Z]/ { rows++; next }
    state == 1                 { state = 2 }
    state == 2 && /^  sat-probe: / { term = 1 }
    END { printf "%d %d %d\n", want + 0, rows + 0, term + 0 }')
EOF

if [ "$want" -eq 0 ]; then
    fail "no phase table header declaring a row count in $what. Either the
  table never reached the wire, or this image predates the header carrying its own count and
  the capture states no expectation to check"
fi
if [ "$rows" -ne "$want" ]; then
    bad "the phase table announced $want rows and $rows arrived. Whole rows are missing: the
  console refuses a line it cannot take whole, and the printer dropped what it was told"
fi
if [ "$term" -ne 1 ]; then
    bad "the phase table's sat-probe line never arrived, so the report was cut after the rows"
fi

assert_no_panic "the bench image panicked while the report printed"

if [ "$rc" -ne 0 ]; then
    exit 1
fi
echo "PASS: $rows of $want phase rows and the sat-probe terminator"
exit 0
