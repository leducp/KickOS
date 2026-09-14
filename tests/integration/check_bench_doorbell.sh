#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Gate on the doorbell ROUND TRIP distribution (BD_DOORBELL, kernel/bench/bench.cc). A round is
# one raise across every peer and the rendezvous that follows it, stamped end to end on the
# raising core, and the kernel runs a fixed burst of them FROM EACH CORE IN TURN before it
# prints the report.
#
# THE PROBE LINE IS READ FIRST AND EVERY OTHER ARM DEPENDS ON IT. A quiet board and a dead
# instrument print the same n, so the burst says how many rounds were asked for and how far
# THIS core's own count moved across them.
#
# The arms, and what each one breaks on:
#   probe    one burst per kernel core; each ran the rounds it was asked for, ON THE CORE IT WAS
#            ASKED FOR, and moved that core's OWN count by exactly that many. A placement that
#            did not take makes "per core" a lie; a db+0 is a sample written to a peer's row.
#   counts   the aggregate carries every core's burst.
#   order    p50 <= p99 <= max. A percentile walk reading the wrong end breaks this.
#   rows     one row per kernel core, EVERY one of them carrying its own burst and no more, and
#            the rows totalling the aggregate. A distribution indexed by anything but the
#            running core puts every sample in one row and still prints a full-looking line.
#   span     where the capture shows a live counter, each core's row MINIMUM is a multiple of
#            the raise floor that core measured inside its own burst. The raise is the part of
#            a round that happens before any peer can answer, so a bracket closed there reports
#            it and nothing else, and every other arm above still reads a full, ordered,
#            correctly indexed distribution. This is the only arm that sees it.
#
# THE BURST SIZE IS READ OFF THE CAPTURE AND NEVER PASSED IN. What is asserted is that every
# burst and every row carry the SAME count, that it is not zero, and that the aggregate is the
# sum of them.
#
# usage: check_bench_doorbell.sh <elf> <kernel cores>

set -u
. "$(dirname "$0")/../lib/gate.sh"
# The call/reply sweep ahead of the first report is a quarter of a million round trips, so the
# library's eight seconds would kill the image before it printed anything this gate reads.
: "${QEMU_TIMEOUT:=240}"

_usage="usage: check_bench_doorbell.sh <elf> <kernel cores>"
elf="${1:?$_usage}"
want="${2:?$_usage}"

require_number "$want" "the kernel core count"
[ -f "$elf" ] || fail "no image at $elf"
need_qemu_machine

if [ "$want" -le 1 ]; then
    fail "registered at $want kernel core(s); BD_DOORBELL is not declared there and a raise
  with no peer is not a round trip"
fi

# No `$` anchor on a poll pattern: the poll greps the RAW log and a console under
# CONFIG_CONSOLE_CRLF leaves a carriage return before every newline.
#
# The doorbell is the LAST distribution in BD_ order, so its own line is no guarantee that the
# per-core rows under it are on the wire. The app's IRQ line is printed after the whole report
# and is what says the block is complete.
poll_image "$elf" "^  doorbell-probe:" "^  irq:" > /dev/null

if [ "$POLL_OK" -ne 1 ]; then
    fail "no doorbell probe and report within ${QEMU_TIMEOUT}s; the image printed neither, so
  nothing below would have been read"
fi

rc=0

# ONE PASS over the capture, so every figure compared below comes out of the same report. Only
# the FIRST report is read: the bench prints one per throughput window and a poll that raced
# the second would otherwise mix two windows' counts.
read -r bursts rounds misplaced shortruns unmoved dn d50 d99 dmax rows full rowsum tooshort sw50 <<EOF
$(printf '%s\n' "$OUT" | awk -v want="$want" -v SPAN_FACTOR=2 '
    function tail_n(s,   t) { t = s; sub(/.*n=/, "", t); sub(/[^0-9].*$/, "", t); return t + 0 }
    function part(s, i,   f) { split(s, f, "/"); return f[i] + 0 }

    /^  doorbell-probe:/ && dseen == 0 {
        asked = -1; on = -2; ran = 0; moved = 0
        for (i = 2; i <= NF; i++) {
            if ($i ~ /^asked=[0-9]+$/) { asked = substr($i, 7) + 0 }
            if ($i ~ /^on=[0-9]+$/)    { on    = substr($i, 4) + 0 }
            if ($i ~ /^ran=[0-9]+$/)   { ran   = substr($i, 5) + 0 }
            if ($i ~ /^db\+[0-9]+$/)   { moved = substr($i, 4) + 0 }
            if ($i ~ /^raise=[0-9]+$/) { raise[on] = substr($i, 7) + 0; raised++ }
        }
        bursts++
        # The FIRST burst sets the count every later one and every row is held to.
        if (bursts == 1)      { rounds = ran }
        if (asked != on)      { misplaced++ }
        if (ran != rounds)    { shortruns++ }
        if (moved != rounds)  { unmoved++ }
        next
    }
    /^  switch:/ && swseen == 0 { swseen = 1; sw50 = part($2, 1); cur = "switch"; next }
    /^  doorbell: / && dseen == 0 {
        dseen = 1; cur = "db"
        d50 = part($2, 1); d99 = part($2, 2); dmax = part($2, 3); dn = tail_n($0)
        next
    }
    # The per-core rows of whichever distribution line came last. The phase table prints
    # `core <n>  n=<count>` with no colon, so its block cannot be read here.
    /^ +core [0-9]+: / {
        if (cur == "db") {
            n = tail_n($0)
            c = $2; sub(/:$/, "", c); c = c + 0
            rows++
            rowsum += n
            if (n == rounds) { full++ }
            # The row MINIMUM against the raise floor the SAME core measured in its own
            # burst. A round trip is the raise plus every far side; SPAN_FACTOR is the
            # margin below which the two are the same object.
            if (raised > 0 && part($3, 1) < SPAN_FACTOR * raise[c]) { tooshort++ }
        }
        next
    }
    { cur = "" }
    END { printf "%d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
                 bursts + 0, rounds + 0, misplaced + 0, shortruns + 0, unmoved + 0,
                 dn + 0, d50 + 0, d99 + 0, dmax + 0,
                 rows + 0, full + 0, rowsum + 0, tooshort + 0, sw50 + 0 }')
EOF

# --- probe -----------------------------------------------------------------
if [ "$bursts" -ne "$want" ]; then
    fail "the capture carried $bursts doorbell burst(s) for a $want-core kernel; every arm
  below would read a zero it was handed rather than one the kernel reported"
fi
if [ "$rounds" -le 0 ]; then
    bad "the first burst ran 0 rounds, so every count below is trivially consistent with every
  other and the distribution is asserted against nothing"
fi
if [ "$misplaced" -ne 0 ]; then
    bad "$misplaced burst(s) ran on a core other than the one asked for; the per-core rows
  below then describe where the reporter happened to sit and not which core raised"
fi
if [ "$shortruns" -ne 0 ]; then
    bad "$shortruns burst(s) ran fewer than $rounds rounds"
fi
if [ "$unmoved" -ne 0 ]; then
    bad "$unmoved burst(s) left the RAISING core's own sample count short of $rounds; the
  bracket never accumulates, or it accumulated on a peer's row"
fi

# --- counts, order ---------------------------------------------------------
_expect=$(( want * rounds ))
if [ "$dn" -ne "$_expect" ]; then
    bad "the doorbell distribution reports n=$dn against $want core(s) x $rounds rounds"
fi
if [ "$d50" -gt "$d99" ] || [ "$d99" -gt "$dmax" ]; then
    bad "the doorbell distribution reports p50=$d50 p99=$d99 max=$dmax, which is not ordered"
fi

# --- per core --------------------------------------------------------------
if [ "$rows" -ne "$want" ]; then
    bad "the doorbell distribution printed $rows per-core row(s) for a $want-core kernel"
fi
if [ "$full" -ne "$want" ]; then
    bad "$full row(s) of $want carried a full burst of $rounds; every core raised the same
  number of rounds, so a row short of it is a sample indexed by something other than the
  raising core"
fi
if [ "$rowsum" -ne "$dn" ]; then
    bad "the doorbell rows total $rowsum samples and the aggregated line reports $dn; both
  come out of ONE read of each row, so they can only differ if a row was printed twice or
  dropped"
fi

# --- the span, where the capture shows a counter ---------------------------
mode="counts only (the switch line reports a p50 of 0, so this board has no cycle source)"
if [ "$sw50" -gt 0 ]; then
    mode="counts and cycles"
    if [ "$tooshort" -ne 0 ]; then
        bad "$tooshort core row(s) report a minimum under twice the raise floor that core
  measured in the same burst; the bracket is closing inside the raise, before any peer could
  have answered, and every count above still reads correct"
    fi
fi

assert_no_panic "the bench image panicked while the doorbell bursts ran"

if [ "$rc" -ne 0 ]; then
    exit 1
fi
echo "PASS: $bursts burst(s) of $rounds, each on the core asked for; doorbell n=$dn p50=$d50 p99=$d99 max=$dmax over $rows rows; $mode"
exit 0
