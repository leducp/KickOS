#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Check that oversized samples increment SAT without entering the statistics.
# Compare the reported tick width with the image architecture. The 64-bit probe
# uses one valid and one oversized sample; 32-bit ticks cannot exercise saturation.
# Also check nanosecond conversion and capping against the reported counter rate.
# Emulator rates do not exercise capping, so fixed controls cover that case.
# --controls checks fixed reports and expected TAP failure counts.
#
# Usage: check_bench_saturate.sh <bench image> | --controls

set -u
. "$(dirname "$0")/../lib/gate.sh"

# Allow time for the call/reply sweep before the phase table.
: "${QEMU_TIMEOUT:=240}"

_usage="usage: check_bench_saturate.sh <bench image> | check_bench_saturate.sh --controls"
controls_only=0
if [ "${1:-}" = "--controls" ]; then
    controls_only=1
    elf=""
    # Initialize the values ctl saves and restores under set -u.
    WANT=32
    MACH="a planted machine"
else
    elf="${1:?$_usage}"

    # Read the architecture independently from the image header. gate.sh sets LC_ALL=C.
    MACH="$(readelf -h "$elf" 2>/dev/null | sed -n 's/^ *Machine: *//p' | head -n1)"
    CLASS="$(readelf -h "$elf" 2>/dev/null | sed -n 's/^ *Class: *//p' | head -n1)"
    if [ -z "$MACH" ]; then
        # The x86-64 UEFI image uses PE32+, so identify it by file type.
        case "$elf" in
            *.efi) MACH="Advanced Micro Devices X86-64"; CLASS="ELF64" ;;
            *) fail "readelf could not read $elf, so nothing here knows what machine it is for" ;;
        esac
    fi

    # These architectures use 64-bit counters; all other supported counters are 32-bit.
    WANT=32
    case "$MACH" in
        *AArch64*|*X86-64*) WANT=64 ;;
        *RISC-V*) if [ "$CLASS" = "ELF64" ]; then WANT=64; fi ;;
        *) ;;
    esac
fi

# Flag maxima in the top 64 KiB as possible wrapped deltas.
BAND=4294901760

read_report() { # <report text>
    REPORT="$1"

    PROBE="$(printf '%s\n' "$REPORT" | grep '^  sat-probe: ' | tail -n1)"
    GOT="$(printf '%s\n' "$PROBE" | sed -n 's/.*width=\([0-9]\{1,\}\).*/\1/p')"
    FITS="$(printf '%s\n' "$PROBE" | sed -n 's/.*fits+\([0-9]\{1,\}\/[0-9]\{1,\}\).*/\1/p')"
    WIDE="$(printf '%s\n' "$PROBE" | sed -n 's/.*wide+\([0-9]\{1,\}\/[0-9]\{1,\}\).*/\1/p')"
    MAXP="$(printf '%s\n' "$PROBE" | sed -n 's/.*max=\([0-9]\{1,\}\).*/\1/p')"

    NSP="$(printf '%s\n' "$REPORT" | grep '^  ns-probe: ' | tail -n1)"
    NSV="$(printf '%s\n' "$NSP" | sed -n 's/.*ns=\([0-9]\{1,\}\).*/\1/p')"
    NSC="$(printf '%s\n' "$NSP" | sed -n 's/.*capped=\([0-9]\{1,\}\).*/\1/p')"
    HZ="$(printf '%s\n' "$REPORT" | sed -n 's/^cycle counter: \([0-9]\{1,\}\) Hz.*/\1/p' | head -n1)"

    # Exclude rows with SAT: migration between unsynchronized counters can also
    # produce a near-limit maximum alongside saturated samples.
    read -r WRAPPED ROWS <<EOF
$(printf '%s\n' "$REPORT" | awk -v b="$BAND" '
    /^ +[A-Z][A-Z_]+ +[0-9]+\/[0-9]+ +min=/ {
        rows++
        split($2, f, "/")
        if (f[2] + 0 >= b && $0 !~ /SAT=/) { n++ }
    }
    END { printf "%d %d\n", n + 0, rows + 0 }')
EOF
}

sat_arms() {
    # Keep checking rows even if the saturation probe is missing.
    if [ -z "$PROBE" ]; then
        echo "not ok 1 - the run printed no sat-probe line, so the accumulator is unwitnessed"
        echo "not ok 2 - no sat-probe line, so nothing states the bench tick's width"
        echo "not ok 3 - no sat-probe line"
        echo "not ok 4 - no sat-probe line"
        rc=1
    else
        echo "ok 1 - the run printed a sat-probe line"

        if [ "$GOT" != "$WANT" ]; then
            echo "not ok 2 - the bench tick is $GOT bits on a machine whose counter is $WANT ($MACH):"
            echo "#          a span longer than the narrower width wraps into the row"
            rc=1
        else
            echo "ok 2 - the bench tick is $WANT bits, as the machine's counter is ($MACH)"
        fi

        if [ "$WANT" = "64" ]; then
            # A missing field affects both probe checks. Continue to check the table too.
            if [ -z "$FITS" ] || [ -z "$WIDE" ] || [ -z "$MAXP" ]; then
                echo "not ok 3 - the sat-probe line carries no count fields: $PROBE"
                echo "not ok 4 - the sat-probe line carries no count fields"
                rc=1
            else
                if [ "$FITS" != "1/0" ]; then
                    echo "not ok 3 - a representable sample moved the accumulator by $FITS (want 1/0),"
                    echo "#          so the statistics and SAT do not partition the samples"
                    rc=1
                else
                    echo "ok 3 - a representable sample lands in the statistics and not in SAT"
                fi

                # An oversized sample must affect only sat; count and max must stay unchanged.
                if [ "$WIDE" != "0/1" ]; then
                    echo "not ok 4 - a sample wider than the statistics moved the accumulator by $WIDE"
                    echo "#          (want 0/1): 0/0 drops it unrecorded, 1/1 counts it in both"
                    rc=1
                elif [ "$MAXP" != "1000" ]; then
                    echo "not ok 4 - the maximum reads $MAXP after a sample too wide to record"
                    echo "#          (want 1000), so a saturated sample still moved a statistic"
                    rc=1
                else
                    echo "ok 4 - a sample wider than the statistics lands in SAT and moves no statistic"
                fi
            fi
        else
            echo "ok 3 - a 32-bit counter forms no delta too wide to record # SKIP"
            echo "ok 4 - a 32-bit counter forms no delta too wide to record # SKIP"
        fi
    fi

    if [ "$ROWS" -eq 0 ]; then
        echo "not ok 5 - the run printed no phase table, so its maxima say nothing"
        rc=1
    elif [ "$WRAPPED" -ne 0 ]; then
        echo "not ok 5 - $WRAPPED phase row(s) of $ROWS report a maximum at or above $BAND and no"
        echo "#          saturation count, which is a wrapped counter read as a cost"
        printf '%s\n' "$REPORT" | awk -v b="$BAND" \
            '/^ +[A-Z][A-Z_]+ +[0-9]+\/[0-9]+ +min=/ { split($2, f, "/")
                 if (f[2] + 0 >= b && $0 !~ /SAT=/) { print "# " $0 } }'
        rc=1
    else
        echo "ok 5 - no phase row of $ROWS carries a wrapped maximum"
    fi

    # Compute the expected nanoseconds and cap count from the reported rate.
    # The multiplication fits signed 64-bit shell arithmetic.
    if [ -z "$NSP" ]; then
        echo "not ok 6 - the run printed no ns-probe line, so the cycle-to-nanosecond conversion is"
        echo "#          unwitnessed and a truncating build reads as a correct one"
        rc=1
    elif printf '%s\n' "$NSP" | grep -q 'rate=0'; then
        echo "ok 6 - this board converts no reading into time, so it prints no ns column # SKIP"
    elif [ -z "$HZ" ] || [ "$HZ" = "0" ]; then
        echo "not ok 6 - the ns-probe reports a rate and the report's own rate line says $HZ Hz, so"
        echo "#          the two disagree about whether anything converts"
        rc=1
    elif [ -z "$NSV" ] || [ -z "$NSC" ]; then
        echo "not ok 6 - the ns-probe line carries no value and cap fields: $NSP"
        rc=1
    else
        WANTNS=$(( 4294967295 * 1000000000 / HZ ))
        WANTC=0
        if [ "$WANTNS" -gt 4294967295 ]; then
            WANTNS=4294967295
            WANTC=1
        fi
        if [ "$NSC" != "$WANTC" ]; then
            echo "not ok 6 - the ns-probe reports capped=$NSC where $HZ Hz makes it $WANTC, so"
            echo "#          the cap count does not follow the rate the report names"
            rc=1
        elif [ "$NSV" != "$WANTNS" ]; then
            echo "not ok 6 - the ns-probe converts 4294967295 cyc at $HZ Hz to $NSV ns where the"
            echo "#          field holds $WANTNS. A conversion that truncates instead of capping"
            echo "#          prints a plausible small number beside a correct cycle figure"
            rc=1
        else
            echo "ok 6 - 4294967295 cyc at $HZ Hz reports $NSV ns with capped=$NSC"
        fi
    fi
}

# RV32 and RV64 fixtures with the rate line and phase table.
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
'  sat-probe: width=32 (no delta this counter forms is too wide)'
}

ctl_rv64() {
    printf '%s\n' \
'cycle counter: 0 Hz (0 = no rate converts a reading; cycles only)' \
'' \
'  phase table (44 rows; cyc avg/max, min last and a floor; leaf -= NULL, composite -= NULL + k*(NEST-NULL)):' \
'    NULL             188/7360  min=160  n=260000' \
'    NEST             688/30700  min=580  n=260000' \
'    NEST_LOCK        773/41180  min=680  n=260000' \
'    CALL_TOTAL       45460/3075300  min=41540  n=259999' \
'    CALL_VALIDATE    508/18100  min=440  n=260000' \
'    CALL_LOCKED      43623/2955660  min=39820  n=259999' \
'    CALL_RESOLVE     757/41240  min=640  n=260000' \
'    CALL_PEEK        293/37060  min=240  n=260000' \
'    CALL_PROBE       232/33140  min=180  n=259999' \
'    CALL_POP         400/63100  min=340  n=259999' \
'    CALL_COPY        3028/155900  min=2700  n=259999' \
'    CALL_MINT        3098/225680  min=2780  n=259999' \
'    CALL_MINT_CAP    548/135820  min=480  n=259999' \
'    CALL_MINT_INFO   1796/135940  min=1600  n=259999' \
'    CALL_DONATE      270/13900  min=240  n=19999' \
'    CALL_PARK        676/97840  min=600  n=259999' \
'    CALL_WAKE        30485/2181840  min=27840  n=259999' \
'    CALL_RESUME      338/31180  min=280  n=260000' \
'    CALL_SLOW_TOTAL  373500/373500  min=373500  n=1' \
'    CALL_SLOW_LOCKED 360040/360040  min=360040  n=1' \
'    CALL_SLOW_DONATE 0/0  min=0  n=0' \
'    CALL_SLOW_PARK   271060/271060  min=271060  n=1' \
'    RECV_LOCKED      34855/1730260  min=31840  n=259999' \
'    RECV_RESOLVE     787/115620  min=680  n=260000' \
'    RECV_SCAN        385/100120  min=340  n=259999' \
'    RECV_PARK        32900/1513020  min=30020  n=259999' \
'    REPLY_TOTAL      97538/905680  min=18300  n=13' \
'    REPLY_VALIDATE   7378/22260  min=5120  n=13' \
'    REPLY_LOCKED     6371/1235980  min=5780  n=260000' \
'    REPLY_LOOKUP     730/723140  min=620  n=260000' \
'    REPLY_COPY       2912/126340  min=2600  n=260000' \
'    REPLY_FUNNEL     388/194340  min=320  n=260000' \
'    REPLY_WAKE       913/77940  min=800  n=260000' \
'    REPLY_RECV_TOTAL 45013/2175880  min=41120  n=260000' \
'    REPLY_RECV_TAIL  1070/91000  min=940  n=259999' \
'    WAKE_UNPARK      293/35480  min=240  n=520012' \
'    PICK_NEXT        275/19440  min=240  n=520064' \
'    SWITCH_TO        54593/1203516560  min=26200  n=520013' \
'    SWITCH_BOOK      274/188180  min=220  n=520039' \
'    MPU_APPLY        384/27840  min=320  n=520039' \
'    MPU_COMMIT       0/0  min=0  n=0' \
'    REENT_SEAT       1943/81080  min=1700  n=520039' \
'    KTIME_REARM      211/162280  min=180  n=520039' \
'    ARCH_SWITCH      50451/1203501420  min=22400  n=520013' \
'  ns-probe: rate=0 (nothing converts, so no ns column is printed)' \
'  sat-probe: width=64 fits+1/0 wide+0/1 max=1000'
}

# At 2.4 GHz, UINT32_MAX cycles fits in a 32-bit nanosecond field.
ctl_fast() {
    ctl_rv64 | sed 's|^cycle counter: .*|cycle counter: 2400000000 Hz (0 = no rate converts a reading; cycles only)|
                    s|^  ns-probe: .*|  ns-probe: cyc=4294967295 ns=1789569706 capped=0|'
}

# At 100 MHz, UINT32_MAX cycles requires capping the nanosecond result.
ctl_capped() {
    ctl_rv32 | sed 's|^cycle counter: .*|cycle counter: 100000000 Hz (0 = no rate converts a reading; cycles only)|
                    s|^  ns-probe: .*|  ns-probe: cyc=4294967295 ns=4294967295 capped=1|'
}

# <name> <bench tick width> <pass|refuse> <findings expected on a refusal> <report text>
_ctl_pass=0
_ctl_refuse=0
ctl() {
    _ctl_save=$WANT
    WANT=$2
    # MACH is used only in diagnostics.
    _ctl_mach=$MACH
    MACH="a planted $2-bit machine"
    # Run in a subshell so an early exit cannot terminate the control suite.
    _ctl_out="$( ( rc=0; read_report "$5"; sat_arms; exit "$rc" ) 2>&1 )"
    _ctl_got=$?
    WANT=$_ctl_save
    MACH=$_ctl_mach
    _ctl_n="$(printf '%s\n' "$_ctl_out" | grep -c '^not ok ')"
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
    ctl 'a 32-bit tick, the rule compiled out' 32 pass 0 "$(ctl_rv32)"
    ctl 'a 64-bit tick, both probe samples partitioned' 64 pass 0 "$(ctl_rv64)"
    ctl 'a rate the nanosecond field holds' 64 pass 0 "$(ctl_fast)"
    ctl 'a rate that overruns the nanosecond field' 32 pass 0 "$(ctl_capped)"
    # A near-limit maximum with SAT is allowed for cross-core counter differences.
    ctl 'a row that saturated rather than wrapped' 64 pass 0 \
        "$(ctl_rv64 | sed 's|^    SWITCH_TO .*|    SWITCH_TO        54593/4294950000  min=26200  n=520013  SAT=2|')"

    ctl 'a report with no sat-probe line' 32 refuse 4 \
        "$(ctl_rv32 | sed '/^  sat-probe: /d')"
    # Use a 64-bit probe on a 32-bit machine to isolate the width check.
    ctl 'a tick wider than the machine it runs on' 32 refuse 1 \
        "$(ctl_rv32 | sed 's|^  sat-probe: .*|  sat-probe: width=64 fits+1/0 wide+0/1 max=1000|')"

    # A missing probe field affects both sample checks.
    ctl 'a sat-probe line clipped before its maximum' 64 refuse 2 \
        "$(ctl_rv64 | sed 's|^  sat-probe: .*|  sat-probe: width=64 fits+1/0 wide+0/1|')"
    ctl 'a representable sample counted as saturated' 64 refuse 1 \
        "$(ctl_rv64 | sed 's|fits+1/0|fits+1/1|')"
    ctl 'a wide sample dropped unrecorded' 64 refuse 1 \
        "$(ctl_rv64 | sed 's|wide+0/1|wide+0/0|')"
    # Truncating 0x100002000 to 32 bits produces 8192.
    ctl 'a wide sample that still moved the maximum' 64 refuse 1 \
        "$(ctl_rv64 | sed 's|max=1000|max=8192|')"

    ctl 'a report whose phase table did not arrive' 32 refuse 1 \
        "$(ctl_rv32 | sed '/^    [A-Z][A-Z_]* /d')"
    ctl 'a row reporting a wrapped delta as a cost' 32 refuse 1 \
        "$(ctl_rv32 | sed 's|^    SWITCH_TO .*|    SWITCH_TO        2672/4294950000  min=2380  n=480039|')"

    ctl 'a report with no ns-probe line' 32 refuse 1 \
        "$(ctl_rv32 | sed '/^  ns-probe: /d')"
    ctl 'a probe that converts where the report names no rate' 64 refuse 1 \
        "$(ctl_fast | sed 's|^cycle counter: .*|cycle counter: 0 Hz (0 = no rate converts a reading; cycles only)|')"
    ctl 'an ns-probe clipped before its value' 64 refuse 1 \
        "$(ctl_fast | sed 's|^  ns-probe: .*|  ns-probe: cyc=4294967295|')"
    # Keep the 2.4 GHz conversion correct and change only the cap count.
    ctl 'a cap counted where the rate makes none' 64 refuse 1 \
        "$(ctl_fast | sed 's|capped=0|capped=1|')"
    # Truncation turns the 42.9-second result into 4294967286 ns.
    ctl 'a conversion that truncates instead of capping' 32 refuse 1 \
        "$(ctl_capped | sed 's|^  ns-probe: .*|  ns-probe: cyc=4294967295 ns=4294967286 capped=0|')"
    ctl 'a conversion printing a figure the rate does not make' 64 refuse 1 \
        "$(ctl_fast | sed 's|ns=1789569706|ns=1789569|')"

    echo "PASS: $_ctl_pass planted report(s) accepted and $_ctl_refuse refused, each naming its
  own arm"
    exit 0
fi

echo "1..6"

run_image "$elf"
OUT_ALL="$OUT"

rc=0
read_report "$OUT_ALL"
sat_arms

if [ "$rc" -ne 0 ]; then
    printf '%s\n' "$OUT_ALL" | grep -E 'sat-probe|ns-probe|SAT=' | sed 's/^/# /'
fi

# Reject faults after a completed report too. Interleaved SMP output may hide
# a fault marker, so this is not a complete fault check.
assert_no_panic "the bench image panicked while the sweep ran"
exit "$rc"
