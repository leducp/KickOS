#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Check phase-table completeness and sample populations. The header supplies
# the expected row count; the build banner selects allowed empty phases.
# Use captures from the current tree because phase definitions and waivers change.
# --controls checks parser branches with fixed reports and expected failure counts.
#
# Usage: check_bench_phase_table.sh <elf> | --controls
#        BENCH_CAPTURE=<log> check_bench_phase_table.sh

set -u
. "$(dirname "$0")/../lib/gate.sh"
# Allow time for the call/reply sweep before the phase table.
: "${QEMU_TIMEOUT:=150}"

_usage="usage: check_bench_phase_table.sh <elf> | check_bench_phase_table.sh --controls"
controls_only=0
if [ "${1:-}" = "--controls" ]; then
    controls_only=1
    what="a planted report"
elif [ -n "${BENCH_CAPTURE:-}" ]; then
    [ -f "$BENCH_CAPTURE" ] || fail "no capture at $BENCH_CAPTURE"
    # Captures may contain CRLF and a leading 0xFF. Use byte-oriented awk parsing.
    OUT="$(tr -d '\r' < "$BENCH_CAPTURE")"
    what="$BENCH_CAPTURE"
else
    elf="${1:?$_usage}"
    [ -f "$elf" ] || fail "no image at $elf"
    need_qemu_machine
    # Avoid end anchors because the raw log may use CRLF.
    poll_image "$elf" "^  sat-probe:" > /dev/null
    if [ "$POLL_OK" -ne 1 ]; then
        fail "the phase table never reached its sat-probe line within ${QEMU_TIMEOUT}s"
    fi
    what="$elf"
fi

rc=0

# Keep the floor well below normal populations (about 20000 or more).
POP_FLOOR=1000

# Return a reason only when the phase is allowed to have no samples.
low_n_reason() { # <row>
    case "$1" in
    CALL_SLOW_TOTAL|CALL_SLOW_LOCKED|CALL_SLOW_DONATE|CALL_SLOW_PARK)
        echo "the slow call path is a one-shot probe: the app drives it once and the sweep
      never takes it again"
        ;;
    REPLY_TOTAL|REPLY_VALIDATE)
        echo "standalone-only since the fused reply-receive, which never enters the
      standalone wrapper; TODO.md's M8.12 section carries the reasoning and names REPLY_LOCKED
      as the reply row to read instead"
        ;;
    # MPU_COMMIT is empty with protection disabled. MPU_APPLY still records
    # the deferred descriptor stash and is not waived.
    MPU_COMMIT)
        if [ "$mpu_posture" = off ]; then
            echo "this image reports 'mpu off' in its banner, so no descriptor is ever
      programmed and the commit bracket cannot be reached"
        fi
        ;;
    # CMake disables KICKOS_LIBC_REENT on these two architectures.
    REENT_SEAT)
        if [ "$bench_arch" = sim ] || [ "$bench_arch" = x86_64 ]; then
            echo "this arch clears KICKOS_LIBC_REENT in the top-level CMakeLists, so no
      reentrancy seat is installed on a switch and the bracket cannot be reached"
        fi
        ;;
    esac
}

# `<row> <n>` per table row, with NONE where the row carries no n= field at all.
phase_row_pops() { # <report text>
    printf '%s\n' "$1" | awk '
        /^  phase table \([0-9]+ rows; / { state = 1; next }
        state == 1 && /^    [A-Z]/ {
            n = "NONE"
            for (i = 2; i <= NF; i++) {
                if ($i ~ /^n=[0-9]+$/) { n = substr($i, 3) }
            }
            printf "%s %s\n", $1, n
            next
        }
        state == 1 { state = 2 }'
}

# Read the declared count and build configuration from this capture.
# Lowercase per-core rows are not phase rows.
read_report() { # <report text>
    read -r want rows term mpu_posture bench_arch <<EOF
$(printf '%s\n' "$1" | awk '
    /^   mpu +/  && mpu  == "" { mpu  = $2 }
    /^   arch +/ && arch == "" { arch = $2 }
    /^  phase table \([0-9]+ rows; / {
        want = $0
        sub(/^  phase table \(/, "", want)
        sub(/ rows;.*$/, "", want)
        state = 1
        next
    }
    state == 1 && /^    [A-Z]/ { rows++; next }
    state == 1                 { state = 2 }
    # Read the terminator independently so a missing header produces one failure.
    /^  sat-probe: / { term = 1 }
    END {
        if (mpu  == "") { mpu  = "NONE" }
        if (arch == "") { arch = "NONE" }
        printf "%d %d %d %s %s\n", want + 0, rows + 0, term + 0, mpu, arch
    }')
EOF
    # Parse the per-row populations separately from the scalar summary.
    pops="$(phase_row_pops "$1")"
}

phase_table_arms() {
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

    # Stop if the build configuration needed by the population waivers is missing.
    if [ "$mpu_posture" != enforce ] && [ "$mpu_posture" != off ]; then
        fail "the banner states no MPU posture, so whether MPU_COMMIT is meant to carry samples
  cannot be read from this capture and a zero there cannot be told from the blindness this
  arm exists to catch"
    fi
    if [ "$bench_arch" = NONE ]; then
        fail "the banner names no arch, so whether REENT_SEAT is meant to carry samples cannot be
  read from this capture"
    fi

    # Check populations as well as row presence.
    while read -r row n; do
        [ -n "$row" ] || continue
        if [ "$n" = NONE ]; then
            bad "the phase table row $row carries no n= field, so its population cannot be read
      and an absent field would satisfy the check below it"
            continue
        fi
        _reason="$(low_n_reason "$row")"
        if [ -n "$_reason" ]; then
            if [ "$n" -ge "$POP_FLOOR" ]; then
                bad "$row is waived as a quiet row and carries $n samples, so the reason it was
      waived for has stopped holding: $_reason. Re-read TODO.md's M8.12 section and drop the
      waiver rather than widening it"
            fi
            continue
        fi
        if [ "$n" -lt "$POP_FLOOR" ]; then
            bad "the phase table row $row carries $n samples against a floor of $POP_FLOOR. A row
      keeps its span when its feed goes away, so the figure still reads plausibly and stands
      for nothing: find what stopped feeding the bracket, and if the row is MEANT to be quiet
      name it in low_n_reason in this gate with the reason"
        fi
    done <<POPS
$pops
POPS
}

# Parser self-checks run in every mode to catch patterns that match no rows.
parse_controls() {
    _ctl="$(phase_row_pops '  phase table (2 rows; cyc avg/max):
    CTL_BUSY         10/10  min=10  n=220000
    CTL_QUIET        10/10  min=10  n=7
  sat-probe: width=32')"
    printf '%s\n' "$_ctl" | grep -qx 'CTL_QUIET 7' \
        || fail "the population scan cannot read a collapsed row out of a synthetic table, so a
  green verdict from it says nothing about a real one"
    printf '%s\n' "$_ctl" | grep -qx 'CTL_BUSY 220000' \
        || fail "the population scan cannot read a populated row, so it would refuse every
  capture or none"
    _ctl="$(phase_row_pops '  phase table (1 rows; cyc avg/max):
    CTL_NOFIELD      10/10  min=10
  sat-probe: width=32')"
    printf '%s\n' "$_ctl" | grep -qx 'CTL_NOFIELD NONE' \
        || fail "a row carrying no n= field does not read as absent, so the arm below is
  satisfied by exactly the thing it exists to catch"
    # Exercise the actual parser because MPU posture controls the waiver.
    _ctl="$(read_report '   mpu     enforce'; printf '%s\n' "$mpu_posture")"
    [ "$_ctl" = enforce ] \
        || fail "the banner posture parse cannot read 'enforce' out of a banner line, so every
  capture would be refused or every MPU_COMMIT waived"
    _ctl="$(read_report '   arch    armv7m'; printf '%s\n' "$bench_arch")"
    [ "$_ctl" = armv7m ] \
        || fail "the banner arch parse cannot read an arch out of a banner line, so every capture
  would be refused or every REENT_SEAT waived"
}

# RV32 fixture: MPU disabled, libc reentrancy enabled.
ctl_rv32() {
    printf '%s\n' \
'   board   qemu-riscv' \
'   arch    rv32imac' \
'   cpu     rv32imac_zicsr' \
'   mpu     off' \
'   sched   tickless' \
'   build   2026-09-19 13:03:09 +0200' \
'   app     Sep 19 2026 13:02:57 +0200' \
'   commit  443562b4-dirty' \
'   heap    16 KiB available' \
'   kstack  1184 B x 17 = 20128 B' \
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
'  sat-probe: width=32 (no delta this counter forms is too wide)'
}

# Shortened x86-64 fixture with its row count adjusted. REENT_SEAT is waived.
ctl_x86() {
    printf '%s\n' \
'   board   qemu-x86_64' \
'   arch    x86_64' \
'   cpu     x86-64' \
'   mpu     off' \
'   sched   tickless' \
'  phase table (8 rows; cyc avg/max, min last and a floor; leaf -= NULL, composite -= NULL + k*(NEST-NULL)):' \
'    NULL             15/4500  min=1  n=260000' \
'    NEST             80/24920  min=60  n=260000' \
'    CALL_TOTAL       9456/1679340  min=9080  n=259999' \
'    REPLY_TOTAL      35953/258200  min=2500  n=13' \
'    MPU_APPLY        16/6380  min=1  n=520039' \
'    MPU_COMMIT       0/0  min=0  n=0' \
'    REENT_SEAT       0/0  min=0  n=0' \
'    ARCH_SWITCH      10839/210995400  min=5400  n=520013' \
'  ns-probe: cyc=4294967295 ns=2148712634 capped=0' \
'  sat-probe: width=64 fits+1/0 wide+0/1 max=1000'
}

# <name> <pass|refuse> <findings expected on a refusal> <report text>
_ctl_pass=0
_ctl_refuse=0
ctl() {
    # Run in a subshell so fail() cannot terminate the control suite.
    _ctl_out="$( ( rc=0; read_report "$4"; phase_table_arms; exit "$rc" ) 2>&1 )"
    _ctl_got=$?
    _ctl_n="$(printf '%s\n' "$_ctl_out" | grep -c '^FAIL: ')"
    if [ "$2" = pass ] && [ "$_ctl_got" -ne 0 ]; then
        fail "the '$1' control is REFUSED, and it is a report this gate must accept; every
  capture below would be refused for the same reason:
$_ctl_out"
    fi
    if [ "$2" = refuse ] && [ "$_ctl_got" -eq 0 ]; then
        fail "the '$1' control PASSES, so this gate cannot see that class and a capture
  carrying it reads as clean"
    fi
    # Require the expected failure count to isolate each validation.
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
    parse_controls

    ctl 'a real one-core rv32 capture' pass 0 "$(ctl_rv32)"
    ctl 'a trimmed capture from the arch that seats no reentrancy' pass 0 "$(ctl_x86)"

    ctl 'a capture with no phase-table header' refuse 1 \
        "$(ctl_rv32 | sed '/^  phase table (/d')"
    ctl 'a table announcing more rows than arrived' refuse 1 \
        "$(ctl_rv32 | sed '/^    CALL_PEEK /d')"
    ctl 'a table whose sat-probe terminator never came' refuse 1 \
        "$(ctl_rv32 | sed '/^  sat-probe: /d')"

    # Feed MPU_COMMIT so only the missing posture check fails.
    ctl 'a capture whose banner states no MPU posture' refuse 1 \
        "$(ctl_rv32 | sed '/^   mpu     /d
                           s|^\(    MPU_COMMIT  *\)0/0  min=0  n=0|\158/5140  min=40  n=520039|')"
    ctl 'a capture whose banner names no arch' refuse 1 \
        "$(ctl_rv32 | sed '/^   arch    /d')"

    ctl 'a busy row collapsed below the floor' refuse 1 \
        "$(ctl_rv32 | sed 's|^\(    RECV_LOCKED      5484/252420  min=5120  \)n=259999|\1n=7|')"
    ctl 'a row carrying no n= field' refuse 1 \
        "$(ctl_rv32 | sed 's|^\(    CALL_RESUME      245/19500  min=220\)  n=220000|\1|')"
    ctl 'a waived row whose population recovered' refuse 1 \
        "$(ctl_rv32 | sed 's|^\(    REPLY_TOTAL .*\)n=13$|\1n=260000|')"

    # Exercise both sides of each configuration-dependent waiver.
    ctl 'MPU_COMMIT zeroed under mpu enforce' refuse 1 \
        "$(ctl_rv32 | sed 's|^   mpu     off|   mpu     enforce|')"
    ctl 'MPU_COMMIT fed under mpu off' refuse 1 \
        "$(ctl_rv32 | sed 's|^\(    MPU_COMMIT  *\)0/0  min=0  n=0|\158/5140  min=40  n=520039|')"
    ctl 'REENT_SEAT zeroed on an arch that seats it' refuse 1 \
        "$(ctl_rv32 | sed 's|^\(    REENT_SEAT  *\)283/27100  min=240  n=520039|\10/0  min=0  n=0|')"

    echo "PASS: $_ctl_pass planted report(s) accepted and $_ctl_refuse refused, each naming its
  own arm"
    exit 0
fi

parse_controls
read_report "$OUT"
phase_table_arms

assert_no_panic "the bench image panicked while the report printed"

if [ "$rc" -ne 0 ]; then
    exit 1
fi
echo "PASS: $rows of $want phase rows, every population above $POP_FLOOR or waived, and the
  sat-probe terminator"
exit 0
