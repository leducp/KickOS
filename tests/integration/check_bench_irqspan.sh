#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Gate on the IRQ distributions the bench reports: the inject-to-handler row, the four masked
# worst-case spans, and the end-to-end span from the raise to the woken userspace thread's
# first read of its device window, split by whether that thread ran on the core that took the
# interrupt.
#
# A board that cannot inject prints a complete, plausible report of zeros, so no cycle figure is
# read before its sample count has been checked.
#
# The entry row spins for the handler inside a syscall, so it is empty on a backend that enters
# the kernel with interrupts masked; the end-to-end span leaves the kernel between the raise and
# the wake, so it is not. An empty entry row beside closed end-to-end spans is that backend. An
# empty entry row beside no closed span is a board that cannot deliver an injected interrupt at
# all, and only that is a failure.
#
# The arms:
#   probe    both probe lines exist, and the raise denominator is non-zero. n=0 beside
#            raised=0 is a line the kernel never attached, which the row alone cannot show.
#   fields   every field an arm reads parsed, on the line that carries it. An unmatched token
#            reads as an in-range zero, and that zero satisfies the window bounds, the tare
#            floor and the foreign identity, so an absent field silently disables its arm.
#   domain   the entry and masked rows subtract two stamps that must come from one counter:
#            per-core cycle counters have no common zero, and a cross-core difference reads as
#            a plausible latency. Three statements per row, each failing where the others pass:
#            the probe names one handler core (`hcore=mixed` is a row fed from several clocks);
#            a populated row was filled on the core the sweep ran on; and the row's maximum is
#            under the raise-to-observation window the raising core measured, which catches a
#            classifier stuck on "local". A frozen or glitching counter heads its rows
#            min/avg/max, and the window bound is not applied there.
#            Above one core, an empty row beside a non-zero foreign count is the honest answer
#            for a line delivered to another core. At one kernel core there is one clock, so any
#            refusal is the instrument disagreeing with itself.
#   population   a populated row's n plus its own probe's foreign equals that probe's raised. A
#            handler that times out inside the sweep's fixed spin lands in neither count. n=0
#            is left to the arms above.
#   entry    the entry row carried samples, or the sweep refused every one as foreign, or the
#            end-to-end span shows this backend masks its syscalls. A description, not a
#            verdict: where none holds, no span closed either, and the e2e clause refuses that.
#   wcase    four masked spans reported, each carrying samples exactly when the entry row
#            does, and none of them more than one span's worth. The first refuses a row that
#            fired where its siblings did not; the second refuses a sample landing in another
#            span's slot, or a slot not cleared between sweeps, which leaves a row climbing.
#            Exact counts are not compared across the five: the entry spin can time out on a
#            contended emulator while the masked one, which raises under its own mask, does not.
#   span     the four rows' labels ascend where four arrived, and at one kernel core their p50s
#            grow with the span and the widest clears the narrowest by more than a bucket.
#            Sample counts cannot show this: an ignored span_bytes leaves four identical rows
#            with the same counts as four that separate.
#   label    each label is the span its own row measured. A row relabelled to a wider span
#            still ascends and still grows, so order does not prove it. Every pair of rows
#            bounds one per-byte rate, and the labels are consistent exactly when one rate fits
#            all six pairs.
#   e2e      the two locality rows total exactly what the probe says was closed, the close path
#            refused nothing, and some span closed. The last clause has two diagnoses: an empty
#            entry row with nothing refused as foreign is a board that delivers no injected
#            interrupt, and anything else is a waiter that never woke. They are branches, not
#            arms, the first implying the second.
#   passes   the sweep's own denominator, which closed and dropped are not. A pass whose waiter
#            never parks is abandoned after the app's retry bound and moves neither counter, so
#            the rows would report the passes that ran as though they were all of them. `asked`
#            is the sweep size the app ran, `raised` what the kernel let through, and every
#            raise owes a counted close.
#   settle   the app's own mark of how many passes waited out their set-up before their first
#            raise, out of how many it ran. A pass whose poll ran out raises anyway, so its first
#            sample may price the set-up: the count is refused only where it cannot be a count,
#            and a short one is stated in the verdict rather than refused.
#   floor    the anti-vacuity arm: the local end-to-end p50 exceeds the tare, which is the same
#            userspace read and the same trap with no interrupt in it. A span that closed early
#            reads as a fast figure and nothing else says so.
#   local    local equals closed at every core count. The line is delivered on its claim core
#            and the waiter is pinned there, so every wake is local by construction and a
#            cross-core sample is a wake the pin did not hold.
#   order    p50 <= p99 <= max on the entry row, the local end-to-end row and the four masked
#            rows. The cross-core row is refused whenever it carries a sample, so it has nothing
#            to order.
#
# What this gate cannot see: above one kernel core no board here delivers an injected line to
# the core that raised it, so the domain arms refuse every sample, the masked rows arrive empty
# and the span arm judges nothing there. A frozen cycle counter fails the span arm at one core,
# but only the capture chain and check_bench_cyccnt.sh settle the counter itself.
#
# With an image it does all of the above. With --controls it runs only the planted reports,
# which cover every arm that reads a report's shape and none that reads a timing figure. The
# capture half is a latency measurement on a machine whose load nobody controls and stays out
# of CI.
#
# Every clause owes a plant no other clause refuses: a plant that trips two arms proves
# neither. The exceptions are named where they sit and are all one shape: a missing probe line
# or denominator field reads as raised=0 or closed=0, so its guard can never refuse a report the
# arms below would let through. Those are proven as a shape rather than as arms of their own.
#
# usage: check_bench_irqspan.sh <elf> <kernel cores>
#        check_bench_irqspan.sh --controls
#        BENCH_CAPTURE=<log> check_bench_irqspan.sh <kernel cores>    reads a recorded capture
#                                                    instead of booting, for a silicon run
#
# The recorded path judges every report window; the polled one judges only the first. A
# silicon capture holds one report per throughput row, and read globally the first window's
# accounting answers for every later one that dropped its own. A poll stops the image on the
# first complete block, so what follows it is a window the run was cut off in.

set -u
. "$(dirname "$0")/../lib/gate.sh"

# gate.sh's verdict helpers, tagged with the report window a finding came from: the arms name
# a row and not a window, so an untagged finding from a multi-window capture cannot be placed.
WIN_TAG=""
fail() { echo "FAIL: $WIN_TAG$*" >&2; exit 1; }
bad() { echo "FAIL: $WIN_TAG$*" >&2; rc=1; }
# gate.sh's default kills the image before the IRQ block prints: the call/reply sweep ahead of
# it is a quarter of a million round trips, and the end-to-end sweep parks and wakes a thread
# once per sample on every core in turn.
: "${QEMU_TIMEOUT:=400}"

_usage="usage: check_bench_irqspan.sh <elf> <kernel cores> | check_bench_irqspan.sh --controls |
       BENCH_CAPTURE=<log> check_bench_irqspan.sh <kernel cores>"
controls_only=0
if [ -n "${BENCH_CAPTURE:-}" ]; then
    [ -f "$BENCH_CAPTURE" ] || fail "no capture at $BENCH_CAPTURE"
    elf=""
    want="${1:?$_usage}"
    require_number "$want" "the kernel core count"
    # A silicon capture arrives CRLF, and no arm reads a line ending.
    OUT="$(tr -d '\r' < "$BENCH_CAPTURE")"
elif [ "${1:-}" = "--controls" ]; then
    controls_only=1
    # For set -u only: every ctl call sets `want` from its own argument and restores it.
    want=1
    elf=""
else
    elf="${1:?$_usage}"
    want="${2:?$_usage}"

    require_number "$want" "the kernel core count"
    [ -f "$elf" ] || fail "no image at $elf"
    need_qemu_machine

    # No `$` anchor: the poll greps the raw log, where KICKOS_CONSOLE_CRLF leaves a carriage
    # return before every newline.
    #
    # Each pattern matches the row's last token `n=<count>)` and not its label: grep answers on
    # a partial final line of a log still being written, so a label-only pattern stops the image
    # before the row's figures and the arms refuse a board that was working.
    #
    # Above one core the cross row prints after the local one and needs its own pattern.
    if [ "$want" -gt 1 ]; then
        poll_image "$elf" "^  irq-probe:" "^  e2e-local:.*n=[0-9]+\)" \
                   "^  e2e-cross:.*n=[0-9]+\)" > /dev/null
    else
        poll_image "$elf" "^  irq-probe:" "^  e2e-local:.*n=[0-9]+\)" > /dev/null
    fi

    if [ "$POLL_OK" -ne 1 ]; then
        fail "no complete IRQ block within ${QEMU_TIMEOUT}s: the entry probe, the end-to-end
  row with its sample count, or above one core the cross row, never arrived whole, so nothing
  below would have been read"
    fi
fi

read_report() { # <capture text>
    # One pass, so every figure compared below comes out of the same report. Only the first
    # report is read: a poll that raced the second window would mix two windows' counts.
    read -r iseen iline iraised en e50 e99 emax wrows wok wbig pseen pline pclosed pdropped ptare \
            l50 l99 lmax ln cn c50 c99 cmax egl ion ihcore iforeign iwin wprobes wforeign \
            wdom wbound wgap qseen qasked qraised \
            ipl ipo ipr iph ipf ipw wmo wmr wmh wmf wmw wmlab ppl ppc ppd ppt qpa qpr \
            tseen tset tpass tps \
            s1 a1 b1 m1 s2 a2 b2 m2 s3 a3 b3 m3 s4 a4 b4 m4 <<EOF
$(printf '%s\n' "$1" | awk '
    function tail_n(s,   t) { t = s; sub(/.*n=/, "", t); sub(/[^0-9].*$/, "", t); return t + 0 }
    function part(s, i,   f) { split(s, f, "/"); return f[i] + 0 }

    # Every field carries a flag saying it parsed: `+ 0` on an unmatched token is an in-range
    # zero that several arms are satisfied by.
    /^  irq-probe:/ && iseen == 0 {
        iseen = 1
        ihcore = -1
        for (i = 2; i <= NF; i++) {
            if ($i ~ /^line=[0-9]+$/)    { iline    = substr($i, 6) + 0; ipl = 1 }
            if ($i ~ /^on=[0-9]+$/)      { ion      = substr($i, 4) + 0; ipo = 1 }
            if ($i ~ /^raised=[0-9]+$/)  { iraised  = substr($i, 8) + 0; ipr = 1 }
            if ($i ~ /^hcore=[0-9]+$/)   { ihcore   = substr($i, 7) + 0; iph = 1 }
            if ($i == "hcore=mixed")     { ihcore   = -2; iph = 1 }
            # A core the kernel cannot name is a parsed field: the domain arm reads the -1.
            if ($i == "hcore=none")      { iph = 1 }
            if ($i ~ /^foreign=[0-9]+$/) { iforeign = substr($i, 9) + 0; ipf = 1 }
            if ($i ~ /^win=[0-9]+$/)     { iwin     = substr($i, 5) + 0; ipw = 1 }
        }
        next
    }
    # Paired with the row below it, not summed: the four spans are sampled in turn inside one
    # sweep, but each span prints its own probe with its own counts and window. wpforeign and
    # wpraised hold this probe only; wforeign is the running total.
    /^  wcase-probe:/ && wprobes < 4 {
        wprobes++
        wphc = -1; wpon = 0; wpwin = 0; wpforeign = 0; wpraised = 0
        so = 0; sr = 0; sh = 0; sf = 0; sw = 0
        for (i = 2; i <= NF; i++) {
            if ($i ~ /^on=[0-9]+$/)      { wpon = substr($i, 4) + 0; so = 1 }
            if ($i ~ /^raised=[0-9]+$/)  { wpraised = substr($i, 8) + 0; sr = 1 }
            if ($i ~ /^hcore=[0-9]+$/)   { wphc = substr($i, 7) + 0; sh = 1 }
            if ($i == "hcore=mixed")     { wphc = -2; sh = 1 }
            if ($i == "hcore=none")      { sh = 1 }
            if ($i ~ /^foreign=[0-9]+$/) { wpforeign = substr($i, 9) + 0; wforeign += wpforeign; sf = 1 }
            if ($i ~ /^win=[0-9]+$/)     { wpwin = substr($i, 5) + 0; sw = 1 }
        }
        # Over the probes that arrived; a missing probe is the probe-count arm.
        if (so == 0) { wmo++ }
        if (sr == 0) { wmr++ }
        if (sh == 0) { wmh++ }
        if (sf == 0) { wmf++ }
        if (sw == 0) { wmw++ }
        next
    }
    /^  e2e-probe:/ && pseen == 0 {
        pseen = 1
        for (i = 2; i <= NF; i++) {
            if ($i ~ /^line=[0-9]+$/)    { pline    = substr($i, 6) + 0; ppl = 1 }
            if ($i ~ /^closed=[0-9]+$/)  { pclosed  = substr($i, 8) + 0; ppc = 1 }
            if ($i ~ /^dropped=[0-9]+$/) { pdropped = substr($i, 9) + 0; ppd = 1 }
            if ($i ~ /^tare=[0-9]+\/[0-9]+$/) { ptare = part(substr($i, 6), 1); ppt = 1 }
        }
        next
    }
    /^  e2e-passes:/ && qseen == 0 {
        qseen = 1
        for (i = 2; i <= NF; i++) {
            if ($i ~ /^asked=[0-9]+$/)  { qasked  = substr($i, 7) + 0; qpa = 1 }
            if ($i ~ /^raised=[0-9]+$/) { qraised = substr($i, 8) + 0; qpr = 1 }
        }
        next
    }
    /^  e2e-settle:/ && tseen == 0 {
        tseen = 1
        for (i = 2; i <= NF; i++) {
            if ($i ~ /^settled=[0-9]+\/[0-9]+$/) {
                tset = part(substr($i, 9), 1); tpass = part(substr($i, 9), 2); tps = 1
            }
        }
        next
    }
    /^  irq:/ && eseen == 0 {
        eseen = 1
        e50 = part($2, 1); e99 = part($2, 2); emax = part($2, 3); en = tail_n($0)
        # A glitching or frozen counter heads the row min/avg/max instead of percentiles. The
        # window bound is skipped there: a glitched read can only inflate, which is no offset.
        if (index($0, "(min/avg/max")) { egl = 1 }
        next
    }
    /^  wcase-irq\[/ && wrows < 4 {
        wrows++
        n = tail_n($0)
        if (n > 0) { wok++ }
        if (n > wbig) { wbig = n }
        # Read off the label, not assumed from row order: rows printed in another order still
        # look plausible.
        if (match($1, /\[[0-9]+B\]/)) { ws[wrows] = substr($1, RSTART + 1, RLENGTH - 3) + 0 }
        else { wmlab++ }
        w50[wrows] = part($2, 1); w99[wrows] = part($2, 2); wmax[wrows] = part($2, 3)
        if (n > 0 && wphc != wpon) { wdom++ }
        if (n > 0 && egl == 0 && wpwin > 0 && part($2, 3) > wpwin) { wbound++ }
        # Scoped to n>0: a handler that timed out inside the fixed spin lands in neither count,
        # and only a populated row shows that gap.
        if (n > 0 && n + wpforeign != wpraised) { wgap++ }
        next
    }
    /^  e2e-local:/ && lseen == 0 {
        lseen = 1
        l50 = part($2, 1); l99 = part($2, 2); lmax = part($2, 3); ln = tail_n($0)
        next
    }
    /^  e2e-cross:/ && cseen == 0 {
        cseen = 1
        c50 = part($2, 1); c99 = part($2, 2); cmax = part($2, 3); cn = tail_n($0)
        next
    }
    END {
        if (iseen == 0) { ihcore = -1 }
        printf "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
               iseen + 0, iline + 0, iraised + 0,
               en + 0, e50 + 0, e99 + 0, emax + 0, wrows + 0, wok + 0, wbig + 0,
               pseen + 0, pline + 0, pclosed + 0, pdropped + 0, ptare + 0,
               l50 + 0, l99 + 0, lmax + 0, ln + 0, cn + 0,
               c50 + 0, c99 + 0, cmax + 0;
        printf " %d %d %d %d %d %d %d %d %d %d %d %d %d",
               egl + 0, ion + 0, ihcore + 0, iforeign + 0, iwin + 0,
               wprobes + 0, wforeign + 0, wdom + 0, wbound + 0, wgap + 0,
               qseen + 0, qasked + 0, qraised + 0;
        printf " %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
               ipl + 0, ipo + 0, ipr + 0, iph + 0, ipf + 0, ipw + 0,
               wmo + 0, wmr + 0, wmh + 0, wmf + 0, wmw + 0, wmlab + 0,
               ppl + 0, ppc + 0, ppd + 0, ppt + 0, qpa + 0, qpr + 0;
        printf " %d %d %d %d", tseen + 0, tset + 0, tpass + 0, tps + 0;
        for (i = 1; i <= 4; i++) {
            printf " %d %d %d %d", ws[i] + 0, w50[i] + 0, w99[i] + 0, wmax[i] + 0;
        }
        printf "\n";
    }')
EOF
}

# A window opens at the row every report begins with and runs to the next one or to the end,
# the same model tools/bench/bench-capture.sh reads these captures by.
window_count() { # <capture text>
    printf '%s\n' "$1" | grep -c '^  throughput: '
}
window_text() { # <capture text> <index>
    printf '%s\n' "$1" | awk -v want="$2" '
        /^  throughput: / { win++ }
        win == want { print }
        win > want { exit }'
}
judge_capture() { # <capture text>
    _jc_n=$(window_count "$1")
    require_number "$_jc_n" "the report window count of this capture"
    if [ "$_jc_n" -lt 1 ]; then
        fail "the capture carries no [  throughput: ] row, so it holds no report window at all
  and every arm below would read a zero it was handed rather than one the kernel reported"
    fi
    _jc_i=1
    while [ "$_jc_i" -le "$_jc_n" ]; do
        WIN_TAG="window $_jc_i of $_jc_n: "
        read_report "$(window_text "$1" "$_jc_i")"
        irq_arms
        _jc_i=$((_jc_i + 1))
    done
    WIN_TAG=""
}

irq_arms() {
    # --- probe -----------------------------------------------------------------
    if [ "$iseen" -ne 1 ] || [ "$pseen" -ne 1 ]; then
        fail "the capture carried no irq-probe or no e2e-probe line, so every arm below reads a
      zero it was handed rather than one the kernel reported"
    fi

    # --- the fields those lines carry, each of them parsed ----------------------
    # An unmatched token reads as an in-range zero that disables the arm reading it: the window
    # bounds scope themselves away, the tare floor passes any p50, the close path reads no
    # refusals, and the entry identity takes a foreign count of zero.
    #
    # Five of these have no plant of their own: a report missing irq-probe's raised=,
    # e2e-probe's closed=, either e2e-passes field or a masked probe's raised= reads as a zero a
    # denominator arm already refuses. The check here names the field instead.
    field_parsed() { # <flag> <line> <field>
        if [ "$1" -ne 1 ]; then
            bad "the $2 line carries no readable $3 field; the arm that reads it would take the
      zero the parser hands it for a figure the kernel reported"
        fi
    }
    field_parsed "$ipl" "irq-probe" "line="
    field_parsed "$ipo" "irq-probe" "on="
    field_parsed "$ipr" "irq-probe" "raised="
    field_parsed "$iph" "irq-probe" "hcore="
    field_parsed "$ipf" "irq-probe" "foreign="
    field_parsed "$ipw" "irq-probe" "win="
    field_parsed "$ppl" "e2e-probe" "line="
    field_parsed "$ppc" "e2e-probe" "closed="
    field_parsed "$ppd" "e2e-probe" "dropped="
    field_parsed "$ppt" "e2e-probe" "tare="
    wfield_parsed() { # <probes missing it> <field>
        if [ "$1" -ne 0 ]; then
            bad "$1 of the masked sweep's probe lines carry no readable $2 field; each row is
      judged against its own probe, so the arm that reads it would take a zero for that sweep's
      own figure"
        fi
    }
    wfield_parsed "$wmo" "on="
    wfield_parsed "$wmr" "raised="
    wfield_parsed "$wmh" "hcore="
    wfield_parsed "$wmf" "foreign="
    wfield_parsed "$wmw" "win="
    if [ "$wmlab" -ne 0 ]; then
        bad "$wmlab worst-case row(s) name no span in their label; the row states what width it
      measured there and nowhere else, so an unread one leaves the figure attached to no width
      at all"
    fi

    if [ "$iraised" -le 0 ]; then
        bad "the entry sweep raised line $iline zero times; the kernel never attached it, so the
      row below reports an instrument that did not run rather than a line that did not fire"
    fi
    if [ "$pdropped" -ne 0 ]; then
        bad "the close path refused $pdropped sample(s): an interrupt that never arrived, a wait
      that returned without the thread ever leaving the CPU, a close from a thread that is not
      the woken waiter, or a span over 4.29 s that the close refuses rather than caps. Each
      makes the rows a subset of what the sweep asked for"
    fi

    # --- the sweep's denominator -----------------------------------------------
    if [ "$qseen" -ne 1 ]; then
        bad "the capture carried no e2e-passes line, so the sweep states no denominator and a
      run that abandoned most of its passes reports the rest as the whole population"
    else
        field_parsed "$qpa" "e2e-passes" "asked="
        field_parsed "$qpr" "e2e-passes" "raised="
        if [ "$qraised" -ne "$qasked" ]; then
            bad "the sweep asked for $qasked pass(es) and the kernel let $qraised raise(s)
      through; the rest were abandoned with the waiter never parked, and they close nothing, so
      neither closed nor dropped moves and the rows read as a complete sweep"
        fi
        if [ "$qraised" -ne "$pclosed" ]; then
            bad "the kernel let $qraised raise(s) through and the probe reports $pclosed closed;
      every raise opens a span that owes a counted close, so the difference is spans that ended
      somewhere this accounting does not name"
        fi
    fi

    # --- the settle mark -------------------------------------------------------
    if [ "$tseen" -ne 1 ]; then
        bad "the capture carried no e2e-settle line, so a pass that raised into its own set-up
      reads exactly like one that waited it out"
    elif [ "$tps" -ne 1 ]; then
        bad "the e2e-settle line carries no readable settled=<n>/<passes> field"
    else
        if [ "$tpass" -le 0 ] || [ $((tpass % want)) -ne 0 ]; then
            bad "the e2e-settle line counts $tpass pass(es) on a $want-core kernel; the sweep
      runs the same number of passes on every kernel core"
        fi
        if [ "$tset" -gt "$tpass" ]; then
            bad "the e2e-settle line counts $tset settled pass(es) out of $tpass"
        fi
    fi

    # --- end to end ------------------------------------------------------------
    # One clause and two diagnoses, not two arms: a board that delivers no injected interrupt
    # closes no span either, so the inner test can never refuse a report the outer one passes.
    if [ "$pclosed" -le 0 ]; then
        if [ "$en" -le 0 ] && [ "$iforeign" -eq 0 ]; then
            bad "line $iline was raised $iraised times, no handler entry was seen, and no
      end-to-end span closed either; this board delivers no injected interrupt at all"
        else
            bad "the probe reports no closed end-to-end span on line $pline; the waiter never
      woke, or never reached its close"
        fi
    fi
    if [ $((ln + cn)) -ne "$pclosed" ]; then
        bad "the two locality rows total $((ln + cn)) samples and the probe reports $pclosed
      closed; a sample landed in neither row or in both"
    fi
    if [ "$ln" -ne "$pclosed" ]; then
        bad "every wake is local by construction, the waiter being pinned to the core its line is
      delivered on, and the local row reports $ln of $pclosed closed spans on a $want-core kernel"
    fi

    # --- the clock domain ------------------------------------------------------
    if [ "$wprobes" -ne 4 ]; then
        bad "the masked sweep printed $wprobes probe line(s) beside $wrows row(s); each span
      prints its own probe and its row is read against it, so a missing one leaves a row judged
      against another span's window"
    fi
    if [ "$en" -gt 0 ]; then
        if [ "$ihcore" -eq -2 ]; then
            bad "the entry row carries $en samples stamped on more than one core; whatever each
      sample claimed about itself, a row fed from two cycle counters is a mix of two clocks"
        elif [ "$ihcore" -lt 0 ]; then
            bad "the entry row carries $en samples and its probe names no handler core; the row
      is a subtraction whose far stamp cannot be placed in any clock"
        elif [ "$ihcore" -ne "$ion" ]; then
            bad "the entry sweep ran on core $ion and its handler stamped on core $ihcore; the
      row subtracts two cores' cycle counters, which have no common zero, so its figure is an
      offset and not a latency"
        fi
        if [ "$egl" -eq 0 ] && [ "$iwin" -gt 0 ] && [ "$emax" -gt "$iwin" ]; then
            bad "the entry row reports max=$emax against a raise-to-observation window of $iwin
      measured on the raising core; the handler stamped outside the bracket the raiser held
      around it, so that stamp is not in the raiser's clock whatever core the probe names"
        fi
    fi
    if [ "$wdom" -ne 0 ]; then
        bad "$wdom masked row(s) carry samples whose handler stamped on a core other than the
      one their sweep ran on; each is a difference across two cycle counters with no common zero"
    fi
    if [ "$wbound" -ne 0 ]; then
        bad "$wbound masked row(s) report a maximum above the raise-to-observation window their
      own sweep measured; a stamp the raiser did not bracket is not in the raiser's clock"
    fi
    if [ "$want" -eq 1 ] && { [ "$iforeign" -ne 0 ] || [ "$wforeign" -ne 0 ]; }; then
        bad "at one kernel core there is one cycle counter, and the sweeps refused $iforeign
      entry and $wforeign masked sample(s) as cross-domain; a refusal here is the instrument
      disagreeing with itself and not a line the board delivers elsewhere"
    fi

    # --- population, on rows the sweep actually filled --------------------------
    # Scoped to n>0; the arms above judge an empty row. The gap is a handler that timed out
    # inside the sweep's fixed spin, counted by neither n nor foreign.
    if [ "$en" -gt 0 ] && [ $((en + iforeign)) -ne "$iraised" ]; then
        bad "the entry row carries n=$en beside foreign=$iforeign, which together are
      $((en + iforeign)) against raised=$iraised; the shortfall is a handler that neither
      answered inside the row's window nor was refused as foreign, so n understates who was
      actually raised at"
    fi
    if [ "$wgap" -ne 0 ]; then
        bad "$wgap masked row(s) carry n samples beside their own probe's foreign count that do
      not sum to that probe's raised; the shortfall is a handler that neither answered inside
      the row's window nor was refused as foreign, so n understates who was actually raised at"
    fi

    # --- the entry row, discriminated against the end-to-end one ----------------
    mode="entry n=$en"
    if [ "$en" -le 0 ]; then
        if [ "$iforeign" -gt 0 ]; then
            mode="entry REFUSED ($iforeign of $iraised samples stamped their entry away from
      core $ion where the sweep ran; the two cycle counters share no zero, so this board has no
      raise-to-entry span in one clock to report and the row states none)"
        else
            mode="entry EMPTY (this backend enters the kernel with interrupts masked, so the
      in-syscall spin cannot be reached; the end-to-end span closed $pclosed times on line $pline)"
        fi
    fi
    if [ "$wrows" -ne 4 ]; then
        bad "the masked sweep printed $wrows worst-case row(s); four spans are reported"
    fi
    _wwant=4
    if [ "$en" -le 0 ]; then
        _wwant=0
    fi
    if [ "$wok" -ne "$_wwant" ]; then
        bad "$wok of $wrows worst-case row(s) carried samples where $_wwant did, the entry row
      having n=$en; all five raise the same line, so one firing and another not is the instrument"
    fi
    # Each span has its own slot, cleared at the start of the sweep, so no row may report more
    # samples than one span raised. A sample written to the wrong span's slot, or a slot left
    # uncleared, climbs past it while every row still looks populated.
    if [ "$wbig" -gt "$iraised" ]; then
        bad "a worst-case row reports $wbig samples where one span raises $iraised; a sample
      landed in another span's slot, or the slots were not cleared, so a row carries more than its
      own span"
    fi

    # --- order, on every row the report emitted ---------------------------------
    # Not keyed on whether a row fired: an empty row prints 0/0/0, which is ordered.
    row_order() { # <name> <p50> <p99> <max>
        if [ "$2" -gt "$3" ] || [ "$3" -gt "$4" ]; then
            bad "the $1 reports p50=$2 p99=$3 max=$4, which is not ordered"
            return 1
        fi
        return 0
    }
    # A masked row refused here is kept out of the label arm, which would refuse it again.
    _wcase_ok=1
    row_order "entry distribution" "$e50" "$e99" "$emax"
    row_order "local end-to-end row" "$l50" "$l99" "$lmax"
    row_order "worst-case row for ${s1}B" "$a1" "$b1" "$m1" || _wcase_ok=0
    row_order "worst-case row for ${s2}B" "$a2" "$b2" "$m2" || _wcase_ok=0
    row_order "worst-case row for ${s3}B" "$a3" "$b3" "$m3" || _wcase_ok=0
    row_order "worst-case row for ${s4}B" "$a4" "$b4" "$m4" || _wcase_ok=0

    # --- the floor -------------------------------------------------------------
    # Both figures are nanoseconds from one clock, so no rate enters the comparison.
    if [ "$ln" -gt 0 ] && [ "$l50" -le "$ptare" ]; then
        bad "the local end-to-end p50 is $l50 ns against a tare of $ptare ns; the span is not
      longer than the same userspace read and the same trap with no interrupt in it, so it did not
      enclose a delivery, a dispatch and a wake"
    fi

    # --- the span, and whether this board can see it ----------------------------
    # Scoped to four rows with four readable labels: a short report or an unread label leaves a
    # span reading 0, which an arm above has already refused.
    _labels_ok=0
    if [ "$wrows" -eq 4 ] && [ "$wmlab" -eq 0 ]; then
        _labels_ok=1
        if [ "$s1" -ge "$s2" ] || [ "$s2" -ge "$s3" ] || [ "$s3" -ge "$s4" ]; then
            _labels_ok=0
            bad "the worst-case rows name spans ${s1}B ${s2}B ${s3}B ${s4}B, which do not ascend;
      a sweep that printed them out of order still prints four plausible rows"
        fi
    fi
    _spans="${s1}B=$a1 ${s2}B=$a2 ${s3}B=$a3 ${s4}B=$a4"
    _judged="spread only"
    if [ "$en" -le 0 ]; then
        _judged="not sampled"
    elif [ "$want" -gt 1 ]; then
        # Reached only by a board that delivers locally above one kernel core, which no capture
        # has shown; judging its ordering would assert a shape no run has produced.
        _judged="not judged above one kernel core (no capture has populated these rows there)"
    else
        _grew=1
        if [ "$a2" -lt "$a1" ] || [ "$a3" -lt "$a2" ] || [ "$a4" -lt "$a3" ]; then
            _grew=0
            bad "the worst-case p50s run $_spans, which does not grow with the span. Each row is
      the same raise with a longer masked body in front of it, so a later row below an earlier one
      is a body the mask does not cover or a counter that is not moving"
        fi
        # A percentile is a bucket low edge at an eighth of an octave, so two figures inside w/8
        # of each other are one reading.
        _step=$((a1 / 8))
        if [ "$a4" -le $((a1 + _step)) ]; then
            _grew=0
            bad "the worst-case p50s run $_spans: the ${s4}B row does not clear the ${s1}B row by
      even one bucket ($_step at this magnitude), so a masked body of ${s4} bytes costs nothing
      this instrument can report against one of ${s1}. Either span_bytes reaches no body, or the
      counter these rows are read from is not moving"
        fi
        _judged="grows with the span"
        # Only over rows no arm above has refused, so no row is refused twice.
        if [ "$_grew" -eq 1 ] && [ "$_labels_ok" -eq 1 ] && [ "$_wcase_ok" -eq 1 ] \
           && [ "$wok" -eq 4 ]; then
            span_labels
            _judged="grows with the span, each label priced against the other three"
        fi
    fi

}

# --- the label against the span its own row measured --------------------------
# Each row is the same raise with the bytes its label names in front of it, so between any two
# rows the cost difference over the span difference is one per-byte rate, and the labels are
# consistent exactly when one rate fits all six pairs. A relabelled row still ascends and still
# grows, so only this arm catches it.
#
# The readings are bucket low edges, so a pair bounds the rate rather than fixing it. The slack
# is two buckets per reading: one for the instrument's resolution, one because the body's cost
# is close to proportional to its length but not exactly so. Where the 64-byte body sits inside
# one bucket of the empty one, that row's label is bounded by nothing and only the two wide
# rows are held to their names.
#
# The rate is cycles per KiB of masked body, scaled by 1024.
span_labels() {
    _rate_lo=0
    _rate_hi=0
    _rate_first=1
    span_pair() { # <narrow p50> <narrow span> <wide p50> <wide span>
        _sp_den=$((4 * ($4 - $2)))
        _sp_lo=$((4 * $3 - 5 * $1))
        _sp_hi=$((5 * $3 - 4 * $1))
        # A bound below zero constrains nothing, and the growth arm has already refused the only
        # report that produces one.
        if [ "$_sp_lo" -lt 0 ]; then
            _sp_lo=0
        fi
        if [ "$_sp_hi" -lt 0 ]; then
            _sp_hi=0
        fi
        _sp_lo=$((_sp_lo * 1024 / _sp_den))
        _sp_hi=$(((_sp_hi * 1024 + _sp_den - 1) / _sp_den))
        if [ "$_rate_first" -eq 1 ] || [ "$_sp_lo" -gt "$_rate_lo" ]; then
            _rate_lo=$_sp_lo
        fi
        if [ "$_rate_first" -eq 1 ] || [ "$_sp_hi" -lt "$_rate_hi" ]; then
            _rate_hi=$_sp_hi
        fi
        _rate_first=0
    }
    span_pair "$a1" "$s1" "$a2" "$s2"
    span_pair "$a1" "$s1" "$a3" "$s3"
    span_pair "$a1" "$s1" "$a4" "$s4"
    span_pair "$a2" "$s2" "$a3" "$s3"
    span_pair "$a2" "$s2" "$a4" "$s4"
    span_pair "$a3" "$s3" "$a4" "$s4"
    if [ "$_rate_lo" -gt "$_rate_hi" ]; then
        bad "the worst-case rows run $_spans, and no one per-byte cost fits the four spans they
      name: the pairs put the masked body at $_rate_lo cyc/KiB and above, and at $_rate_hi
      cyc/KiB and below. Each row is the same raise with the bytes its label names in front of
      it, so a row whose label and figure disagree is a published width that was never measured"
    fi
}

# --- the arms, proven on planted reports before the real one ------------------
# Each plant differs from a good report in one figure. An arm whose plant does not fire reports
# every real capture clean.
#
# ctl_one and ctl_four are real reports: one kernel core, where the masked spans separate, and
# four, where they do not and must not be judged.
ctl_one() {
    printf '%s\n' \
'  irq-probe: line=6 raise=inject on=0 raised=100 hcore=0 foreign=0 win=263960' \
'  irq:       1408/3328/172560 cyc  (p50/p99/max, n=100)' \
'  wcase-probe: line=6 raise=inject on=0 raised=100 hcore=0 foreign=0 win=31740' \
'  wcase-irq[0B]:    1536/2048/20220 cyc  (p50/p99/max, n=100)' \
'  wcase-probe: line=6 raise=inject on=0 raised=100 hcore=0 foreign=0 win=23040' \
'  wcase-irq[64B]:   1792/2048/19740 cyc  (p50/p99/max, n=100)' \
'  wcase-probe: line=6 raise=inject on=0 raised=100 hcore=0 foreign=0 win=5600' \
'  wcase-irq[256B]:  2304/3584/3860 cyc  (p50/p99/max, n=100)' \
'  wcase-probe: line=6 raise=inject on=0 raised=100 hcore=0 foreign=0 win=15860' \
'  wcase-irq[1024B]: 4608/6144/6740 cyc  (p50/p99/max, n=100)' \
'  e2e-settle: settled=2/2' \
'  e2e-probe: line=7 closed=50 dropped=0 tare=1500/1828 ns  (min/avg, n=64)' \
'  e2e-passes: asked=50 raised=50' \
'  e2e-local: 8192/106496/114300 ns  (p50/p99/max, n=50)'
}
ctl_four() {
    printf '%s\n' \
'  irq-probe: line=200 raise=inject on=3 raised=100 hcore=0 foreign=100 win=132871' \
'  irq:       0/0/0 cyc  (p50/p99/max, n=0)' \
'  wcase-probe: line=200 raise=inject on=3 raised=100 hcore=0 foreign=100 win=98035' \
'  wcase-irq[0B]:    0/0/0 cyc  (p50/p99/max, n=0)' \
'  wcase-probe: line=200 raise=inject on=3 raised=100 hcore=0 foreign=100 win=17493' \
'  wcase-irq[64B]:   0/0/0 cyc  (p50/p99/max, n=0)' \
'  wcase-probe: line=200 raise=inject on=3 raised=100 hcore=0 foreign=100 win=100469' \
'  wcase-irq[256B]:  0/0/0 cyc  (p50/p99/max, n=0)' \
'  wcase-probe: line=200 raise=inject on=3 raised=100 hcore=0 foreign=100 win=103725' \
'  wcase-irq[1024B]: 0/0/0 cyc  (p50/p99/max, n=0)' \
'  e2e-settle: settled=8/8' \
'  e2e-probe: line=201 closed=200 dropped=0 tare=1104/1363 ns  (min/avg, n=64)' \
'  e2e-passes: asked=200 raised=200' \
'  e2e-local: 15360/276336/276336 ns  (p50/max/max, n=200)' \
'  e2e-cross: 0/0/0 ns  (p50/p99/max, n=0)'
}

# What the four-core boards printed before the domain arms, verbatim, and must be refused: rows
# filled from a peer's cycle counter beside a probe that says so.
ctl_four_offset() {
    printf '%s\n' \
'  irq-probe: line=200 raise=inject on=3 raised=100 hcore=0 foreign=0 win=132871' \
'  irq:       983040/983040/1087713 cyc  (p50/p99/max, n=100)' \
'  wcase-probe: line=200 raise=inject on=3 raised=100 hcore=0 foreign=0 win=98035' \
'  wcase-irq[0B]:    983040/983040/1002594 cyc  (p50/p99/max, n=100)' \
'  wcase-probe: line=200 raise=inject on=3 raised=100 hcore=0 foreign=0 win=17493' \
'  wcase-irq[64B]:   983040/983040/1052727 cyc  (p50/p99/max, n=100)' \
'  wcase-probe: line=200 raise=inject on=3 raised=100 hcore=0 foreign=0 win=100469' \
'  wcase-irq[256B]:  983040/983040/1008384 cyc  (p50/p99/max, n=100)' \
'  wcase-probe: line=200 raise=inject on=3 raised=100 hcore=0 foreign=0 win=103725' \
'  wcase-irq[1024B]: 983040/983040/1007383 cyc  (p50/p99/max, n=100)' \
'  e2e-settle: settled=8/8' \
'  e2e-probe: line=201 closed=200 dropped=0 tare=1104/1363 ns  (min/avg, n=64)' \
'  e2e-passes: asked=200 raised=200' \
'  e2e-local: 98304/360448/378464 ns  (p50/p99/max, n=50)' \
'  e2e-cross: 147456/360448/2752960 ns  (p50/p99/max, n=150)'
}

# A board that delivers its own raise locally above one kernel core. No capture has shown it,
# so a plant is the only place that shape exists. Every clock-domain plant below is this report
# with one field moved.
ctl_four_local() {
    printf '%s\n' \
'  irq-probe: line=200 raise=inject on=3 raised=100 hcore=3 foreign=0 win=263960' \
'  irq:       1408/3328/172560 cyc  (p50/p99/max, n=100)' \
'  wcase-probe: line=200 raise=inject on=3 raised=100 hcore=3 foreign=0 win=31740' \
'  wcase-irq[0B]:    1536/2048/20220 cyc  (p50/p99/max, n=100)' \
'  wcase-probe: line=200 raise=inject on=3 raised=100 hcore=3 foreign=0 win=23040' \
'  wcase-irq[64B]:   1792/2048/19740 cyc  (p50/p99/max, n=100)' \
'  wcase-probe: line=200 raise=inject on=3 raised=100 hcore=3 foreign=0 win=5600' \
'  wcase-irq[256B]:  2304/3584/3860 cyc  (p50/p99/max, n=100)' \
'  wcase-probe: line=200 raise=inject on=3 raised=100 hcore=3 foreign=0 win=15860' \
'  wcase-irq[1024B]: 4608/6144/6740 cyc  (p50/p99/max, n=100)' \
'  e2e-settle: settled=8/8' \
'  e2e-probe: line=201 closed=200 dropped=0 tare=1104/1363 ns  (min/avg, n=64)' \
'  e2e-passes: asked=200 raised=200' \
'  e2e-local: 15360/276336/276336 ns  (p50/max/max, n=200)' \
'  e2e-cross: 0/0/0 ns  (p50/p99/max, n=0)'
}

# <name> <kernel cores> <expect: pass|refuse> <report text>
ctl() {
    _ctl_save=$want
    want=$2
    # A subshell, because fail exits and would otherwise end the whole control suite.
    ( rc=0; read_report "$4"; irq_arms 2> /dev/null; exit "$rc" )
    _ctl_got=$?
    want=$_ctl_save
    if [ "$3" = pass ] && [ "$_ctl_got" -ne 0 ]; then
        fail "the '$1' control is REFUSED, and it is a report this gate must accept; every
  capture below would be refused for the same reason"
    fi
    if [ "$3" = refuse ] && [ "$_ctl_got" -eq 0 ]; then
        fail "the '$1' control PASSES, so this gate cannot see that class and a capture
  carrying it reads as clean"
    fi
}

# Every refuse plant below is a report that passes but for the single field its name points at,
# and the two whole-shape controls that cannot meet that standard say so themselves.
ctl 'one core, spans separating' 1 pass "$(ctl_one)"
# Keys the span arm on the core count: flat rows pass at four kernel cores, as the emulator
# reports there, and flat rows at one core are refused below.
ctl 'four cores, spans flat' 4 pass "$(ctl_four)"
ctl 'four cores delivering locally' 4 pass "$(ctl_four_local)"

# --- the probe lines, and the raise denominator under them --------------------
# Not isolating, and it cannot be: a report with no irq-probe line carries raised=0 and one
# with no e2e-probe line carries closed=0, which those arms refuse anyway. The guard buys a
# single finding instead of a cascade read off handed zeros.
ctl 'a report with no end-to-end probe line' 1 refuse \
    "$(ctl_one | sed '/^  e2e-probe: /d')"
# A line the kernel never attached: every sweep raised it zero times and every row is empty.
# Isolates the raise denominator.
ctl 'a line that was never raised' 1 refuse \
    "$(ctl_one | sed 's|raised=100|raised=0|
                      s|n=100)|n=0)|
                      s|^\(  irq: *\)[0-9]*/[0-9]*/[0-9]*|\10/0/0|
                      s|^\(  wcase-irq\[[0-9]*B\]: *\)[0-9]*/[0-9]*/[0-9]*|\10/0/0|')"
# Two refused closes beside a sweep whose every raise closed: `raised` and `closed` still
# agree, so the denominator arms cannot reach it. Isolates the dropped count.
ctl 'closes the sweep refused' 1 refuse \
    "$(ctl_one | sed 's|dropped=0|dropped=2|')"

# --- a field that did not parse, one plant per field -------------------------
# Every plant here deletes a field whose handed zero moves no arm that reads the value, so only
# the parse check refuses it. The five fields a denominator arm already covers have no plant.
ctl 'an entry probe with no line number' 1 refuse \
    "$(ctl_one | sed 's|^\(  irq-probe: \)line=6 |\1|')"
ctl 'an entry probe that names no sweep core' 1 refuse \
    "$(ctl_one | sed 's|^\(  irq-probe: line=6 raise=inject \)on=0 |\1|')"
# The masked rows are empty here, so nothing reads the handler core and only the parse refuses
# it.
ctl 'an entry probe that names no handler core at all' 4 refuse \
    "$(ctl_four | sed 's|^\(  irq-probe: .*\)hcore=0 |\1|')"
ctl 'an entry probe with no refusal count' 1 refuse \
    "$(ctl_one | sed 's|^\(  irq-probe: .*\)foreign=0 |\1|')"
# Deleted, the entry window scopes its bound away and the row may report any maximum.
ctl 'an entry probe with no window' 1 refuse \
    "$(ctl_one | sed 's|^\(  irq-probe: .*\) win=263960|\1|')"
ctl 'a masked probe that names no sweep core' 1 refuse \
    "$(ctl_one | sed 's| on=0 raised=100 hcore=0 foreign=0 win=23040| raised=100 hcore=0 foreign=0 win=23040|')"
ctl 'a masked probe that names no handler core at all' 4 refuse \
    "$(ctl_four | sed 's| hcore=0 foreign=100 win=17493| foreign=100 win=17493|')"
ctl 'a masked probe with no refusal count' 1 refuse \
    "$(ctl_one | sed 's| foreign=0 win=23040| win=23040|')"
ctl 'a masked probe with no window' 1 refuse \
    "$(ctl_one | sed 's| win=5600||')"
ctl 'an end-to-end probe with no line number' 1 refuse \
    "$(ctl_one | sed 's|^\(  e2e-probe: \)line=7 |\1|')"
ctl 'an end-to-end probe with no refusal count' 1 refuse \
    "$(ctl_one | sed 's| dropped=0||')"
# Corrupted rather than deleted, as a torn console line leaves it: the floor would then compare
# every p50 against a tare of zero.
ctl 'an end-to-end probe whose tare does not parse' 1 refuse \
    "$(ctl_one | sed 's|tare=1500/1828|tare=?/?|')"
# The widest row, so the other three still ascend and the order arm is scoped away rather than
# tripped.
ctl 'a worst-case row that names no span' 1 refuse \
    "$(ctl_one | sed 's|wcase-irq\[1024B\]:|wcase-irq[]:      |')"

# --- the sweep's denominator, one clause per plant ---------------------------
# The raiser gives a pass up after its retry bound and reports nothing: thirteen abandoned and
# the rest closing cleanly reads as a full report.
ctl 'a sweep that abandoned passes' 1 refuse \
    "$(ctl_one \
        | sed 's|asked=50 raised=50|asked=50 raised=37|' \
        | sed 's|closed=50|closed=37|' \
        | sed 's|^\(  e2e-local: .*\)n=50)|\1n=37)|')"
# Every pass raised and a quarter of them closing nowhere. The locality total still agrees with
# `closed`, so the e2e arm cannot reach it.
ctl 'raises that closed nothing' 1 refuse \
    "$(ctl_one \
        | sed 's|closed=50|closed=37|' \
        | sed 's|^\(  e2e-local: .*\)n=50)|\1n=37)|')"
ctl 'a sweep with no denominator at all' 1 refuse \
    "$(ctl_one | sed '/^  e2e-passes: /d')"

# --- the settle mark, one clause per plant ------------------------------------
# A short count is the one shape the arm lets through, the pass having raised anyway.
ctl 'a pass that raised before its set-up settled' 1 pass \
    "$(ctl_one | sed 's|settled=2/2|settled=1/2|')"
ctl 'a sweep with no settle mark' 1 refuse \
    "$(ctl_one | sed '/^  e2e-settle: /d')"
ctl 'a settle mark that does not parse' 1 refuse \
    "$(ctl_one | sed 's|settled=2/2|settled=2|')"
ctl 'a settle mark counting no pass' 1 refuse \
    "$(ctl_one | sed 's|settled=2/2|settled=0/0|')"
ctl 'a settle mark whose passes skip a core' 4 refuse \
    "$(ctl_four | sed 's|settled=8/8|settled=6/6|')"
ctl 'a settle mark past its own passes' 1 refuse \
    "$(ctl_one | sed 's|settled=2/2|settled=3/2|')"

# --- the end-to-end span, and the two diagnoses of an empty one ---------------
# A sweep that ran its passes and closed nothing. asked and raised move with closed, so the
# three denominator arms are satisfied and only the closed count refuses it.
ctl 'a sweep that closed no span' 1 refuse \
    "$(ctl_one \
        | sed 's|closed=50|closed=0|
               s|asked=50 raised=50|asked=0 raised=0|
               s|^\(  e2e-local: \)[0-9/]* ns  (p50/p99/max, n=50)|\10/0/0 ns  (p50/p99/max, n=0)|')"
# The same, with the entry and masked rows empty and nothing refused as foreign: the board
# delivers no injected interrupt at all, which is the other branch of that clause.
ctl 'a board that delivers no injected interrupt' 1 refuse \
    "$(ctl_one | sed 's|n=100)|n=0)|
                      s|^\(  irq: *\)[0-9]*/[0-9]*/[0-9]*|\10/0/0|
                      s|^\(  wcase-irq\[[0-9]*B\]: *\)[0-9]*/[0-9]*/[0-9]*|\10/0/0|
                      s|closed=50|closed=0|
                      s|asked=50 raised=50|asked=0 raised=0|
                      s|^\(  e2e-local: \)[0-9/]* ns  (p50/p99/max, n=50)|\10/0/0 ns  (p50/p99/max, n=0)|')"
# One sample counted in both rows. The local row still carries every closed span, so only the
# total refuses it.
ctl 'a closed span in both locality rows' 4 refuse \
    "$(ctl_four | sed 's|^\(  e2e-cross: \)0/0/0 ns  (p50/p99/max, n=0)|\18192/8192/8192 ns  (p50/p99/max, n=1)|')"
# One sample classified cross above one kernel core, where the pin makes every wake local. The
# rows still total what closed, so only the locality clause refuses it.
ctl 'a cross-core sample above one kernel core' 4 refuse \
    "$(ctl_four | sed 's|^\(  e2e-local: .*\)n=200)|\1n=199)|
                       s|^\(  e2e-cross: \)0/0/0 ns  (p50/p99/max, n=0)|\18192/8192/8192 ns  (p50/p99/max, n=1)|')"
# The same at ONE kernel core, where the cross row is not even declared.
ctl 'a cross-core sample at one kernel core' 1 refuse \
    "$(ctl_one | sed 's|^\(  e2e-local: .*\)n=50)|\1n=49)|'
       printf '%s\n' '  e2e-cross: 8192/8192/8192 ns  (p50/p99/max, n=1)')"

# --- the clock domain, one plant per statement -------------------------------
# The whole-shape control and the only plant here that trips several arms. It witnesses no
# single arm; each has its own plant below.
ctl 'rows filled from a peer core' 4 refuse "$(ctl_four_offset)"
ctl 'an entry row stamped on several cores' 4 refuse \
    "$(ctl_four_local | sed 's|^\(  irq-probe: .*\)hcore=3|\1hcore=mixed|')"
ctl 'an entry row whose probe names no handler core' 4 refuse \
    "$(ctl_four_local | sed 's|^\(  irq-probe: .*\)hcore=3|\1hcore=none|')"
ctl 'an entry row stamped on a peer core' 4 refuse \
    "$(ctl_four_local | sed 's|^\(  irq-probe: .*\)hcore=3|\1hcore=0|')"
# The probe names the sweep's core and only the arithmetic refuses it: a classifier stuck on
# "local" that also reports the expected core.
ctl 'an entry row outside its own window' 4 refuse \
    "$(ctl_four_local | sed 's|^\(  irq-probe: .*\)win=263960|\1win=100000|')"
ctl 'a masked row stamped on a peer core' 4 refuse \
    "$(ctl_four_local | sed 's|^\(  wcase-probe: .*\)hcore=3\( foreign=0 win=23040\)|\1hcore=0\2|')"
ctl 'a masked row outside its own window' 1 refuse \
    "$(ctl_one | sed 's|^\(  wcase-probe: .*\)win=5600|\1win=3000|')"
# One sample refused at ONE kernel core. The row is cut to 99 so it still owes its own probe
# exactly, keeping the population arm out of this one.
ctl 'a refusal at one kernel core' 1 refuse \
    "$(ctl_one | sed 's|^\(  irq-probe: .*\)foreign=0|\1foreign=1|
                      s|^\(  irq: *1408/3328/172560 cyc  (p50/p99/max, \)n=100)|\1n=99)|')"

# --- population, one plant per row shape -------------------------------------
# Both leave raised=100 and foreign=0 beside a row cut to n=10: handlers timing out inside the
# sweep's fixed spin, landing in neither count.
ctl 'the entry row short of its own raise count' 1 refuse \
    "$(ctl_one | sed 's|^\(  irq:       1408/3328/172560 cyc  (p50/p99/max, \)n=100)|\1n=10)|')"
ctl 'a masked row short of its own raise count' 1 refuse \
    "$(ctl_one | sed 's|^\(  wcase-irq\[64B\]:   1792/2048/19740 cyc  (p50/p99/max, \)n=100)|\1n=10)|')"

# --- the four masked rows: how many arrived, which fired, and how many samples -
# The entry row is empty here, so the populated-row count still owes zero, and the label arm
# is scoped past a short report: only the row count refuses this one.
ctl 'a masked sweep that printed three rows' 4 refuse \
    "$(ctl_four | sed '/^  wcase-irq\[256B\]:/d')"
# The empty row is the first, so the p50s still ascend and clear each other, and only the
# count of populated rows refuses it.
ctl 'a masked row that fired where its siblings did not' 1 refuse \
    "$(ctl_one | sed 's|^\(  wcase-irq\[0B\]: *\)1536/2048/20220 cyc  (p50/p99/max, n=100)|\10/0/0 cyc  (p50/p99/max, n=0)|')"
# Each row holding every earlier row's samples, as a slot not cleared at the start of the sweep
# or a sample landing in another span's slot leaves it. Each probe's raised climbs with it, so
# every row still owes its own probe exactly and the population arm is silent; only the count
# against the raises one span makes refuses it.
ctl 'a masked slot carried across the spans' 1 refuse \
    "$(ctl_one | sed 's|^\(  wcase-probe: .*\)raised=100\( hcore=0 foreign=0 win=23040\)|\1raised=200\2|
                      s|^\(  wcase-probe: .*\)raised=100\( hcore=0 foreign=0 win=5600\)|\1raised=300\2|
                      s|^\(  wcase-probe: .*\)raised=100\( hcore=0 foreign=0 win=15860\)|\1raised=400\2|
                      s|^\(  wcase-irq\[64B\]: *1792/2048/19740 cyc  (p50/p99/max, \)n=100)|\1n=200)|
                      s|^\(  wcase-irq\[256B\]: *2304/3584/3860 cyc  (p50/p99/max, \)n=100)|\1n=300)|
                      s|^\(  wcase-irq\[1024B\]: *4608/6144/6740 cyc  (p50/p99/max, \)n=100)|\1n=400)|')"
# The last probe is the one deleted, so the row below it reads the 256B probe's window; its
# figures come down under that window, leaving the probe count as the only thing wrong.
ctl 'a masked row with no probe of its own' 1 refuse \
    "$(ctl_one | sed '/^  wcase-probe: .*win=15860$/d
                      s|^\(  wcase-irq\[1024B\]: \)4608/6144/6740|\14608/5120/5400|')"

# --- the distributions the rows report ---------------------------------------
ctl 'flat spans at one core' 1 refuse \
    "$(ctl_one | sed 's|^\(  wcase-irq\[[0-9]*B\]:  *\)[0-9]*/|\11536/|')"
ctl 'spans that stop growing' 1 refuse \
    "$(ctl_one | sed 's|^\(  wcase-irq\[1024B\]:  *\)[0-9]*/|\11792/|')"
ctl 'spans reported out of order' 1 refuse \
    "$(ctl_one | sed 's|wcase-irq\[64B\]|wcase-irq[256B]|; s|wcase-irq\[256B\]:  2304|wcase-irq[64B]:  2304|')"
# One row renamed to four times the span it measured, every figure left alone: the labels
# still ascend, the p50s still grow and the widest still clears the narrowest, so only the label
# arm refuses it.
ctl 'a row naming a span it did not measure' 1 refuse \
    "$(ctl_one | sed 's|wcase-irq\[1024B\]:|wcase-irq[4096B]:|')"
# The same row renamed down, still ascending behind the 256B one, pricing its bytes above its
# siblings rather than below. An arm cut to the over-claim alone reports this one clean.
ctl 'a row naming a span narrower than the one it measured' 1 refuse \
    "$(ctl_one | sed 's|wcase-irq\[1024B\]:|wcase-irq[512B]: |')"
ctl 'a worst-case row out of order' 1 refuse \
    "$(ctl_one | sed 's|^\(  wcase-irq\[256B\]:  *\)2304/3584/3860|\13584/2304/3860|')"
ctl 'the local row out of order' 4 refuse \
    "$(ctl_four | sed 's|^\(  e2e-local: \)15360/276336/276336|\1276336/15360/276336|')"
ctl 'the local row at the tare' 4 refuse \
    "$(ctl_four | sed 's|^\(  e2e-local: \)15360/|\11104/|')"

# --- the recorded-capture path, which no control above reaches -----------------
# Every silicon verdict comes through BENCH_CAPTURE=, which differs from the polled path in the
# CR every console line ends in and in holding several report windows. The broken window is the
# 'sweep that abandoned passes' report entire, so these controls prove which windows are read
# rather than which arm reads them.
ctl_windows() { # <how many> <broken index, 0 for none>
    _cw_i=1
    while [ "$_cw_i" -le "$1" ]; do
        printf '%s\n' \
'  throughput: 42813 ctx-sw/s  (23357 ns/sw avg over 40000 switches / 934 ms)' \
'  switch:    83/83/308 cyc  576/576/2138 ns  (min/avg/max, n=40002)'
        if [ "$_cw_i" = "$2" ]; then
            ctl_one | sed 's|asked=50 raised=50|asked=50 raised=37|
                           s|closed=50|closed=37|
                           s|^\(  e2e-local: .*\)n=50)|\1n=37)|'
        else
            ctl_one
        fi
        _cw_i=$((_cw_i + 1))
    done
}

# <name> <expect: pass|refuse> <capture text>. Written with the CR the console leaves on every
# line and read back the way the recorded path reads it, so the strip is under control too.
_CR=$(printf '\r')
ctl_capture() {
    _cc_f="$TMP/planted_capture.log"
    # A literal CR through a variable: \r in a sed REPLACEMENT is a GNU extension, and this
    # script is run by /bin/sh.
    printf '%s\n' "$3" | sed "s/\$/$_CR/" > "$_cc_f"
    _cc_save=$want
    want=1
    ( rc=0; judge_capture "$(tr -d '\r' < "$_cc_f")" 2> /dev/null; exit "$rc" )
    _cc_got=$?
    want=$_cc_save
    if [ "$2" = pass ] && [ "$_cc_got" -ne 0 ]; then
        fail "the '$1' capture control is REFUSED, and it is a capture this gate must accept;
  every silicon capture would be refused for the same reason"
    fi
    if [ "$2" = refuse ] && [ "$_cc_got" -eq 0 ]; then
        fail "the '$1' capture control PASSES, so this gate cannot see that class and a silicon
  capture carrying it reads as clean"
    fi
}

scratch_dir
ctl_capture 'three clean windows, CRLF' pass "$(ctl_windows 3 0)"
# Nothing in windows one and two is wrong, so only reading the third refuses it.
ctl_capture 'a third window short of its denominator' refuse "$(ctl_windows 3 3)"
ctl_capture 'a first window short of its denominator' refuse "$(ctl_windows 3 1)"
# A complete report and no throughput row above it: the loop runs zero times and the verdict
# is the window count alone.
ctl_capture 'a capture holding no report window' refuse "$(ctl_one)"

echo "== control: each arm fires on a planted report that no other arm refuses, four flat
  spans pass at four kernel cores and are refused at one, a field deleted from a line that still
  agrees with it is refused by name rather than disabling the arm that reads it, a row renamed to
  a span it did not measure is refused either way it is renamed, a sweep short of its own
  denominator is refused, the six-figure report the four-core boards printed before the domain
  arms existed is refused whole, and a three-window CRLF capture is judged window by window
  rather than on its first =="

if [ "$controls_only" -eq 1 ]; then
    echo "PASS: every arm above went red on a report planted to break it and green on the two
  real ones; no image was run"
    exit 0
fi

rc=0
if [ -n "${BENCH_CAPTURE:-}" ]; then
    judge_capture "$OUT"
    _windows=$(window_count "$OUT")
else
    read_report "$OUT"
    irq_arms
    _windows=1
fi

assert_no_panic "the bench image panicked while the IRQ sweep ran"

if [ "$rc" -ne 0 ]; then
    exit 1
fi
_split="local n=$ln (one core, so every wake is local)"
if [ "$want" -gt 1 ]; then
    _split="local n=$ln cross n=$cn"
fi
echo "PASS: $_windows report window(s) judged; in the last, line $iline raised $iraised,
      $mode; line $pline closed $pclosed end to end; $_split; $tset of $tpass pass(es) settled"
if [ "$tset" -lt "$tpass" ]; then
    echo "NOTE: $((tpass - tset)) pass(es) raised before their set-up settled, so their first
      sample may carry it"
fi
echo "      masked spans $_spans ($_judged)"
exit 0
