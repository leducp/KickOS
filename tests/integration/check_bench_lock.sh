#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Gate on the outermost lock's HOLD and WAIT distributions (kernel/bench/bench.cc, the BD_*
# family in <kickos/bench.h>). It reads the lines the kernel prints from inside kos_bench.
#
# THE ASSERTIONS ARE SAMPLE COUNTS AND ORDERINGS, AND A CYCLE FIGURE ONLY WHERE THE CAPTURE
# ITSELF SHOWS A COUNTER. A backend whose bench_cyccnt returns 0 reports every delta as 0,
# which is expected and says nothing about where a sample was accumulated; n says that. The
# switch line's own maximum is the discriminator, and it comes from an instrument this gate
# does not otherwise read, so the cycle arm is never its own witness.
#
# The arms, and what each one breaks on:
#   probe    the kernel opens three nested IrqLocks and reports how far the lock counts moved
#            on ITS OWN core. Exactly one, from a starting depth of zero. Zero says the bracket
#            never accumulates or wrote a peer's row; three says it samples every nesting level
#            instead of the outermost, which inflates n and deflates every quantile.
#   counts   the hold distribution carried samples at all.
#   order    p50 <= p99 <= max. A percentile walk reading the wrong end breaks this.
#   rows     above one core: one row per kernel core, more than one of them accumulating, the
#            rows totalling the aggregate, and the aggregate EXCEEDING every single row. The
#            last of those is what a report reading one row instead of summing them breaks;
#            the total is a claim about the print, the report reading each row exactly once.
#   wait     above one core: the spin in arch_kernel_lock produced samples. It cannot at one.
#   cycles   where the switch line shows a live counter, the hold figures are not all zero.
#   scope    THE SWITCH ROW'S OWN DENOMINATOR. The trap-handler IPC fastpath swaps threads
#            inside the trap and never reaches the deferred switcher that stamps the bracket,
#            so those swaps are in no distribution. switch-probe counts the ones this window
#            held, and a window that held any publishes a switch row which is a SUBSET of the
#            physical swaps in it with nothing else saying so. The reported windows are the
#            ping-pong burst and the doorbell probe, neither of which issues an IPC call, so
#            the honest figure there is zero and anything else is a finding.
#
# usage: check_bench_lock.sh <elf> <kernel cores>

set -u
. "$(dirname "$0")/../lib/gate.sh"
# The call/reply sweep ahead of the first report is a quarter of a million round trips, so the
# library's eight seconds would kill the image before it printed anything this gate reads.
: "${QEMU_TIMEOUT:=240}"

_usage="usage: check_bench_lock.sh <elf> <kernel cores>"
elf="${1:?$_usage}"
want="${2:?$_usage}"

require_number "$want" "the kernel core count"
[ -f "$elf" ] || fail "no image at $elf"
need_qemu_machine

# No `$` anchor on a poll pattern: the poll greps the RAW log and a console under
# CONFIG_CONSOLE_CRLF leaves a carriage return before every newline. The awk pass below reads
# $OUT, which poll_image has already stripped.
#
# The distributions print in BD_ order, so the arrival of a later one is the guarantee that an
# earlier one's per-core block is complete. Above one core that is lock-wait; at one core
# lock-hold is itself the last, and prints no per-core block to wait for.
if [ "$want" -gt 1 ]; then
    poll_image "$elf" "^  lock-probe:" "^  lock-wait:" > /dev/null
else
    poll_image "$elf" "^  lock-probe:" "^  lock-hold:" > /dev/null
fi

if [ "$POLL_OK" -ne 1 ]; then
    fail "no lock-probe and lock distribution within ${QEMU_TIMEOUT}s; the image printed
  neither, so nothing below would have been read"
fi

rc=0

# ONE PASS over the capture, so every figure compared below comes out of the same report. Only
# the FIRST report is read: the bench prints one per throughput window and a poll that raced
# the second would otherwise mix two windows' counts.
read -r pseen phold pwait pdepth hn h50 h99 hmax wn rows spread rowsum rowmax swmax \
        spseen spfast <<EOF
$(printf '%s\n' "$OUT" | awk '
    function tail_n(s,   t) { t = s; sub(/.*n=/, "", t); sub(/[^0-9].*$/, "", t); return t + 0 }
    function part(s, i,   f) { split(s, f, "/"); return f[i] + 0 }

    /^  lock-probe:/ && pseen == 0 {
        pseen = 1
        for (i = 2; i <= NF; i++) {
            if ($i ~ /^hold\+[0-9]+$/)  { phold  = substr($i, 6) + 0 }
            if ($i ~ /^wait\+[0-9]+$/)  { pwait  = substr($i, 6) + 0 }
            if ($i ~ /^depth=[0-9]+$/)  { pdepth = substr($i, 7) + 0 }
        }
        next
    }
    /^  switch-probe:/ && spseen == 0 {
        spseen = 1
        for (i = 2; i <= NF; i++) {
            if ($i ~ /^fastpath-swaps=[0-9]+$/) { spfast = substr($i, 16) + 0 }
        }
        next
    }
    /^  switch:/ && swseen == 0 { swseen = 1; swmax = part($2, 3); cur = "switch"; next }
    /^  lock-hold:/ && hseen == 0 {
        hseen = 1; cur = "hold"
        h50 = part($2, 1); h99 = part($2, 2); hmax = part($2, 3); hn = tail_n($0)
        next
    }
    /^  lock-wait:/ && wseen == 0 { wseen = 1; cur = "wait"; wn = tail_n($0); next }
    # The per-core rows of whichever distribution line came last. The phase table prints
    # `core <n>  n=<count>` with no colon, so its block cannot be read here.
    /^ +core [0-9]+: / {
        if (cur == "hold") {
            n = tail_n($0)
            rows++
            rowsum += n
            if (n > rowmax) { rowmax = n }
            if (n > 0) { spread++ }
        }
        next
    }
    { cur = "" }
    END { printf "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
                 pseen + 0, phold + 0, pwait + 0, pdepth + 0,
                 hn + 0, h50 + 0, h99 + 0, hmax + 0, wn + 0,
                 rows + 0, spread + 0, rowsum + 0, rowmax + 0, swmax + 0,
                 spseen + 0, spfast + 0 }')
EOF

# --- probe -----------------------------------------------------------------
if [ "$pseen" -ne 1 ]; then
    fail "the capture carried no lock-probe line, so every arm below reads a zero it was
  handed rather than one the kernel reported"
fi
if [ "$pdepth" -ne 0 ]; then
    bad "the lock probe ran at bracket depth $pdepth; hold+1 would then say nothing about the
  OUTERMOST bracket, the probe's own locks never having been outermost"
fi
if [ "$phold" -ne 1 ]; then
    bad "three nested IrqLocks moved the hold count by $phold on the probing core; exactly one
  is the outermost bracket, 0 is an accumulator that never fired or wrote a peer's row, and 3
  is one sampling every nesting level"
fi

# --- the switch row's scope ------------------------------------------------
if [ "$spseen" -ne 1 ]; then
    bad "the capture carried no switch-probe line, so nothing states how many physical swaps
  this window held outside the switch row. A build that stopped printing it would leave the row
  looking complete"
elif [ "$spfast" -ne 0 ]; then
    bad "the report window held $spfast IPC-fastpath swap(s), which swap inside the trap and
  never reach the deferred switcher the bracket is stamped from. The switch row's n is then a
  subset of the physical swaps in its own window, and every percentile under it describes the
  deferred path alone"
fi

# --- counts, order ---------------------------------------------------------
if [ "$hn" -le 0 ]; then
    bad "the hold distribution carried no samples; a quiet board and a dead instrument are the
  same output, which is why the probe above is read first"
fi
if [ "$h50" -gt "$h99" ] || [ "$h99" -gt "$hmax" ]; then
    bad "the hold distribution reports p50=$h50 p99=$h99 max=$hmax, which is not ordered"
fi

# --- per core --------------------------------------------------------------
if [ "$want" -gt 1 ]; then
    if [ "$pwait" -ne 1 ]; then
        bad "three nested IrqLocks moved the wait count by $pwait on the probing core; the
  cross-core lock is taken once, as the depth rises from zero"
    fi
    if [ "$rows" -ne "$want" ]; then
        bad "the hold distribution printed $rows per-core row(s) for a $want-core kernel"
    fi
    if [ "$spread" -lt 2 ]; then
        bad "$spread core(s) of $want accumulated a hold sample; the distribution is not
  indexed by the running core, or one core alone ever took the lock"
    fi
    if [ "$rowsum" -ne "$hn" ]; then
        bad "the hold rows total $rowsum samples and the aggregated line reports $hn; both come
  out of ONE read of each row, so they can only differ if a row was printed twice or dropped"
    fi
    if [ "$hn" -le "$rowmax" ]; then
        bad "the aggregated hold line reports $hn samples and the busiest single core reports
  $rowmax; the report is reading one row rather than summing them"
    fi
    if [ "$wn" -le 0 ]; then
        bad "the wait distribution carried no samples on a $want-core kernel; every acquire
  from depth zero passes through the spin this instrument brackets"
    fi
fi

# --- cycles, where the capture shows a counter -----------------------------
mode="counts only (the switch line reports a maximum of 0, so this board has no cycle source)"
if [ "$swmax" -gt 0 ]; then
    mode="counts and cycles"
    if [ "$hmax" -le 0 ]; then
        bad "the switch instrument measured $swmax cycles and the hold distribution measured 0;
  the bracket is counting samples without timing them"
    fi
    if [ "$h50" -le 0 ]; then
        bad "the hold p50 is 0 on a board whose counter runs; the histogram took no sample the
  percentile walk could find"
    fi
fi

assert_no_panic "the bench image panicked while the sweep ran"

if [ "$rc" -ne 0 ]; then
    exit 1
fi
_spread="at one kernel core, which prints no per-core block"
if [ "$want" -gt 1 ]; then
    _spread="over $spread of $want cores, wait n=$wn"
fi
echo "PASS: probe hold+$phold wait+$pwait at depth $pdepth; hold n=$hn p50=$h50 p99=$h99 max=$hmax $_spread; $mode"
exit 0
