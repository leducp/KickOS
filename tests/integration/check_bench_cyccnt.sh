#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Check that both assembly switch timing and C phase timing use a moving counter.
# This checks counter activity, not its units or rate.
# --controls checks fixed reports and expected TAP failure counts.
#
# Usage: check_bench_cyccnt.sh <bench image> | --controls

set -u
. "$(dirname "$0")/../lib/gate.sh"

# Allow time for the call/reply sweep and throughput windows on emulated SMP.
: "${QEMU_TIMEOUT:=240}"

_usage="usage: check_bench_cyccnt.sh <bench image> | check_bench_cyccnt.sh --controls"
controls_only=0
if [ "${1:-}" = "--controls" ]; then
    controls_only=1
    elf=""
else
    elf="${1:?$_usage}"
fi

read_report() { # <report text>
    REPORT="$1"

    # Read the last switch window. Allow variable padding after the label.
    SW="$(printf '%s\n' "$REPORT" \
          | sed -n 's|.*switch: \{1,\}\([0-9]\{1,\}\)/\([0-9]\{1,\}\)/\([0-9]\{1,\}\) cyc.*n=\([0-9]\{1,\}\)).*|\1 \2 \3 \4|p' \
          | tail -n1)"
    P50=0
    P99=0
    MAX=0
    N=0
    if [ -n "$SW" ]; then
        P50="${SW%% *}"
        REST="${SW#* }"
        P99="${REST%% *}"
        REST="${REST#* }"
        MAX="${REST%% *}"
        N="${REST#* }"
    fi

    ROWS="$(printf '%s\n' "$REPORT" | sed -n 's|^ *[A-Z][A-Z_]* \{1,\}[0-9]\{1,\}/\([0-9]\{1,\}\) \{1,\}min=.*|\1|p')"
    # Do not count the empty line printf emits for an empty string.
    LIVE=0
    if [ -n "$ROWS" ]; then
        LIVE="$(printf '%s\n' "$ROWS" | grep -c -v '^0$')"
    fi
}

cyccnt_arms() {
    if [ -z "$SW" ]; then
        echo "not ok 1 - the run printed no switch line, so nothing reports the bracket at all"
        printf '%s\n' "$REPORT" | tail -n 15 | sed 's/^/# /'
        exit 1
    fi
    echo "ok 1 - the run printed a switch line"

    # An empty accumulator also has a zero median, so its control fails both checks.
    if [ "$N" -eq 0 ]; then
        echo "not ok 2 - the switch accumulator took no sample, so the arch switch path feeds it"
        echo "#          nothing (n=0)"
        rc=1
    else
        echo "ok 2 - the arch switch path fed the accumulator $N sample(s)"
    fi

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

    # A missing table and a table with all-zero maxima share this check.
    if [ -z "$ROWS" ]; then
        echo "not ok 4 - the run printed no phase table, so the C-side reader is unwitnessed"
        rc=1
    elif [ "$LIVE" -eq 0 ]; then
        echo "not ok 4 - every phase row has a maximum of 0, so the brackets in"
        echo "#          kernel/include/kickos/bench.h read a counter that does not advance"
        rc=1
    else
        echo "ok 4 - $LIVE phase row(s) carry a non-zero maximum"
    fi
}

# RV32 fixture with a full phase table and two throughput windows.
ctl_rv32() {
    printf '%s\n' \
'cycle counter: 0 Hz (0 = no rate converts a reading; cycles only)' \
'' \
'  phase table (44 rows; cyc avg/max, min last and a floor; leaf -= NULL, composite -= NULL + k*(NEST-NULL)):' \
'    NULL             139/10340  min=100  n=220000' \
'    NEST             522/25100  min=480  n=220000' \
'    NEST_LOCK        300/50400  min=280  n=220000' \
'    CALL_TOTAL       12806/712460  min=12160  n=219999' \
'    CALL_VALIDATE    377/26980  min=340  n=220000' \
'    CALL_LOCKED      11360/570480  min=10780  n=219999' \
'    CALL_RESOLVE     424/25600  min=360  n=220000' \
'    CALL_PEEK        229/52140  min=180  n=220000' \
'    CALL_PROBE       215/30040  min=180  n=219999' \
'    CALL_POP         311/17180  min=280  n=219999' \
'    CALL_COPY        465/65640  min=360  n=219999' \
'    CALL_MINT        1367/87700  min=1280  n=219999' \
'    CALL_MINT_CAP    453/21780  min=420  n=219999' \
'    CALL_MINT_INFO   287/25540  min=240  n=219999' \
'    CALL_DONATE      212/21640  min=180  n=19999' \
'    CALL_PARK        518/28620  min=460  n=219999' \
'    CALL_WAKE        4171/124080  min=3900  n=219999' \
'    CALL_RESUME      245/19500  min=220  n=220000' \
'    CALL_SLOW_TOTAL  182440/182440  min=182440  n=1' \
'    CALL_SLOW_LOCKED 164340/164340  min=164340  n=1' \
'    CALL_SLOW_DONATE 0/0  min=0  n=0' \
'    CALL_SLOW_PARK   58240/58240  min=58240  n=1' \
'    RECV_LOCKED      5484/252420  min=5120  n=259999' \
'    RECV_RESOLVE     485/91480  min=380  n=260000' \
'    RECV_SCAN        181/46100  min=160  n=259999' \
'    RECV_PARK        3979/84620  min=3700  n=259999' \
'    REPLY_TOTAL      49013/232300  min=13580  n=13' \
'    REPLY_VALIDATE   9361/32680  min=4740  n=13' \
'    REPLY_LOCKED     2674/374640  min=2440  n=260000' \
'    REPLY_LOOKUP     439/175680  min=400  n=260000' \
'    REPLY_COPY       344/39200  min=240  n=260000' \
'    REPLY_FUNNEL     344/69280  min=300  n=260000' \
'    REPLY_WAKE       745/40920  min=600  n=260000' \
'    REPLY_RECV_TOTAL 10155/561880  min=9420  n=260000' \
'    REPLY_RECV_TAIL  858/98680  min=560  n=259999' \
'    WAKE_UNPARK      209/920600  min=120  n=520012' \
'    PICK_NEXT        226/37400  min=160  n=480064' \
'    SWITCH_TO        2672/182520  min=2380  n=480039' \
'    SWITCH_BOOK      226/160280  min=140  n=520039' \
'    MPU_APPLY        57/5060  min=40  n=520039' \
'    MPU_COMMIT       0/0  min=0  n=0' \
'    REENT_SEAT       283/27100  min=240  n=520039' \
'    KTIME_REARM      208/14720  min=140  n=520039' \
'    ARCH_SWITCH      804/115460  min=640  n=480039' \
'  ns-probe: rate=0 (nothing converts, so no ns column is printed)' \
'  sat-probe: width=32 (no delta this counter forms is too wide)' \
'' \
'  throughput: 151547 ctx-sw/s  (6598 ns/sw avg over 40000 switches / 263 ms)' \
'  switch-probe: fastpath-swaps=0  (swapped inside the trap, so not in the switch row)' \
'  switch:    1536/1664/9520 cyc  (p50/p99/max, n=40001)' \
'  lock-hold: 2560/4096/32280 cyc  (p50/p99/max, n=80240)' \
'  lock-site: core=0 site=0x80003bb2 max=32280' \
'  irq:       1536/171760/171760 cyc  (p50/max/max, n=100)' \
'  throughput: 153082 ctx-sw/s  (6532 ns/sw avg over 40000 switches / 261 ms)' \
'  switch-probe: fastpath-swaps=0  (swapped inside the trap, so not in the switch row)' \
'  switch:    1536/1664/9420 cyc  (p50/p99/max, n=40001)' \
'  lock-hold: 2560/4096/41400 cyc  (p50/p99/max, n=80238)' \
'  lock-site: core=0 site=0x80001750 max=41400' \
'  irq:       1536/4600/4600 cyc  (p50/max/max, n=100)'
}

# <name> <pass|refuse> <findings expected on a refusal> <report text>
_ctl_pass=0
_ctl_refuse=0
ctl() {
    # Run in a subshell so an early exit cannot terminate the control suite.
    _ctl_out="$( ( rc=0; read_report "$4"; cyccnt_arms; exit "$rc" ) 2>&1 )"
    _ctl_got=$?
    _ctl_n="$(printf '%s\n' "$_ctl_out" | grep -c '^not ok ')"
    if [ "$2" = pass ] && [ "$_ctl_got" -ne 0 ]; then
        fail "the '$1' control is REFUSED, and it is a report this gate must accept; every
  capture below would be refused for the same reason:
$_ctl_out"
    fi
    if [ "$2" = refuse ] && [ "$_ctl_got" -eq 0 ]; then
        fail "the '$1' control PASSES, so this gate cannot see that class and a capture
  carrying it reads as clean"
    fi
    # Check the failure count so another check cannot hide a missing validation.
    if [ "$2" = refuse ] && [ "$_ctl_n" -ne "$3" ]; then
        fail "the '$1' control produced $_ctl_n finding(s) where $3 was expected, so it does not
  isolate the arm it was written for:
$_ctl_out"
    fi
    if [ "$2" = pass ]; then
        _ctl_pass=$((_ctl_pass + 1))
    else
        _ctl_refuse=$((_ctl_refuse + 1))
    fi
}

if [ "$controls_only" -eq 1 ]; then
    ctl 'a complete report from a live counter' pass 0 "$(ctl_rv32)"

    ctl 'a report with no switch line' refuse 1 \
        "$(ctl_rv32 | sed '/^  switch: /d')"
    # An empty accumulator has zero values and uses the low-sample max label.
    ctl 'a switch path that feeds the accumulator nothing' refuse 2 \
        "$(ctl_rv32 | sed 's|^\(  switch: *\)[0-9]*/[0-9]*/[0-9]* cyc  (p50/p99/max, n=[0-9]*)|\10/0/0 cyc  (p50/max/max, n=0)|')"
    ctl 'a histogram the percentile walk finds nothing in' refuse 1 \
        "$(ctl_rv32 | sed 's|^\(  switch: *\)[0-9]*/|\10/|')"
    ctl 'a counter frozen at a constant' refuse 1 \
        "$(ctl_rv32 | sed 's|^\(  switch: *\)[0-9]*/[0-9]*/[0-9]*|\11536/1536/1536|')"

    ctl 'a report whose phase table did not arrive' refuse 1 \
        "$(ctl_rv32 | sed '/^    [A-Z][A-Z_]* /d')"
    ctl 'a C-side reader that measures nothing' refuse 1 \
        "$(ctl_rv32 | sed 's|^\(    [A-Z][A-Z_]*  *\)[0-9]*/[0-9]*  min=[0-9]*|\10/0  min=0|')"

    echo "PASS: $_ctl_pass planted report(s) accepted and $_ctl_refuse refused, each naming its
  own arm"
    exit 0
fi

echo "1..4"

run_image "$elf"
OUT_ALL="$OUT"

rc=0
read_report "$OUT_ALL"
cyccnt_arms

if [ "$rc" -ne 0 ]; then
    printf '%s\n' "$OUT_ALL" | grep -E 'cycle counter|switch:' | sed 's/^/# /'
fi

# Reject faults after a completed report too. Interleaved SMP output may hide
# a fault marker, so this is not a complete fault check.
assert_no_panic "the bench image panicked while the sweep ran"
exit "$rc"
