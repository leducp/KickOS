#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The phase table's saturation rule, witnessed at runtime. A span that outruns the row's
# 32-bit statistics must be COUNTED APART and must enter no statistic; a wrapped one reads as
# a cost of about 2^32 cycles and poisons the average with it.
#
# usage: check_bench_saturate.sh <bench image>
#
# Three failures this refuses, each of which reports a complete and plausible-looking table:
#   the delta is narrowed to 32 bits before the subtraction, so a long span wraps;
#   the saturation arm drops the sample and counts nothing, so the row silently loses spans;
#   the sample is counted into the statistics AND into the saturation count.
#
# WHERE ARM 6 HAS TEETH. A 32-bit cycle count can only outrun a 32-bit nanosecond field below
# about 1 GHz. No emulated board here is that slow, so the cap branch is unexercised in CI and
# the arm reads the conversion for exactness alone.
#
# THE NANOSECOND COLUMN HAS THE SAME FAILURE and arm 6 reads its own probe. A cycle figure is
# converted into a uint32_t ns column, and a span of more than 4.295 seconds does not fit one.
# Truncated it prints a plausible SMALL number beside a correct cycle figure, which is the
# saturation defect one layer out; capped it prints the field's maximum and says how many
# columns were capped. Whether the widest 32-bit cycle count converts inside the field is a
# RATE fact, above about 1 GHz it does, so the arm reads the pair for agreement and never
# the value alone.
#
# THE PROBE IS THE DISCRIMINATOR AND THE ROW ARM IS A FLOOR, not the other way round. A row
# only saturates on a board that parks a thread past the counter's period or that closes a
# span on another core, so reading the rows is a witness on one board and vacuous on the rest.
# The probe is neither WHERE THE TICK IS 64 BITS (riscv64, aarch64, x86_64): there it is a
# fixed pair of deltas fed through the shipped accumulator, one the row can hold and one it
# cannot, and arms 3 and 4 read the partition.
#
# BELOW THAT WIDTH THERE IS NO SATURATION RULE TO WITNESS. acc_add's saturation branch is
# itself compiled out at a 32-bit tick (kernel/bench/bench.cc, guarded on
# KICKOS_BENCH_TICK_BITS), because no delta a 32-bit counter forms is too wide for a 32-bit
# row, and the kernel prints a probe line carrying a width and no count fields. So arms 3 and
# 4 SKIP there and what a 32-bit board gets out of the probe is arm 2 alone: the width the
# build declares, checked against what the ELF says the machine is, which is what refuses a
# tick type that disagrees with its counter.

set -u
. "$(dirname "$0")/../lib/gate.sh"

# The report the probe prints with lands after the whole call/reply sweep, which is a quarter
# of a million round trips on four emulated cores. gate.sh's 20 s default expires mid-sweep
# and reads as an image that printed no table at all.
: "${QEMU_TIMEOUT:=240}"

elf="${1:?usage: check_bench_saturate.sh <bench image>}"

echo "1..6"

# What the MACHINE is, independently of anything the image says about itself. readelf's own
# heading is the only reader here, so the C locale gate.sh sets is what keeps `Machine:`
# spelled that way.
MACH="$(readelf -h "$elf" 2>/dev/null | sed -n 's/^ *Machine: *//p' | head -n1)"
CLASS="$(readelf -h "$elf" 2>/dev/null | sed -n 's/^ *Class: *//p' | head -n1)"
if [ -z "$MACH" ]; then
    # A PE32+ image (the x86_64 UEFI handover) is not an ELF and readelf answers nothing.
    # Its machine is known from the file type instead, and nothing else in the tree links a
    # bench image that is neither.
    case "$elf" in
        *.efi) MACH="Advanced Micro Devices X86-64"; CLASS="ELF64" ;;
        *) fail "readelf could not read $elf, so nothing here knows what machine it is for" ;;
    esac
fi

# A 64-bit counter on these three, so a delta wider than the statistics can be formed and the
# saturation rule has to fire. Everywhere else the counter is 32 bits and no delta is.
WANT=32
case "$MACH" in
    *AArch64*|*X86-64*) WANT=64 ;;
    *RISC-V*) if [ "$CLASS" = "ELF64" ]; then WANT=64; fi ;;
    *) ;;
esac

run_image "$elf"
OUT_ALL="$OUT"

rc=0

# NO EARLY EXIT ON A MISSING PROBE: the row arm at the end reads the defect off the table
# itself and is the one arm an image without the instrument can still fail.
PROBE="$(printf '%s\n' "$OUT_ALL" | grep '^  sat-probe: ' | tail -n1)"
if [ -z "$PROBE" ]; then
    echo "not ok 1 - the run printed no sat-probe line, so the accumulator is unwitnessed"
    echo "not ok 2 - no sat-probe line, so nothing states the bench tick's width"
    echo "not ok 3 - no sat-probe line"
    echo "not ok 4 - no sat-probe line"
    rc=1
    GOT=""
else
    echo "ok 1 - the run printed a sat-probe line"
    GOT="$(printf '%s\n' "$PROBE" | sed -n 's/.*width=\([0-9]\{1,\}\).*/\1/p')"
fi

if [ -z "$PROBE" ]; then
    :
elif [ "$GOT" != "$WANT" ]; then
    echo "not ok 2 - the bench tick is $GOT bits on a machine whose counter is $WANT ($MACH):"
    echo "#          a span longer than the narrower width wraps into the row"
    rc=1
else
    echo "ok 2 - the bench tick is $WANT bits, as the machine's counter is ($MACH)"
fi

if [ -z "$PROBE" ]; then
    :
elif [ "$WANT" = "64" ]; then
    FITS="$(printf '%s\n' "$PROBE" | sed -n 's/.*fits+\([0-9]\{1,\}\/[0-9]\{1,\}\).*/\1/p')"
    WIDE="$(printf '%s\n' "$PROBE" | sed -n 's/.*wide+\([0-9]\{1,\}\/[0-9]\{1,\}\).*/\1/p')"
    MAXP="$(printf '%s\n' "$PROBE" | sed -n 's/.*max=\([0-9]\{1,\}\).*/\1/p')"

    # NEVER a bare exit on a field that did not parse: the row arm below names the defect in
    # its own right, and a gate that stops here reports the missing field instead of it.
    if [ -z "$FITS" ] || [ -z "$WIDE" ] || [ -z "$MAXP" ]; then
        echo "not ok 3 - the sat-probe line carries no count fields: $PROBE"
        echo "not ok 4 - the sat-probe line carries no count fields"
        rc=1
    else
        # count/sat, in that order. A representable delta is one sample and no saturation.
        if [ "$FITS" != "1/0" ]; then
            echo "not ok 3 - a representable sample moved the accumulator by $FITS (want 1/0),"
            echo "#          so the statistics and SAT do not partition the samples"
            rc=1
        else
            echo "ok 3 - a representable sample lands in the statistics and not in SAT"
        fi

        # 0/1 and nothing else: 0/0 is a build that drops the sample in silence, 1/1 is one
        # that counts it twice, and the maximum says which statistic the second count moved.
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

# Read off the rows themselves. A wrapped delta is uniform over
# the counter's range, so with any number of them the largest sits just under 2^32.
#
# THE ROW MUST CARRY NO SAT COLUMN FOR THIS TO BE A FINDING. Above
# one kernel core a bracket enclosing an inline swap can close on a core whose cycle counter
# shares no zero with the opening core's; that difference is a representable number anywhere
# in the range, so a maximum near 2^32 is expected there. Such a row always carries SAT too,
# the same population straddling the boundary, and a wrapped row never can: a build that wraps
# has no saturation count to print. So the pair is the discriminator and the maximum alone is
# not.
BAND=4294901760
read -r WRAPPED ROWS <<EOF
$(printf '%s\n' "$OUT_ALL" | awk -v b="$BAND" '
    /^ +[A-Z][A-Z_]+ +[0-9]+\/[0-9]+ +min=/ {
        rows++
        split($2, f, "/")
        if (f[2] + 0 >= b && $0 !~ /SAT=/) { n++ }
    }
    END { printf "%d %d\n", n + 0, rows + 0 }')
EOF
if [ "$ROWS" -eq 0 ]; then
    echo "not ok 5 - the run printed no phase table, so its maxima say nothing"
    rc=1
elif [ "$WRAPPED" -ne 0 ]; then
    echo "not ok 5 - $WRAPPED phase row(s) of $ROWS report a maximum at or above $BAND and no"
    echo "#          saturation count, which is a wrapped counter read as a cost"
    printf '%s\n' "$OUT_ALL" | awk -v b="$BAND" \
        '/^ +[A-Z][A-Z_]+ +[0-9]+\/[0-9]+ +min=/ { split($2, f, "/")
             if (f[2] + 0 >= b && $0 !~ /SAT=/) { print "# " $0 } }'
    rc=1
else
    echo "ok 5 - no phase row of $ROWS carries a wrapped maximum"
fi

# --- the ns column's own cap ------------------------------------------------
# THE EXPECTED VALUE IS DERIVED FROM THE RATE THE REPORT ITSELF NAMES, never compared against a
# threshold: whether the widest 32-bit cycle count converts inside a uint32_t ns field depends
# entirely on the counter's rate. $(( )) is 64-bit here and 2^32 * 1e9 is 4.3e18, inside it.
NSP="$(printf '%s\n' "$OUT_ALL" | grep '^  ns-probe: ' | tail -n1)"
HZ="$(printf '%s\n' "$OUT_ALL" | sed -n 's/^cycle counter: \([0-9]\{1,\}\) Hz.*/\1/p' | head -n1)"
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
else
    NSV="$(printf '%s\n' "$NSP" | sed -n 's/.*ns=\([0-9]\{1,\}\).*/\1/p')"
    NSC="$(printf '%s\n' "$NSP" | sed -n 's/.*capped=\([0-9]\{1,\}\).*/\1/p')"
    if [ -z "$NSV" ] || [ -z "$NSC" ]; then
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
fi

if [ "$rc" -ne 0 ]; then
    printf '%s\n' "$OUT_ALL" | grep -E 'sat-probe|ns-probe|SAT=' | sed 's/^/# /'
fi

# A COMPLETE REPORT IS NOT A SURVIVED RUN. Every arm above reads text the image printed, so an
# image that prints the whole report and then faults answers all six of them. This is weak
# above one core, where an interleaved line can hide the marker from grep, and the positive
# assertion it is owed is arm 1: bench_phase_print ends the table on the sat-probe line, so a
# run that faults anywhere in the report never reaches it.
assert_no_panic "the bench image panicked while the sweep ran"
exit "$rc"
