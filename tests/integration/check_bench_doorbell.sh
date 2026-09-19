#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Check doorbell rounds from each kernel core: placement, sample counts, totals
# and ordering. Each round must include the raise and all peer replies.
# Compare its minimum with the same core's raise-only measurement.
# Read the expected burst size from the capture.
# --controls checks fixed reports and expected failure counts.
#
# Usage: check_bench_doorbell.sh <elf> <kernel cores> | --controls

set -u
. "$(dirname "$0")/../lib/gate.sh"
# Allow time for the call/reply sweep before the first report.
: "${QEMU_TIMEOUT:=240}"

_usage="usage: check_bench_doorbell.sh <elf> <kernel cores> | check_bench_doorbell.sh --controls"
controls_only=0
if [ "${1:-}" = "--controls" ]; then
    controls_only=1
    # Initialize before ctl saves and restores want under set -u.
    want=4
    elf=""
else
    elf="${1:?$_usage}"
    want="${2:?$_usage}"

    require_number "$want" "the kernel core count"
    [ -f "$elf" ] || fail "no image at $elf"
    need_qemu_machine

    if [ "$want" -le 1 ]; then
        fail "registered at $want kernel core(s); BD_DOORBELL is not declared there and a raise
  with no peer is not a round trip"
    fi

    # Wait for the IRQ line after the distribution block so all core rows arrive.
    # Avoid end anchors because the raw log may use CRLF.
    poll_image "$elf" "^  doorbell-probe:" "^  irq:" > /dev/null

    if [ "$POLL_OK" -ne 1 ]; then
        fail "no doorbell probe and report within ${QEMU_TIMEOUT}s; the image printed neither, so
  nothing below would have been read"
    fi
fi

rc=0

# Read the first report window only.
read_report() { # <report text>
    read -r bursts rounds misplaced shortruns unmoved dn d50 d99 dmax rows full rowsum tooshort \
            sw50 <<EOF
$(printf '%s\n' "$1" | awk -v SPAN_FACTOR=2 '
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
        # Use the first burst as the expected count for every core.
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
    # Distribution core rows have a colon; phase-table core rows do not.
    /^ +core [0-9]+: / {
        if (cur == "db") {
            n = tail_n($0)
            c = $2; sub(/:$/, "", c); c = c + 0
            rows++
            rowsum += n
            if (n == rounds) { full++ }
            # Compare round minimum with the raise-only cost on the same core.
            # SPAN_FACTOR sets the minimum ratio.
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
}

doorbell_arms() {
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

    # The burst, per-core and aggregate counts are linked: inconsistent counts
    # fail at least two checks, so their controls expect two failures.
    _expect=$(( want * rounds ))
    if [ "$dn" -ne "$_expect" ]; then
        bad "the doorbell distribution reports n=$dn against $want core(s) x $rounds rounds"
    fi
    if [ "$d50" -gt "$d99" ] || [ "$d99" -gt "$dmax" ]; then
        bad "the doorbell distribution reports p50=$d50 p99=$d99 max=$dmax, which is not ordered"
    fi

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

    mode="counts only (the switch line reports a p50 of 0, so this board has no cycle source)"
    if [ "$sw50" -gt 0 ]; then
        mode="counts and cycles"
        if [ "$tooshort" -ne 0 ]; then
            bad "$tooshort core row(s) report a minimum under twice the raise floor that core
  measured in the same burst; the bracket is closing inside the raise, before any peer could
  have answered, and every count above still reads correct"
        fi
    fi
}

# Four-core RV64 fixture. Keep other distributions to test row selection.
ctl_four() {
    printf '%s\n' \
'  throughput: 18928 ctx-sw/s  (52830 ns/sw avg over 40000 switches / 2113 ms)' \
'  doorbell-probe: asked=0 on=0 ran=64 db+64 raise=3400' \
'  doorbell-probe: asked=1 on=1 ran=64 db+64 raise=5980' \
'  doorbell-probe: asked=2 on=2 ran=64 db+64 raise=3060' \
'  doorbell-probe: asked=3 on=3 ran=64 db+64 raise=3420' \
'  switch-probe: fastpath-swaps=0  (swapped inside the trap, so not in the switch row)' \
'  switch:    1280/7168/67180 cyc  (p50/p99/max, n=40004)' \
'    core 0: 560/1686/67180 cyc  (min/avg/max, n=8951)' \
'    core 1: 540/1742/36760 cyc  (min/avg/max, n=9698)' \
'    core 2: 520/1703/62920 cyc  (min/avg/max, n=10332)' \
'    core 3: 560/1707/45560 cyc  (min/avg/max, n=11023)' \
'  lock-hold: 36864/180224/6239580 cyc  (p50/p99/max, n=198244)' \
'    core 0: 1900/50046/6239580 cyc  (min/avg/max, n=48303)' \
'    core 1: 3320/49533/3950340 cyc  (min/avg/max, n=49182)' \
'    core 2: 2100/48760/5707700 cyc  (min/avg/max, n=49888)' \
'    core 3: 1900/48907/5839840 cyc  (min/avg/max, n=50871)' \
'  lock-wait: 15360/131072/6203580 cyc  (p50/p99/max, n=239019)' \
'    core 0: 180/25398/6203580 cyc  (min/avg/max, n=57517)' \
'    core 1: 180/24268/3843060 cyc  (min/avg/max, n=58961)' \
'    core 2: 180/23441/2287440 cyc  (min/avg/max, n=60323)' \
'    core 3: 180/23080/5825260 cyc  (min/avg/max, n=62218)' \
'  doorbell:  30720/68720/68720 cyc  (p50/max/max, n=256)' \
'    core 0: 21060/32892/68720 cyc  (min/avg/max, n=64)' \
'    core 1: 15340/28631/43380 cyc  (min/avg/max, n=64)' \
'    core 2: 26680/36953/50280 cyc  (min/avg/max, n=64)' \
'    core 3: 21500/33907/60020 cyc  (min/avg/max, n=64)' \
'  lock-site: core=0 site=0xffffffff80011574 max=6239580' \
'  lock-site: core=1 site=0xffffffff800016a8 max=3950340' \
'  lock-site: core=2 site=0xffffffff800016a8 max=5707700' \
'  lock-site: core=3 site=0xffffffff80011574 max=5839840'
}

# <name> <kernel cores> <pass|refuse> <findings expected on a refusal> <report text>
_ctl_pass=0
_ctl_refuse=0
ctl() {
    _ctl_save=$want
    want=$2
    # Run in a subshell so fail() cannot terminate the control suite.
    _ctl_out="$( ( rc=0; read_report "$5"; doorbell_arms; exit "$rc" ) 2>&1 )"
    _ctl_got=$?
    want=$_ctl_save
    _ctl_n="$(printf '%s\n' "$_ctl_out" | grep -c '^FAIL: ')"
    if [ "$3" = pass ] && [ "$_ctl_got" -ne 0 ]; then
        fail "the '$1' control is REFUSED, and it is a report this gate must accept; every
  capture below would be refused for the same reason:
$_ctl_out"
    fi
    if [ "$3" = refuse ] && [ "$_ctl_got" -eq 0 ]; then
        fail "the '$1' control PASSES, so this gate cannot see that class and a capture
  carrying it reads as clean"
    fi
    # Check the failure count so another check cannot hide a missing validation.
    if [ "$3" = refuse ] && [ "$_ctl_n" -ne "$4" ]; then
        fail "the '$1' control produced $_ctl_n finding(s) where $4 was expected, so it does not
  isolate the arm it was written for:
$_ctl_out"
    fi
    if [ "$3" = pass ]; then
        _ctl_pass=$((_ctl_pass + 1))
    else
        _ctl_refuse=$((_ctl_refuse + 1))
    fi
}

if [ "$controls_only" -eq 1 ]; then
    ctl 'four cores, every burst fed' 4 pass 0 "$(ctl_four)"
    # Without a cycle source, counts remain valid and all measured values are zero.
    ctl 'a board reporting counts and no cycles' 4 pass 0 \
        "$(ctl_four | sed 's|raise=[0-9]*|raise=0|
                           s|^\(  switch: *\)[0-9]*/[0-9]*/[0-9]*|\10/0/0|
                           s|^\(  lock-hold: \)[0-9]*/[0-9]*/[0-9]*|\10/0/0|
                           s|^\(  lock-wait: \)[0-9]*/[0-9]*/[0-9]*|\10/0/0|
                           s|^\(  doorbell:  \)[0-9]*/[0-9]*/[0-9]*|\10/0/0|
                           s|^\(    core [0-9]*: \)[0-9]*/[0-9]*/[0-9]*|\10/0/0|
                           s|^\(  lock-site: .*\)max=[0-9]*|\1max=0|')"

    # Remove the last burst so the expected round count remains valid.
    ctl 'a core the sweep never raised from' 4 refuse 1 \
        "$(ctl_four | sed '/^  doorbell-probe: asked=3 /d')"
    # Zero rounds must fail even when all counts agree.
    ctl 'bursts that ran no round at all' 4 refuse 1 \
        "$(ctl_four | sed 's|ran=64 db+64|ran=0 db+0|
                           s|raise=[0-9]*|raise=0|
                           s|^\(  doorbell:  \)30720/68720/68720 cyc  (p50/max/max, n=256)|\10/0/0 cyc  (p50/max/max, n=0)|
                           s|^\(    core [0-9]*: \)[0-9]*/[0-9]*/[0-9]* cyc  (min/avg/max, n=64)|\10/0/0 cyc  (min/avg/max, n=0)|')"
    # Move core 2's burst to core 1. Both span comparisons still pass.
    ctl 'a burst that ran somewhere else' 4 refuse 1 \
        "$(ctl_four | sed 's|^  doorbell-probe: asked=2 on=2 |  doorbell-probe: asked=2 on=1 |')"
    # Change only completed rounds, preserving the sample delta.
    ctl 'a burst short of the rounds it was asked for' 4 refuse 1 \
        "$(ctl_four | sed 's|^\(  doorbell-probe: asked=3 on=3 \)ran=64|\1ran=60|')"
    ctl 'a burst whose own count never moved' 4 refuse 1 \
        "$(ctl_four | sed 's|^\(  doorbell-probe: asked=3 on=3 ran=64 \)db+64|\1db+0|')"

    ctl 'a percentile walk reading the wrong end' 4 refuse 1 \
        "$(ctl_four | sed 's|^\(  doorbell:  \)30720/68720|\168720/30720|')"
    # Add an empty row to preserve totals while changing the row count.
    ctl 'more per-core rows than the kernel has cores' 4 refuse 1 \
        "$(ctl_four | sed 's|^\(    core 3: 21500/33907/60020 cyc  (min/avg/max, n=64)\)|\1\n    core 4: 0/0/0 cyc  (min/avg/max, n=0)|')"
    # A short row also breaks the aggregate sum.
    ctl 'a row short of the burst that fed it' 4 refuse 2 \
        "$(ctl_four | sed 's|^\(    core 3: 21500/33907/60020 cyc  (min/avg/max, n=\)64)|\160)|')"
    # A short aggregate breaks both the expected burst total and the row sum.
    ctl 'an aggregate under the bursts that fed it' 4 refuse 2 \
        "$(ctl_four | sed 's|^\(  doorbell:  30720/68720/68720 cyc  (p50/max/max, n=\)256)|\1252)|')"

    # Core 1 has the largest raise floor; shorten its span without changing ordering.
    ctl 'a round trip closed inside its own raise' 4 refuse 1 \
        "$(ctl_four | sed 's|^\(    core 1: \)15340/|\16000/|')"

    echo "PASS: $_ctl_pass planted report(s) accepted and $_ctl_refuse refused, each naming its
  own arm"
    exit 0
fi

read_report "$OUT"
doorbell_arms

assert_no_panic "the bench image panicked while the doorbell bursts ran"

if [ "$rc" -ne 0 ]; then
    exit 1
fi
echo "PASS: $bursts burst(s) of $rounds, each on the core asked for; doorbell n=$dn p50=$d50 p99=$d99 max=$dmax over $rows rows; $mode"
exit 0
