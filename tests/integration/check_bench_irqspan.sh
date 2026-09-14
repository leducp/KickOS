#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Gate on the IRQ distributions the bench reports: the inject-to-handler row, the four masked
# worst-case spans, and the END-TO-END span from the raise to the woken userspace thread's
# first read of its device window, split by whether that thread ran on the core that took the
# interrupt.
#
# A BOARD THAT CANNOT INJECT PRINTS A COMPLETE, PLAUSIBLE REPORT OF ZEROS. So nothing here is
# read off a cycle figure before its sample count has been refused.
#
# THE TWO INSTRUMENTS DISCRIMINATE EACH OTHER, which is why one gate reads both. The
# inject-to-handler row spins for the handler INSIDE a syscall, so it is empty on a backend
# that enters the kernel with interrupts masked; the end-to-end span leaves the kernel between
# the raise and the wake, so it is not. An empty entry row beside closed end-to-end spans is
# therefore that backend and not a dead line, and an empty entry row beside NO closed span is a
# board that cannot deliver an injected interrupt at all. Only the second is a failure.
#
# The arms, and what each one breaks on:
#   probe    both probe lines exist, and the raise denominator is non-zero. n=0 beside
#            raised=0 is a line the kernel never attached, which the row alone cannot show.
#   domain   THE ENTRY AND MASKED ROWS ARE SUBTRACTIONS, so both their stamps must come out of
#            one counter. Per-core cycle counters have no common zero, and a difference taken
#            across two of them is an offset that reads as a plausible latency and is not one.
#            THREE STATEMENTS PER ROW, EACH FAILING WHERE THE OTHERS PASS. The probe names ONE
#            core its handler stamped on, and a row carrying samples was filled on the core the
#            sweep ran on; `hcore=mixed` is a row fed from several clocks however each of its
#            samples was classified. And the row's maximum is held
#            under the raise-to-observation window the RAISING core measured, which is
#            arithmetic no classifier can talk its way out of: a stamp the raiser did not
#            bracket is not in the raiser's clock, whatever core the probe names. That last one
#            is what catches a classifier stuck on "local". It says nothing where the counter
#            is frozen or glitches, which the report declares by heading its rows min/avg/max,
#            and where no offset can hide either.
#
#            REFUSING IS NOT FAILING ABOVE ONE CORE. A controller that delivers the line to a
#            core other than the one that raised it leaves no raise-to-entry span in one clock
#            to report, and an empty row beside a non-zero foreign count is the honest answer.
#            At ONE kernel core there is one clock, so a refusal there is the instrument
#            disagreeing with itself and not a line the board delivers elsewhere.
#   population   a populated row (n>0) owes its own probe's raised exactly: n plus that
#            probe's own foreign must sum to it. A handler that times out inside the sweep's
#            fixed spin lands in neither count, so a row can carry fewer samples than it was
#            raised at and still read raised>0. n=0 is not judged here: a line never attached
#            and a raise that reached no handler both print it, and the arms above tell those
#            two apart by a better signal than the row's own emptiness.
#   entry    the entry row carried samples, OR the sweep refused every one as foreign, OR the
#            end-to-end span shows this backend masks its syscalls. A DESCRIPTION and not a
#            verdict: where none of the three holds, no span closed either, and the e2e clause
#            below is what refuses that.
#   wcase    four masked spans reported, each carrying samples exactly when the entry row
#            does, and none of them more than one span's worth. The first refuses a row that
#            fired where its siblings did not; the second refuses a shared accumulator that
#            was not cleared between spans, which leaves every row populated and climbing.
#            Exact counts are NOT compared across the five: the entry spin can time out on a
#            contended emulator while the masked one, which raises under its own mask and
#            releases it, does not.
#   span     the four rows' labels ascend where four of them arrived, and at ONE kernel core
#            their p50s grow with the span and the widest clears the narrowest by more than a
#            bucket. Counting samples says nothing here: four identical rows carry the same
#            counts as four that separate, which is what an ignored span_bytes leaves behind.
#   e2e      the two locality rows total exactly what the probe says was closed, the close path
#            refused nothing, and some span closed at all. That last one carries two diagnoses
#            under one clause: an empty entry row and nothing refused as foreign beside it is a
#            board that delivers no injected interrupt, and anything else is a waiter that never
#            woke. They are branches and not arms, the first implying the second.
#   passes   THE SWEEP'S OWN DENOMINATOR, which closed and dropped are not. A pass whose waiter
#            never parks is abandoned by the app after its retry bound and closes nothing, so
#            neither counter moves and the rows report the passes that ran as though they were
#            all of them. `asked` is the sweep size the app ran, `raised` what the kernel let
#            through, and every raise owes a counted close. Same job as irq-probe's `raised=`,
#            which the kernel counts alone only because that sweep's loop is the kernel's.
#   floor    THE ANTI-VACUITY ARM, on BOTH end-to-end rows. Each p50 exceeds the TARE, which is
#            the same userspace read and the same trap with no interrupt in it. A span that
#            closed early reads as a suspiciously fast figure and nothing else says so.
#   local    above one kernel core both populations carried at least the share the sweep
#            CONSTRUCTS, not merely one sample: the sweep places the raiser and the waiter and
#            runs a same-core and a next-core pass per core, so each population is one pass in
#            2*want of what closed. A sweep that placed only one end left the other row to
#            coincidence and reported single digits on a backend that delivers a line to the
#            hart that injected it. At one core the cross slot does not exist and every wake is
#            local by construction, so the arm there is that local equals closed.
#   order    p50 <= p99 <= max on every row the report emits, the cross-core one and the four
#            masked ones included.
#
# WHAT THIS GATE CANNOT SEE. Above one kernel core no board here delivers an injected line to
# the core that raised it, so the domain arms refuse every sample and the masked rows arrive
# empty. The span arm therefore reports and judges nothing there: a board that did deliver
# locally would fill those rows honestly, and no capture has ever shown one. A board whose cycle counter is frozen fails the span arm at one core rather than
# passing it, which is the right way round but says nothing about the counter on its own; the
# capture chain and check_bench_cyccnt.sh are what settle that.
#
# TWO WAYS IN. With an image it does all of the above. With --controls it runs the planted
# reports alone and exits, which is every arm that reads a report's SHAPE and none that reads a
# timing figure: the plants are text and finish before the capture is touched. The capture half
# is a latency measurement off a machine whose load nobody controls and stays out of CI.
#
# EVERY CLAUSE ABOVE OWES A PLANT NO OTHER CLAUSE REFUSES. A plant that trips two arms proves
# neither: remove the arm it was written for and the report is still refused, so the control
# stays green and the hole it was covering does not show. The one exception is named where it
# sits: a report missing a probe LINE carries raised=0 or closed=0, so the guard that stops on
# it can never refuse a report the two arms below it would let through, and it is proven as a
# shape rather than as an arm of its own.
#
# usage: check_bench_irqspan.sh <elf> <kernel cores>
#        check_bench_irqspan.sh --controls
#        BENCH_CAPTURE=<log> check_bench_irqspan.sh <kernel cores>    reads a recorded capture
#                                                    instead of booting, for a silicon run
#
# THE RECORDED PATH JUDGES EVERY REPORT WINDOW AND THE POLLED ONE JUDGES THE FIRST. A silicon
# capture holds one report per throughput row, and read globally the first window's
# accounting answers for every later one that dropped its own, which is the hole
# tools/bench/bench-capture.sh's bench_windows closes on its side. A poll has no such
# capture to read: it stops the image on the first complete block by construction, so what
# follows that block is a window the run was cut off in the middle of.

set -u
. "$(dirname "$0")/../lib/gate.sh"

# gate.sh's two verdict helpers, tagged with the report window a finding came from. A capture
# carries one report per throughput row and every arm below names a row and not a window, so an
# untagged finding from a three-window capture cannot be placed.
WIN_TAG=""
fail() { echo "FAIL: $WIN_TAG$*" >&2; exit 1; }
bad() { echo "FAIL: $WIN_TAG$*" >&2; rc=1; }
# The call/reply sweep ahead of the IRQ block is a quarter of a million round trips, and the
# end-to-end sweep parks and wakes a thread once per sample on every core in turn. gate.sh's
# own default would kill the image before it printed anything this gate reads.
: "${QEMU_TIMEOUT:=400}"

_usage="usage: check_bench_irqspan.sh <elf> <kernel cores> | check_bench_irqspan.sh --controls |
       BENCH_CAPTURE=<log> check_bench_irqspan.sh <kernel cores>"
controls_only=0
if [ -n "${BENCH_CAPTURE:-}" ]; then
    [ -f "$BENCH_CAPTURE" ] || fail "no capture at $BENCH_CAPTURE"
    elf=""
    want="${1:?$_usage}"
    require_number "$want" "the kernel core count"
    # A silicon capture arrives CRLF and tools/bench/ keeps the CR on purpose. No arm below
    # reads a line ending, so stripping it here loses nothing this gate is entitled to.
    OUT="$(tr -d '\r' < "$BENCH_CAPTURE")"
elif [ "${1:-}" = "--controls" ]; then
    controls_only=1
    # Every ctl call sets `want` from its own argument and restores it; this is what the
    # unset-variable guard needs before the first of them.
    want=1
    elf=""
else
    elf="${1:?$_usage}"
    want="${2:?$_usage}"

    require_number "$want" "the kernel core count"
    [ -f "$elf" ] || fail "no image at $elf"
    need_qemu_machine

    # No `$` anchor on a poll pattern: the poll greps the RAW log and a console under
    # KICKOS_CONSOLE_CRLF leaves a carriage return before every newline. The awk pass below
    # reads $OUT, which poll_image has already stripped.
    #
    # THE PATTERN MATCHES THE ROW'S LAST FIELD AND NOT ITS LABEL, because the poll reads a log
    # the image is still writing and grep answers on a partial final line. A label-only pattern
    # is satisfied by the bytes `  e2e-local:` alone, and the stop that follows cuts the row off
    # before its figures: the arms below then read a row that arrived with no numbers in it and
    # refuse a board that was working. `n=<count>)` is the row's own last token, so nothing but a
    # whole row satisfies this.
    #
    # AND THE CROSS ROW IS A SECOND PATTERN WHERE THE KERNEL HAS SEVERAL CORES, since it prints
    # AFTER the local one: stopping on the local row alone leaves it never printed at all.
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
    # ONE PASS over the capture, so every figure compared below comes out of the same report. Only
    # the FIRST report is read: the bench prints one per throughput window and a poll that raced
    # the second would mix two windows' counts.
    read -r iseen iline iraised en e50 e99 emax wrows wok wbig pseen pline pclosed pdropped ptare \
            l50 l99 lmax ln cn c50 c99 cmax egl ion ihcore iforeign iwin wprobes wforeign \
            wdom wbound wgap qseen qasked qraised \
            s1 a1 b1 m1 s2 a2 b2 m2 s3 a3 b3 m3 s4 a4 b4 m4 <<EOF
$(printf '%s\n' "$1" | awk '
    function tail_n(s,   t) { t = s; sub(/.*n=/, "", t); sub(/[^0-9].*$/, "", t); return t + 0 }
    function part(s, i,   f) { split(s, f, "/"); return f[i] + 0 }

    /^  irq-probe:/ && iseen == 0 {
        iseen = 1
        ihcore = -1
        for (i = 2; i <= NF; i++) {
            if ($i ~ /^line=[0-9]+$/)    { iline    = substr($i, 6) + 0 }
            if ($i ~ /^on=[0-9]+$/)      { ion      = substr($i, 4) + 0 }
            if ($i ~ /^raised=[0-9]+$/)  { iraised  = substr($i, 8) + 0 }
            if ($i ~ /^hcore=[0-9]+$/)   { ihcore   = substr($i, 7) + 0 }
            if ($i == "hcore=mixed")     { ihcore   = -2 }
            if ($i ~ /^foreign=[0-9]+$/) { iforeign = substr($i, 9) + 0 }
            if ($i ~ /^win=[0-9]+$/)     { iwin     = substr($i, 5) + 0 }
        }
        next
    }
    # PAIRED WITH THE ROW BELOW IT, not summed: each masked span is its own sweep with its own
    # placement and its own window, so a row is read against the probe it belongs to. wpforeign
    # and wpraised hold the values this probe just printed, kept beside the running total in
    # wforeign.
    /^  wcase-probe:/ && wprobes < 4 {
        wprobes++
        wphc = -1; wpon = 0; wpwin = 0; wpforeign = 0; wpraised = 0
        for (i = 2; i <= NF; i++) {
            if ($i ~ /^on=[0-9]+$/)      { wpon = substr($i, 4) + 0 }
            if ($i ~ /^raised=[0-9]+$/)  { wpraised = substr($i, 8) + 0 }
            if ($i ~ /^hcore=[0-9]+$/)   { wphc = substr($i, 7) + 0 }
            if ($i == "hcore=mixed")     { wphc = -2 }
            if ($i ~ /^foreign=[0-9]+$/) { wpforeign = substr($i, 9) + 0; wforeign += wpforeign }
            if ($i ~ /^win=[0-9]+$/)     { wpwin = substr($i, 5) + 0 }
        }
        next
    }
    /^  e2e-probe:/ && pseen == 0 {
        pseen = 1
        for (i = 2; i <= NF; i++) {
            if ($i ~ /^line=[0-9]+$/)    { pline    = substr($i, 6) + 0 }
            if ($i ~ /^closed=[0-9]+$/)  { pclosed  = substr($i, 8) + 0 }
            if ($i ~ /^dropped=[0-9]+$/) { pdropped = substr($i, 9) + 0 }
            if ($i ~ /^tare=[0-9]+\/[0-9]+$/) { ptare = part(substr($i, 6), 1) }
        }
        next
    }
    /^  e2e-passes:/ && qseen == 0 {
        qseen = 1
        for (i = 2; i <= NF; i++) {
            if ($i ~ /^asked=[0-9]+$/)  { qasked  = substr($i, 7) + 0 }
            if ($i ~ /^raised=[0-9]+$/) { qraised = substr($i, 8) + 0 }
        }
        next
    }
    /^  irq:/ && eseen == 0 {
        eseen = 1
        e50 = part($2, 1); e99 = part($2, 2); emax = part($2, 3); en = tail_n($0)
        # A GLITCHING OR FROZEN COUNTER DECLARES ITSELF IN THE HEADING, the kernel printing
        # min/avg/max there instead of percentiles. The window bound has nothing to say on such
        # a part: a glitched read can only inflate, and an inflated stamp is not an offset.
        if (index($0, "(min/avg/max")) { egl = 1 }
        next
    }
    /^  wcase-irq\[/ && wrows < 4 {
        wrows++
        n = tail_n($0)
        if (n > 0) { wok++ }
        if (n > wbig) { wbig = n }
        # The span the label names, read off the label rather than assumed from the order the
        # rows arrive in: the four share one accumulator and are re-reported per sweep, so a
        # sweep that walked them in another order would still print four plausible rows.
        if (match($1, /\[[0-9]+B\]/)) { ws[wrows] = substr($1, RSTART + 1, RLENGTH - 3) + 0 }
        w50[wrows] = part($2, 1); w99[wrows] = part($2, 2); wmax[wrows] = part($2, 3)
        if (n > 0 && wphc != wpon) { wdom++ }
        if (n > 0 && egl == 0 && wpwin > 0 && part($2, 3) > wpwin) { wbound++ }
        # n plus this same probe foreign owes this same probe raised. Scoped to n>0: a handler
        # that timed out inside the sweep fixed spin lands in neither count, and only a
        # populated row can show the gap against its own denominator.
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
        for (i = 1; i <= 4; i++) {
            printf " %d %d %d %d", ws[i] + 0, w50[i] + 0, w99[i] + 0, wmax[i] + 0;
        }
        printf "\n";
    }')
EOF
}

# A window opens at the row every report begins with and runs to the next one or to the end,
# which is the model tools/bench/bench-capture.sh already reads these captures by.
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
    if [ "$iraised" -le 0 ]; then
        bad "the entry sweep raised line $iline zero times; the kernel never attached it, so the
      row below reports an instrument that did not run rather than a line that did not fire"
    fi
    if [ "$pdropped" -ne 0 ]; then
        bad "the close path refused $pdropped sample(s): an interrupt that never arrived, a wait
      that returned without the thread ever leaving the CPU, or a close from a thread that is not
      the woken waiter. Each makes the rows a subset of what the sweep asked for"
    fi

    # --- the sweep's denominator -----------------------------------------------
    if [ "$qseen" -ne 1 ]; then
        bad "the capture carried no e2e-passes line, so the sweep states no denominator and a
      run that abandoned most of its passes reports the rest as the whole population"
    else
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

    # --- end to end ------------------------------------------------------------
    # ONE CLAUSE AND TWO DIAGNOSES, not two arms. A board that delivers no injected interrupt
    # at all closes no span either, so the inner test can never refuse a report the outer one
    # lets through.
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
    if [ "$want" -gt 1 ]; then
        # NOT merely non-empty. The sweep places both ends and runs one same-core pass and one
        # next-core pass per core, so each population is at least one pass in 2*want of what closed,
        # BY CONSTRUCTION. Half of that is the floor here: it leaves the construction a factor of
        # two of slack and still refuses a population that arrived by coincidence, which is what a
        # sweep that places only one end produces on a backend whose line follows its injector.
        _lfloor=$((pclosed / (4 * want)))
        if [ "$_lfloor" -lt 1 ]; then
            _lfloor=1
        fi
        if [ "$ln" -lt "$_lfloor" ] || [ "$cn" -lt "$_lfloor" ]; then
            bad "the locality split reports local n=$ln cross n=$cn of $pclosed closed on a
      $want-core kernel, and each population is constructed at $_lfloor or more; a classifier stuck
      on one answer empties a row, and a sweep that places one end only leaves the other row to
      coincidence. A mixed distribution answers nothing, which is why the two are split"
        fi
    else
        if [ "$ln" -ne "$pclosed" ]; then
            bad "at one kernel core every wake is local by construction, and the local row reports
      $ln of $pclosed closed spans"
        fi
    fi

    # --- the clock domain ------------------------------------------------------
    if [ "$wprobes" -ne 4 ]; then
        bad "the masked sweep printed $wprobes probe line(s) beside $wrows row(s); each span is
      its own sweep and its row is read against its own probe, so a missing one leaves a row
      judged against another span's window"
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
    # n=0 is not this arm's to judge: a line never attached and a raise that reached no handler
    # both print n=0 beside raised>0, and the arms above already tell those apart. Where n>0,
    # the row's own count plus its own probe's foreign owes its own probe's raised exactly; the
    # gap is a handler that timed out inside the sweep's fixed spin, counted by neither.
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
    # ONE accumulator serves all four spans and is cleared between them, so no row may report more
    # samples than one span's worth. Left uncleared they climb across the sweep and every row still
    # looks populated.
    if [ "$wbig" -gt "$iraised" ]; then
        bad "a worst-case row reports $wbig samples where one span raises $iraised; the slot the
      four spans share was not cleared between them, so the later rows carry the earlier ones"
    fi

    # --- order, on EVERY row the report emitted ---------------------------------
    # A row nothing reads is a row whose distribution may be anything, and the cross-core one was
    # read for its sample count alone. Four of the seven are an empty 0/0/0 on a backend that
    # populates none of them, which is ordered, so no arm is keyed on whether a row fired.
    row_order() { # <name> <p50> <p99> <max>
        if [ "$2" -gt "$3" ] || [ "$3" -gt "$4" ]; then
            bad "the $1 reports p50=$2 p99=$3 max=$4, which is not ordered"
        fi
    }
    row_order "entry distribution" "$e50" "$e99" "$emax"
    row_order "local end-to-end row" "$l50" "$l99" "$lmax"
    row_order "cross-core end-to-end row" "$c50" "$c99" "$cmax"
    row_order "worst-case row for ${s1}B" "$a1" "$b1" "$m1"
    row_order "worst-case row for ${s2}B" "$a2" "$b2" "$m2"
    row_order "worst-case row for ${s3}B" "$a3" "$b3" "$m3"
    row_order "worst-case row for ${s4}B" "$a4" "$b4" "$m4"

    # --- the floor -------------------------------------------------------------
    # Both figures come from one clock and one unit, so this comparison is arithmetic and not an
    # assumption about any board's rate.
    if [ "$ln" -gt 0 ] && [ "$l50" -le "$ptare" ]; then
        bad "the local end-to-end p50 is $l50 ns against a tare of $ptare ns; the span is not
      longer than the same userspace read and the same trap with no interrupt in it, so it did not
      enclose a delivery, a dispatch and a wake"
    fi
    if [ "$cn" -gt 0 ] && [ "$c50" -le "$ptare" ]; then
        bad "the cross-core end-to-end p50 is $c50 ns against a tare of $ptare ns; the span that
      crossed a core is not longer than the same userspace read and the same trap with no
      interrupt in it, so it did not enclose a delivery, a dispatch and a wake"
    fi

    # --- the span, and whether this board can see it ----------------------------
    # THE FOUR ROWS EXIST TO SAY THAT A LONGER MASKED WINDOW COSTS MORE, and counting their samples
    # does not say it: four rows with the same figure carry the same sample counts as four rows
    # that separate, and an ignored span_bytes leaves exactly that.
    #
    # The labels are read off the rows and checked to ascend, so the comparison below is between
    # the spans it names and not between whichever rows arrived first.
    # Scoped to four rows: a short report leaves an unread label reading 0, which fails this
    # comparison for a reason the row-count arm above has already named.
    if [ "$wrows" -eq 4 ] \
       && { [ "$s1" -ge "$s2" ] || [ "$s2" -ge "$s3" ] || [ "$s3" -ge "$s4" ]; }; then
        bad "the worst-case rows name spans ${s1}B ${s2}B ${s3}B ${s4}B, which do not ascend; the
      four share one accumulator and are re-reported per sweep, so a sweep that walked them out of
      order still prints four plausible rows"
    fi
    _spans="${s1}B=$a1 ${s2}B=$a2 ${s3}B=$a3 ${s4}B=$a4"
    _judged="spread only"
    if [ "$en" -le 0 ]; then
        # Nothing was sampled, so there is no spread to have. The arms above already refused the
        # case where that happens beside a closed end-to-end span.
        _judged="not sampled"
    elif [ "$want" -gt 1 ]; then
        # ABOVE ONE KERNEL CORE NOTHING HERE HAS EVER POPULATED THESE ROWS. Every four-core board
        # in this fleet routes the injected line to a core other than the raiser, so the domain
        # arms refuse the lot and the branch above takes the empty case instead. This one is kept
        # for a board that delivers locally: the rows would then mean what they mean at one core,
        # and the ordering could be judged. Until a capture shows that, judging it asserts a shape
        # no run has produced.
        _judged="not judged above one kernel core (no capture has populated these rows there)"
    else
        if [ "$a2" -lt "$a1" ] || [ "$a3" -lt "$a2" ] || [ "$a4" -lt "$a3" ]; then
            bad "the worst-case p50s run $_spans, which does not grow with the span. Each row is
      the same raise with a longer masked body in front of it, so a later row below an earlier one
      is a body the mask does not cover or a counter that is not moving"
        fi
        # ONE BUCKET, which is the smallest difference this instrument can report: a percentile is
        # a bucket LOW edge at an eighth of an octave, so two figures inside w/8 of each other are
        # one reading.
        _step=$((a1 / 8))
        if [ "$a4" -le $((a1 + _step)) ]; then
            bad "the worst-case p50s run $_spans: the ${s4}B row does not clear the ${s1}B row by
      even one bucket ($_step at this magnitude), so a masked body of ${s4} bytes costs nothing
      this instrument can report against one of ${s1}. Either span_bytes reaches no body, or the
      counter these rows are read from is not moving"
        fi
        _judged="grows with the span"
    fi

}

# --- the arms, proven on planted reports before the real one ------------------
#
# EVERY ARM READS A NUMBER OFF A ROW, and a report of the right SHAPE carrying the wrong numbers
# is what this gate exists to refuse, so each plant below differs from a good one in one figure.
# An arm whose plant does not fire reports every real capture clean.
#
# The two good plants are real reports, one per core count: one kernel core, where the masked
# spans separate, and four, where they do not and must not be judged.
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
'  e2e-probe: line=201 closed=200 dropped=0 tare=1104/1363 ns  (min/avg, n=64)' \
'  e2e-passes: asked=200 raised=200' \
'  e2e-local: 98304/360448/378464 ns  (p50/p99/max, n=50)' \
'  e2e-cross: 147456/360448/2752960 ns  (p50/p99/max, n=150)'
}

# WHAT THE FOUR-CORE BOARDS PRINT, kept verbatim as the plant that must be refused: the rows
# populated from a peer's cycle counter, and a probe that says so if anything reads it.
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
'  e2e-probe: line=201 closed=200 dropped=0 tare=1104/1363 ns  (min/avg, n=64)' \
'  e2e-passes: asked=200 raised=200' \
'  e2e-local: 98304/360448/378464 ns  (p50/p99/max, n=50)' \
'  e2e-cross: 147456/360448/2752960 ns  (p50/p99/max, n=150)'
}

# A BOARD THAT DELIVERS ITS OWN RAISE LOCALLY ABOVE ONE KERNEL CORE. Nothing on this bench has
# ever printed it, and the clock-domain arms are kept for it: they are the arms that say what a
# populated four-core row owes, and a plant is the only place that shape exists. Every
# clock-domain plant below is this report with one field moved, so the arm each one names is
# the only thing that can refuse it.
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
'  e2e-probe: line=201 closed=200 dropped=0 tare=1104/1363 ns  (min/avg, n=64)' \
'  e2e-passes: asked=200 raised=200' \
'  e2e-local: 98304/360448/378464 ns  (p50/p99/max, n=50)' \
'  e2e-cross: 147456/360448/2752960 ns  (p50/p99/max, n=150)'
}

# <name> <kernel cores> <expect: pass|refuse> <report text>
ctl() {
    _ctl_save=$want
    want=$2
    # A SUBSHELL, because an arm that calls fail exits: the plant written for one of those would
    # otherwise take the whole control suite with it and every plant below it would go unread.
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

# ONE PLANT PER ARM, and one arm per plant. A plant that trips two arms proves neither:
# removing the arm it was written for still leaves the report refused, and the control reads as
# green. So every refuse plant below is a report that PASSES but for the single field its name
# points at, and the two whole-shape controls that cannot meet that standard say so themselves.
ctl 'one core, spans separating' 1 pass "$(ctl_one)"
# THE ONE THAT SAYS THE SPAN ARM IS KEYED RIGHT: four flat rows at four kernel cores, which is
# what the emulator really reports there, and which the same figures at one core must refuse.
ctl 'four cores, spans flat' 4 pass "$(ctl_four)"
# The third good report, and the base every clock-domain plant is one field away from.
ctl 'four cores delivering locally' 4 pass "$(ctl_four_local)"

# --- the probe lines, and the raise denominator under them --------------------
# NOT AN ISOLATING CONTROL, and it is the one arm that cannot have one. A report with no
# irq-probe line carries raised=0 and one with no e2e-probe line carries closed=0, so the guard
# can never refuse a report those two arms would let through; what it buys is a single finding
# instead of a cascade read off handed zeros. It is proven here as a SHAPE and merged into them
# for coverage.
ctl 'a report with no end-to-end probe line' 1 refuse \
    "$(ctl_one | sed '/^  e2e-probe: /d')"
# A line the kernel never attached: every sweep raised it zero times and every row is empty,
# which is the state the row alone cannot show. Isolates the raise denominator.
ctl 'a line that was never raised' 1 refuse \
    "$(ctl_one | sed 's|raised=100|raised=0|
                      s|n=100)|n=0)|
                      s|^\(  irq: *\)[0-9]*/[0-9]*/[0-9]*|\10/0/0|
                      s|^\(  wcase-irq\[[0-9]*B\]: *\)[0-9]*/[0-9]*/[0-9]*|\10/0/0|')"
# Two closes that opened no span, on top of a sweep whose every raise closed. `raised` and
# `closed` still agree, so the denominator arms cannot reach it. Isolates the dropped count.
ctl 'closes the sweep refused' 1 refuse \
    "$(ctl_one | sed 's|dropped=0|dropped=2|')"

# --- the sweep's denominator, ONE CLAUSE PER PLANT ---------------------------
# THE SHORTFALL THE APP ITSELF CANNOT REPORT: its raiser gives a pass up after its retry bound
# and says so in a comment. Thirteen passes abandoned, and the rest closing cleanly, which is
# exactly the shape that produced a full-looking report.
ctl 'a sweep that abandoned passes' 1 refuse \
    "$(ctl_one \
        | sed 's|asked=50 raised=50|asked=50 raised=37|' \
        | sed 's|closed=50|closed=37|' \
        | sed 's|^\(  e2e-local: .*\)n=50)|\1n=37)|')"
# Every pass raised and a quarter of them closing nowhere. The locality total still agrees with
# `closed`, so this is the clause the e2e arm cannot reach.
ctl 'raises that closed nothing' 1 refuse \
    "$(ctl_one \
        | sed 's|closed=50|closed=37|' \
        | sed 's|^\(  e2e-local: .*\)n=50)|\1n=37)|')"
ctl 'a sweep with no denominator at all' 1 refuse \
    "$(ctl_one | sed '/^  e2e-passes: /d')"

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
# One closed span in neither row. Both populations clear the constructed floor, so only the
# total refuses it.
ctl 'a closed span in neither locality row' 4 refuse \
    "$(ctl_four | sed 's|^\(  e2e-cross: .*\)n=150)|\1n=149)|')"
# The two rows still total what closed, and the local one arrived by coincidence rather than by
# the placement the sweep constructs. Isolates the population floor.
ctl 'a locality population left to coincidence' 4 refuse \
    "$(ctl_four | sed 's|^\(  e2e-local: .*\)n=50)|\1n=5)|
                       s|^\(  e2e-cross: .*\)n=150)|\1n=195)|')"
# One sample classified cross at ONE kernel core, where there is one core to wake on. The rows
# still total what closed, so only the one-core clause refuses it.
ctl 'a cross-core sample at one kernel core' 1 refuse \
    "$(ctl_one | sed 's|^\(  e2e-local: .*\)n=50)|\1n=49)|'
       printf '%s\n' '  e2e-cross: 8192/8192/8192 ns  (p50/p99/max, n=1)')"

# --- the clock domain, one plant per statement -------------------------------
# THE WHOLE-SHAPE CONTROL, AND THE ONLY PLANT HERE THAT TRIPS SEVERAL ARMS: a real report rather
# than one written for an arm, kept verbatim because what it proves is that this gate refuses
# the six-figure report the four-core boards print. It is NOT the witness for any single arm,
# and each of them has its own plant below.
ctl 'rows filled from a peer core' 4 refuse "$(ctl_four_offset)"
# The four rows are populated and locally delivered, and one field of the entry probe moves.
ctl 'an entry row stamped on several cores' 4 refuse \
    "$(ctl_four_local | sed 's|^\(  irq-probe: .*\)hcore=3|\1hcore=mixed|')"
ctl 'an entry row whose probe names no handler core' 4 refuse \
    "$(ctl_four_local | sed 's|^\(  irq-probe: .*\)hcore=3|\1hcore=none|')"
ctl 'an entry row stamped on a peer core' 4 refuse \
    "$(ctl_four_local | sed 's|^\(  irq-probe: .*\)hcore=3|\1hcore=0|')"
# The probe names the core the sweep ran on and only the arithmetic refuses it: this is what a
# classifier stuck on "local" leaves when it also reports the core the reader wants to see.
ctl 'an entry row outside its own window' 4 refuse \
    "$(ctl_four_local | sed 's|^\(  irq-probe: .*\)win=263960|\1win=100000|')"
ctl 'a masked row stamped on a peer core' 4 refuse \
    "$(ctl_four_local | sed 's|^\(  wcase-probe: .*\)hcore=3\( foreign=0 win=23040\)|\1hcore=0\2|')"
ctl 'a masked row outside its own window' 1 refuse \
    "$(ctl_one | sed 's|^\(  wcase-probe: .*\)win=5600|\1win=3000|')"
# One sample refused at ONE kernel core. The row is cut to 99 so that it still owes its own
# probe exactly, which is what keeps the population arm out of this one.
ctl 'a refusal at one kernel core' 1 refuse \
    "$(ctl_one | sed 's|^\(  irq-probe: .*\)foreign=0|\1foreign=1|
                      s|^\(  irq: *1408/3328/172560 cyc  (p50/p99/max, \)n=100)|\1n=99)|')"

# --- population, one plant per row shape -------------------------------------
# Both leave raised=100 and foreign=0 untouched beside a row cut to n=10: the exact shape a
# handler timing out inside the sweep's fixed spin leaves behind, landing in neither count.
ctl 'the entry row short of its own raise count' 1 refuse \
    "$(ctl_one | sed 's|^\(  irq:       1408/3328/172560 cyc  (p50/p99/max, \)n=100)|\1n=10)|')"
ctl 'a masked row short of its own raise count' 1 refuse \
    "$(ctl_one | sed 's|^\(  wcase-irq\[64B\]:   1792/2048/19740 cyc  (p50/p99/max, \)n=100)|\1n=10)|')"

# --- the four masked rows: how many arrived, which fired, and how many samples -
# Three rows where four spans are reported. The entry row is empty here, so the count of
# POPULATED rows is still the zero it owes, and the label arm is scoped past a short report:
# only the row count refuses this one.
ctl 'a masked sweep that printed three rows' 4 refuse \
    "$(ctl_four | sed '/^  wcase-irq\[256B\]:/d')"
# One span firing where its siblings did not. The empty row is the FIRST, so the p50s still
# ascend and clear each other, and only the count of populated rows refuses it.
ctl 'a masked row that fired where its siblings did not' 1 refuse \
    "$(ctl_one | sed 's|^\(  wcase-irq\[0B\]: *\)1536/2048/20220 cyc  (p50/p99/max, n=100)|\10/0/0 cyc  (p50/p99/max, n=0)|')"
# THE SHAPE THE HEADER OF THIS FILE IS WRITTEN AROUND: the slot the four spans share carried
# across them, each row holding every earlier row's samples. Its per-span probe carried too, so
# each row still owes its own probe exactly and the population arm is silent; what refuses it is
# the count against the raises ONE span makes.
ctl 'a masked slot carried across the spans' 1 refuse \
    "$(ctl_one | sed 's|^\(  wcase-probe: .*\)raised=100\( hcore=0 foreign=0 win=23040\)|\1raised=200\2|
                      s|^\(  wcase-probe: .*\)raised=100\( hcore=0 foreign=0 win=5600\)|\1raised=300\2|
                      s|^\(  wcase-probe: .*\)raised=100\( hcore=0 foreign=0 win=15860\)|\1raised=400\2|
                      s|^\(  wcase-irq\[64B\]: *1792/2048/19740 cyc  (p50/p99/max, \)n=100)|\1n=200)|
                      s|^\(  wcase-irq\[256B\]: *2304/3584/3860 cyc  (p50/p99/max, \)n=100)|\1n=300)|
                      s|^\(  wcase-irq\[1024B\]: *4608/6144/6740 cyc  (p50/p99/max, \)n=100)|\1n=400)|')"
# The last probe is the one deleted, so the row below it reads the 256B probe's window; its
# figures come down under that window, leaving the probe COUNT as the only thing wrong.
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
ctl 'a worst-case row out of order' 1 refuse \
    "$(ctl_one | sed 's|^\(  wcase-irq\[256B\]:  *\)2304/3584/3860|\13584/2304/3860|')"
ctl 'the cross row out of order' 4 refuse \
    "$(ctl_four | sed 's|^\(  e2e-cross: \)147456/360448/|\1360448/147456/|')"
ctl 'the cross row at the tare' 4 refuse \
    "$(ctl_four | sed 's|^\(  e2e-cross: \)147456/|\11104/|')"
ctl 'the local row at the tare' 4 refuse \
    "$(ctl_four | sed 's|^\(  e2e-local: \)98304/|\11104/|')"

# --- the recorded-capture path, which no control above reaches -----------------
# EVERY SILICON VERDICT IN THIS TREE COMES THROUGH BENCH_CAPTURE=, and it differs from the polled
# path in the two ways the plants below carry: the CR every console line ends in, and the several
# report windows a capture holds against the one window a poll stops on. A capture whose LAST
# window is the broken one is exactly what a first-window reader reports clean.
# The broken window is the 'sweep that abandoned passes' report entire, so ONE arm refuses it
# and what these two controls prove is which windows are read rather than which arm reads them.
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

# <name> <expect: pass|refuse> <capture text>. Written out with the CR the console leaves on
# every line and read back the way the recorded path reads it, so the strip is under control too.
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
# THE ONE THE FIRST-WINDOW READER REPORTS CLEAN: two accounted windows and a third that abandoned
# thirteen passes. Nothing in windows one and two is wrong, so only reading the third refuses it.
ctl_capture 'a third window short of its denominator' refuse "$(ctl_windows 3 3)"
ctl_capture 'a first window short of its denominator' refuse "$(ctl_windows 3 1)"
# A complete report and no throughput row above it. Every window arm reads a window this
# capture never opens, so the loop runs zero times and the verdict is the window count alone.
ctl_capture 'a capture holding no report window' refuse "$(ctl_one)"

echo "== control: each arm fires on a planted report that no other arm refuses, four flat
  spans pass at four kernel cores and are refused at one, a sweep short of its own denominator
  is refused, the six-figure report the four-core boards printed before the domain arms existed
  is refused whole, and a three-window CRLF capture is judged window by window rather than on
  its first =="

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
    # The poll stopped the image on the first complete block, so anything after it is a window
    # the run was cut off inside.
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
      $mode; line $pline closed $pclosed end to end; $_split"
echo "      masked spans $_spans ($_judged)"
exit 0
