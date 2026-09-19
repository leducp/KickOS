#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Check one phase-count row per core, activity on multiple cores, and matching
# per-core and aggregate totals. Counts work even without a cycle source.
# --controls checks fixed reports and expected failure counts.
#
# Usage: check_bench_percore.sh <elf> <kernel cores> | --controls

set -u
. "$(dirname "$0")/../lib/gate.sh"
# Allow time for the call/reply sweep before the phase table.
: "${QEMU_TIMEOUT:=150}"

_usage="usage: check_bench_percore.sh <elf> <kernel cores> | check_bench_percore.sh --controls"
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
    if [ "$want" -le 1 ]; then
        fail "kernel core count is $want. At one core the bench prints no per-core block at all,
  so this gate belongs only on a preset whose kernel core count exceeds one"
    fi
    [ -f "$elf" ] || fail "no image at $elf"
    need_qemu_machine

    # Avoid end anchors when polling the raw CRLF log. OUT has CR removed.
    poll_image "$elf" "^  per-core phase samples:" > /dev/null

    if [ "$POLL_OK" -ne 1 ]; then
        fail "no per-core phase block within ${QEMU_TIMEOUT}s; the image printed no phase table"
    fi
fi

rc=0

# Compare the sum over phases with the sum over cores in the same capture.
# Phase core rows have no colon, unlike distribution core rows.
# Parse n before the optional SAT column; neither total includes saturated samples.
read_report() { # <report text>
    read -r prows ptot crows ctot spread <<EOF
$(printf '%s\n' "$1" | awk '
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
}

percore_arms() {
    # Stop on a missing table instead of checking default zero values.
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
}

# Full four-core RV64 phase-table fixture, including both closing probes.
ctl_four() {
    printf '%s\n' \
'  phase table (44 rows; cyc avg/max, min last and a floor; leaf -= NULL, composite -= NULL + k*(NEST-NULL)):' \
'    NULL             140/8000  min=120  n=260000' \
'    NEST             836/23260  min=760  n=260000' \
'    NEST_LOCK        1715/854420  min=1600  n=260000' \
'    CALL_TOTAL       73832/2774380  min=69840  n=259997' \
'    CALL_VALIDATE    637/874560  min=540  n=260000' \
'    CALL_LOCKED      70254/2769760  min=66500  n=259997' \
'    CALL_RESOLVE     446/60320  min=360  n=260000' \
'    CALL_PEEK        293/28000  min=260  n=260000' \
'    CALL_PROBE       243/173740  min=220  n=259997' \
'    CALL_POP         346/134100  min=300  n=259997' \
'    CALL_COPY        3681/884420  min=3320  n=259997' \
'    CALL_MINT        3792/888380  min=3560  n=259997' \
'    CALL_MINT_CAP    485/144000  min=440  n=259997' \
'    CALL_MINT_INFO   2166/885940  min=2000  n=259997' \
'    CALL_DONATE      290/68000  min=260  n=20000' \
'    CALL_PARK        1020/88620  min=900  n=259997' \
'    CALL_WAKE        52916/2751900  min=50120  n=259997' \
'    CALL_RESUME      345/97540  min=280  n=260000' \
'    CALL_SLOW_TOTAL  346813/477760  min=187980  n=3' \
'    CALL_SLOW_LOCKED 303033/378840  min=163500  n=3' \
'    CALL_SLOW_DONATE 0/0  min=0  n=0' \
'    CALL_SLOW_PARK   268306/347300  min=143100  n=3' \
'    RECV_LOCKED      56500/1463860  min=53620  n=259997' \
'    RECV_RESOLVE     530/125480  min=440  n=260000' \
'    RECV_SCAN        297/59600  min=260  n=259997' \
'    RECV_PARK        54233/1227340  min=51440  n=259997' \
'    REPLY_TOTAL      202953/1011280  min=114260  n=13' \
'    REPLY_VALIDATE   11656/26240  min=9160  n=13' \
'    REPLY_LOCKED     8250/893920  min=7700  n=260000' \
'    REPLY_LOOKUP     662/885340  min=600  n=260000' \
'    REPLY_COPY       3696/97200  min=3340  n=260000' \
'    REPLY_FUNNEL     482/102380  min=420  n=260000' \
'    REPLY_WAKE       1220/102040  min=1080  n=260000' \
'    REPLY_RECV_TOTAL 70647/2203760  min=66880  n=260000' \
'    REPLY_RECV_TAIL  3353/143540  min=3120  n=259997' \
'    WAKE_UNPARK      325/275220  min=260  n=520023' \
'    PICK_NEXT        629/52320  min=560  n=520239' \
'    SWITCH_TO        90004/1601778480  min=35280  n=520078' \
'    SWITCH_BOOK      288/156460  min=240  n=520104' \
'    MPU_APPLY        588/950560  min=520  n=520104' \
'    MPU_COMMIT       0/0  min=0  n=0' \
'    REENT_SEAT       2571/184620  min=240  n=520104' \
'    KTIME_REARM      551/97120  min=480  n=520104' \
'    ARCH_SWITCH      81684/1601664880  min=1100  n=520078' \
'  per-core phase samples:' \
'    core 0  n=4420229' \
'    core 1  n=2640127' \
'    core 2  n=2640208' \
'    core 3  n=1760263' \
'  ns-probe: rate=0 (nothing converts, so no ns column is printed)' \
'  sat-probe: width=64 fits+1/0 wide+0/1 max=1000'
}

# <name> <kernel cores> <pass|refuse> <findings expected on a refusal> <report text>
_ctl_pass=0
_ctl_refuse=0
ctl() {
    _ctl_save=$want
    want=$2
    # Run in a subshell so fail() cannot terminate the control suite.
    _ctl_out="$( ( rc=0; read_report "$5"; percore_arms; exit "$rc" ) 2>&1 )"
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
    ctl 'four cores, both blocks whole' 4 pass 0 "$(ctl_four)"

    # Remove both blocks to isolate the missing-table check from the totals check.
    ctl 'a capture carrying no phase block at all' 4 refuse 1 \
        "$(ctl_four | sed '/^  phase table/d
                           /^    [A-Z][A-Z_]*  /d
                           /^  per-core phase samples:/d
                           /^    core [0-9]*  n=/d')"
    # Split core 3 into two rows so totals and active-core counts remain valid.
    ctl 'more per-core rows than the kernel has cores' 4 refuse 1 \
        "$(ctl_four | sed 's|^    core 3  n=1760263|    core 3  n=1260263\n    core 4  n=500000|')"
    ctl 'one core holding every phase sample' 4 refuse 1 \
        "$(ctl_four | sed 's|^    core 0  n=4420229|    core 0  n=11460827|
                           s|^    core 1  n=2640127|    core 1  n=0|
                           s|^    core 2  n=2640208|    core 2  n=0|
                           s|^    core 3  n=1760263|    core 3  n=0|')"
    ctl 'rows that do not total the table' 4 refuse 1 \
        "$(ctl_four | sed 's|^    core 3  n=1760263|    core 3  n=1660263|')"

    echo "PASS: $_ctl_pass planted report(s) accepted and $_ctl_refuse refused, each naming its
  own arm"
    exit 0
fi

read_report "$OUT"
percore_arms

assert_no_panic "the bench image panicked while the sweep ran"

if [ "$rc" -ne 0 ]; then
    exit 1
fi
echo "PASS: $spread of $want cores accumulated, $ptot samples aggregated"
exit 0
