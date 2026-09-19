#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Check lock sampling, per-core totals, statistic labels and maximum/site pairs.
# Use the switch counter to decide whether cycle values must be nonzero.
# --controls checks parser branches with fixed reports and expected failure counts.
#
# Usage: check_bench_lock.sh <elf> <kernel cores> | --controls

set -u
. "$(dirname "$0")/../lib/gate.sh"
# Allow time for the call/reply sweep before the first report.
: "${QEMU_TIMEOUT:=240}"

_usage="usage: check_bench_lock.sh <elf> <kernel cores> | check_bench_lock.sh --controls"
controls_only=0
if [ "${1:-}" = "--controls" ]; then
    controls_only=1
    # Initialize before ctl saves and restores want under set -u.
    want=1
    elf=""
else
    elf="${1:?$_usage}"
    want="${2:?$_usage}"

    require_number "$want" "the kernel core count"
    [ -f "$elf" ] || fail "no image at $elf"
    need_qemu_machine

    # Wait for the last core so capture includes the whole site block.
    # Avoid end anchors because the raw log may use CRLF.
    poll_image "$elf" "^  lock-probe:" "^  lock-site: core=$((want - 1)) " > /dev/null

    if [ "$POLL_OK" -ne 1 ]; then
        fail "no lock-probe and lock distribution within ${QEMU_TIMEOUT}s; the image printed
  neither, so nothing below would have been read"
    fi
fi

rc=0

# Read counts from the first report window only.
read_report() { # <report text>
    read -r pseen phold pwait pdepth hn h50 h99 hmax wn rows spread rowsum rowmax swmax \
            spseen spfast labrows labhi lablo labmid sites sitelive sitemax siteunstable \
            sitecores <<EOF
$(printf '%s\n' "$1" | awk -v want="$want" '
    function tail_n(s,   t) { t = s; sub(/.*n=/, "", t); sub(/[^0-9].*$/, "", t); return t + 0 }
    function part(s, i,   f) { split(s, f, "/"); return f[i] + 0 }
    function commit_sites(   k) {
        sites = bsites; sitelive = blive; sitemax = bmax; siteunstable = bunstable
        sitecores = 0
        for (k in bcore) { sitecores++ }
        siteseen = 1
    }

    # Check every percentile label before parsing row-specific fields.
    # min/avg/max builds have no percentile labels.
    /\(p50\// {
        labrows++
        lab = $0; sub(/^.*\(/, "", lab); sub(/,.*$/, "", lab)
        if (tail_n($0) >= 1000) {
            if (lab != "p50/p99/max") { labhi++ }
        } else {
            if (lab != "p50/max/max") { lablo++ }
            else if (part($2, 2) != part($2, 3)) { labmid++ }
        }
    }
    # The second throughput line closes the first window, even if it has no sites.
    # lock-probe appears only once at startup.
    /^  throughput:/ {
        if (win >= 1 && siteseen == 0) { commit_sites() }
        win++
    }
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
    # Collect the first window only; later windows cannot supply missing sites.
    # Site 0 means that core has taken no sample.
    /^  lock-site:/ && siteseen == 0 {
        bsites++
        for (i = 2; i <= NF; i++) {
            if ($i ~ /^core=[0-9]+$/) { bcore[substr($i, 6) + 0] = 1 }
            if ($i ~ /^site=0x[0-9a-fA-F]+$/ && $i != "site=0x0") { blive++ }
            if ($i ~ /^max=[0-9]+$/) {
                v = substr($i, 5) + 0
                if (v > bmax) { bmax = v }
            }
            if ($i == "unstable=1") { bunstable++ }
        }
        cur = ""
        next
    }
    # Distribution core rows have a colon; phase-table core rows do not.
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
    END { if (siteseen == 0) { commit_sites() }
          printf "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d\n",
                 pseen + 0, phold + 0, pwait + 0, pdepth + 0,
                 hn + 0, h50 + 0, h99 + 0, hmax + 0, wn + 0,
                 rows + 0, spread + 0, rowsum + 0, rowmax + 0, swmax + 0,
                 spseen + 0, spfast + 0,
                 labrows + 0, labhi + 0, lablo + 0, labmid + 0,
                 sites + 0, sitelive + 0, sitemax + 0, siteunstable + 0,
                 sitecores + 0 }')
EOF
}

lock_arms() {
    # Stop on a missing probe to avoid reporting errors from default zero values.
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

    if [ "$hn" -le 0 ]; then
        bad "the hold distribution carried no samples; a quiet board and a dead instrument are the
  same output, which is why the probe above is read first"
    fi
    if [ "$h50" -gt "$h99" ] || [ "$h99" -gt "$hmax" ]; then
        bad "the hold distribution reports p50=$h50 p99=$h99 max=$hmax, which is not ordered"
    fi

    # Require at least one percentile row before checking its labels.
    # All emulator presets using this gate report percentiles.
    if [ "$labrows" -le 0 ]; then
        bad "no distribution row in the capture names the statistic its middle column carries,
  so the three clauses below decide nothing"
    fi
    if [ "$labhi" -ne 0 ]; then
        bad "$labhi row(s) carrying 1000 samples or more do not head their middle column p99,
  so a figure the population supports is being withheld under another name"
    fi
    if [ "$lablo" -ne 0 ]; then
        bad "$lablo row(s) under 1000 samples head their middle column p99; below 100 samples
  the 99th nearest rank IS the largest sample, and a few hundred put one or two above it, so
  what the column carries is the top sample under a percentile's name"
    fi
    if [ "$labmid" -ne 0 ]; then
        bad "$labmid row(s) head their middle column max and print something other than their
  own maximum there, so the label and the figure disagree"
    fi

    if [ "$sites" -ne "$want" ]; then
        bad "the capture carried $sites lock-site line(s) for a $want-core kernel; the maximum
  of a core with no line named is a figure nothing can chase"
    fi
    if [ "$hn" -gt 0 ] && [ "$sitelive" -eq 0 ]; then
        bad "the hold distribution carried $hn samples and every lock-site line names 0x0; the
  site cell is written by the first sample whatever its width, so no core filed one"
    fi
    if [ "$sitecores" -ne "$sites" ]; then
        bad "the first report's site block carried $sites line(s) naming $sitecores distinct
  core(s); a block completed by a LATER report reaches the right line count while repeating a
  core and hiding the one the first report never printed"
    fi
    if [ "$siteunstable" -ne 0 ]; then
        bad "$siteunstable lock-site line(s) carry unstable=1; the reader could not read that
  core's site and its figure as one pair, so the address and the maximum on that line may name
  different releases and neither is quotable"
    fi
    if [ "$sitemax" -lt "$hmax" ]; then
        bad "the largest lock-site line reports max=$sitemax against an aggregated hold maximum
  of $hmax; the site lines are read AFTER that row and the row is still fed between the two, so
  a site maximum can only be the larger"
    fi

    if [ "$want" -gt 1 ]; then
        if [ "$pwait" -ne 1 ]; then
            bad "three nested IrqLocks moved the wait count by $pwait on the probing core; the
  cross-core lock is taken once, as the depth rises from zero"
        fi
        if [ "$rows" -ne "$want" ]; then
            bad "the hold distribution printed $rows per-core row(s) for a $want-core kernel"
        fi
        # With matching totals, a single active core also equals the aggregate count.
        # That control must trigger both checks.
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

    # An ordered distribution with max=0 also has p50=0, so both checks fail.
    if [ "$swmax" -gt 0 ]; then
        if [ "$hmax" -le 0 ]; then
            bad "the switch instrument measured $swmax cycles and the hold distribution measured 0;
  the bracket is counting samples without timing them"
        fi
        if [ "$h50" -le 0 ]; then
            bad "the hold p50 is 0 on a board whose counter runs; the histogram took no sample the
  percentile walk could find"
        fi
    fi
}

# One-core RV32 fixture. The IRQ row exercises the low-sample label check.
ctl_one() {
    printf '%s\n' \
'  lock-probe: hold+1 core=0 depth=0  (three nested, one outermost)' \
'  throughput: 153082 ctx-sw/s  (6532 ns/sw avg over 40000 switches / 261 ms)' \
'  switch-probe: fastpath-swaps=0  (swapped inside the trap, so not in the switch row)' \
'  switch:    1536/1664/9420 cyc  (p50/p99/max, n=40001)' \
'  lock-hold: 2560/4096/41400 cyc  (p50/p99/max, n=80238)' \
'  lock-site: core=0 site=0x80001750 max=41400' \
'  irq:       1536/4600/4600 cyc  (p50/max/max, n=100)'
}

# A later report window has no startup lock-probe line.
ctl_window() {
    printf '%s\n' \
'  throughput: 61240 ctx-sw/s  (16328 ns/sw avg over 40000 switches / 653 ms)' \
'  switch-probe: fastpath-swaps=0  (swapped inside the trap, so not in the switch row)' \
'  switch:    2048/4096/65536 cyc  (p50/p99/max, n=40012)' \
'  lock-hold: 4096/8192/131072 cyc  (p50/p99/max, n=80000)' \
'  lock-wait: 512/1024/32768 cyc  (p50/p99/max, n=79000)' \
'  lock-site: core=0 site=0x40201750 max=131072' \
'  lock-site: core=1 site=0x40201750 max=120832' \
'  lock-site: core=2 site=0x40203bb2 max=118784' \
'  lock-site: core=3 site=0x40203bb2 max=116736'
}

ctl_four() {
    printf '%s\n' \
'  lock-probe: hold+1 wait+1 core=2 depth=0  (three nested, one outermost)' \
'  throughput: 61233 ctx-sw/s  (16330 ns/sw avg over 40000 switches / 653 ms)' \
'  switch-probe: fastpath-swaps=0  (swapped inside the trap, so not in the switch row)' \
'  switch:    2048/4096/65536 cyc  (p50/p99/max, n=40012)' \
'    core 0: 1800/2600/61440 cyc  (min/avg/max, n=10003)' \
'    core 1: 1800/2600/60416 cyc  (min/avg/max, n=10003)' \
'    core 2: 1800/2600/59392 cyc  (min/avg/max, n=10003)' \
'    core 3: 1800/2600/58368 cyc  (min/avg/max, n=10003)' \
'  lock-hold: 4096/8192/131072 cyc  (p50/p99/max, n=80000)' \
'    core 0: 3000/5000/131072 cyc  (min/avg/max, n=20000)' \
'    core 1: 3000/5000/120832 cyc  (min/avg/max, n=20000)' \
'    core 2: 3000/5000/118784 cyc  (min/avg/max, n=20000)' \
'    core 3: 3000/5000/116736 cyc  (min/avg/max, n=20000)' \
'  lock-wait: 512/1024/32768 cyc  (p50/p99/max, n=79000)' \
'    core 0: 400/900/32768 cyc  (min/avg/max, n=19750)' \
'    core 1: 400/900/31744 cyc  (min/avg/max, n=19750)' \
'    core 2: 400/900/30720 cyc  (min/avg/max, n=19750)' \
'    core 3: 400/900/29696 cyc  (min/avg/max, n=19750)' \
'  lock-site: core=0 site=0x40201750 max=131072' \
'  lock-site: core=1 site=0x40201750 max=120832' \
'  lock-site: core=2 site=0x40203bb2 max=118784' \
'  lock-site: core=3 site=0x40203bb2 max=116736'
}

# <name> <kernel cores> <pass|refuse> <findings expected on a refusal> <report text>
_ctl_pass=0
_ctl_refuse=0
ctl() {
    _ctl_save=$want
    want=$2
    # Run in a subshell so fail() cannot terminate the control suite.
    _ctl_out="$( ( rc=0; read_report "$5"; lock_arms; exit "$rc" ) 2>&1 )"
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
    ctl 'one core, a complete block' 1 pass 0 "$(ctl_one)"
    ctl 'four cores, every row fed' 4 pass 0 "$(ctl_four)"
    ctl 'a second report window, as the app prints it' 4 pass 0 "$(ctl_four)
$(ctl_window)"
    ctl 'a core missing from the first window, complete in the second' 4 refuse 1 \
        "$(printf '%s\n' "$(ctl_four)" | /bin/grep -v '^  lock-site: core=3 ')
$(ctl_window)"
    # An empty site block fails three checks: count, nonzero site and maximum.
    ctl 'no site line in the first window' 4 refuse 3 \
        "$(printf '%s\n' "$(ctl_four)" | /bin/grep -v '^  lock-site: ')
$(ctl_window)"
    ctl 'a site block repeating a core' 4 refuse 1 \
        "$(printf '%s\n' "$(ctl_four)" | sed 's/^  lock-site: core=3 /  lock-site: core=2 /')"
    ctl 'a lock-site line reporting an unstable pair' 4 refuse 1 \
        "$(printf '%s\n' "$(ctl_four)" | sed 's/^\(  lock-site: core=2 .*\)$/\1 unstable=1/')"

    ctl 'a report with no lock probe' 1 refuse 1 \
        "$(ctl_one | sed '/^  lock-probe:/d')"
    ctl 'a probe that ran inside a bracket' 1 refuse 1 \
        "$(ctl_one | sed 's|depth=0|depth=2|')"
    ctl 'a bracket that sampled every nesting level' 1 refuse 1 \
        "$(ctl_one | sed 's|hold+1|hold+3|')"
    ctl 'a report with no switch probe' 1 refuse 1 \
        "$(ctl_one | sed '/^  switch-probe:/d')"
    ctl 'a window holding fastpath swaps' 1 refuse 1 \
        "$(ctl_one | sed 's|fastpath-swaps=0|fastpath-swaps=7|')"

    # Zero the switch values too, isolating the empty-population check.
    ctl 'a hold row that never sampled' 1 refuse 1 \
        "$(ctl_one | sed 's|^\(  switch: *\)[0-9]*/[0-9]*/[0-9]*|\10/0/0|
                          s|^\(  lock-hold: \)[0-9]*/[0-9]*/[0-9]* cyc  (p50/p99/max, n=80238)|\10/0/0 cyc  (p50/max/max, n=0)|
                          s|^\(  lock-site: core=0 site=0x80001750 \)max=41400|\1max=0|')"
    ctl 'a percentile walk reading the wrong end' 1 refuse 1 \
        "$(ctl_one | sed 's|^\(  lock-hold: \)2560/4096|\14096/2560|')"

    ctl 'a report whose rows name no statistic' 1 refuse 1 \
        "$(ctl_one | sed 's|(p50/p99/max,|(min/avg/max,|
                          s|(p50/max/max,|(min/avg/max,|')"
    ctl 'a populated row withholding its p99' 1 refuse 1 \
        "$(ctl_one | sed 's|^\(  lock-hold: .*\)(p50/p99/max,|\1(p50/max/max,|')"
    ctl 'a short row claiming a p99' 1 refuse 1 \
        "$(ctl_one | sed 's|^\(  irq: .*\)(p50/max/max,|\1(p50/p99/max,|')"
    ctl 'a short row whose middle column is not its max' 1 refuse 1 \
        "$(ctl_one | sed 's|^\(  irq: *\)1536/4600/4600|\11536/2300/4600|')"

    # Remove the smallest maximum so only the site count check fails.
    ctl 'a core whose maximum names no site' 4 refuse 1 \
        "$(ctl_four | sed '/^  lock-site: core=3 /d')"
    ctl 'a build that files no site at all' 1 refuse 1 \
        "$(ctl_one | sed 's|site=0x80001750|site=0x0|')"
    ctl 'a site maximum under the row it belongs to' 1 refuse 1 \
        "$(ctl_one | sed 's|^\(  lock-site: .*\)max=41400|\1max=100|')"

    ctl 'a probe that never took the cross-core lock' 4 refuse 1 \
        "$(ctl_four | sed 's|wait+1|wait+0|')"
    # Keep the aggregate total and active-core count valid while adding a row.
    ctl 'more per-core rows than the kernel has cores' 4 refuse 1 \
        "$(ctl_four | sed 's|^\(    core 3: 3000/5000/116736 cyc  (min/avg/max, n=\)20000)|\116000)\n    core 4: 3000/5000/110000 cyc  (min/avg/max, n=4000)|')"
    # A single active core also equals the aggregate, so both checks fail.
    ctl 'one core holding every hold sample' 4 refuse 2 \
        "$(ctl_four | sed 's|^\(    core 0: 3000/5000/131072 cyc  (min/avg/max, n=\)20000)|\180000)|
                           s|^\(    core 1: .*n=\)20000)|\10)|
                           s|^\(    core 2: .*n=\)20000)|\10)|
                           s|^\(    core 3: .*n=\)20000)|\10)|')"
    ctl 'rows that do not total the aggregate' 4 refuse 1 \
        "$(ctl_four | sed 's|^\(    core 3: 3000/5000/116736 cyc  (min/avg/max, n=\)20000)|\119000)|')"
    ctl 'a wait row that never sampled' 4 refuse 1 \
        "$(ctl_four | sed 's|^\(  lock-wait: \)512/1024/32768 cyc  (p50/p99/max, n=79000)|\10/0/0 cyc  (p50/max/max, n=0)|')"

    # Zero max also forces zero p50; the next control isolates p50 alone.
    ctl 'a bracket counting samples without timing them' 1 refuse 2 \
        "$(ctl_one | sed 's|^\(  lock-hold: \)2560/4096/41400|\10/0/0|
                          s|^\(  lock-site: .*\)max=41400|\1max=0|')"
    ctl 'a histogram the percentile walk finds nothing in' 1 refuse 1 \
        "$(ctl_one | sed 's|^\(  lock-hold: \)2560/4096|\10/4096|')"

    echo "PASS: $_ctl_pass planted report(s) accepted and $_ctl_refuse refused, each naming its
  own arm"
    exit 0
fi

read_report "$OUT"
lock_arms

assert_no_panic "the bench image panicked while the sweep ran"

if [ "$rc" -ne 0 ]; then
    exit 1
fi
_spread="at one kernel core, which prints no per-core block"
if [ "$want" -gt 1 ]; then
    _spread="over $spread of $want cores, wait n=$wn"
fi
echo "PASS: probe hold+$phold wait+$pwait at depth $pdepth; hold n=$hn p50=$h50 p99=$h99 max=$hmax $_spread; $labrows row(s) naming their own statistic; $sites site line(s)"
exit 0
