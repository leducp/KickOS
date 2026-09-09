#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Security CI gate for RISC-V full-C++ under PMP. The gp small-data window
# (__global_pointer$ +/- 0x800) is anchored INSIDE the .appdata grant, so it is
# reachable by an unprivileged thread. That is only safe if the window holds NOTHING
# kernel-owned: the KickOS libs are built -msmall-data-limit=0 so they emit no
# .sdata/.sbss and their globals land in the kernel-side .data/.bss instead. One stray
# KickOS global left in .sdata/.sbss would sit in the app-granted window: a
# privilege-escalation vector (an unprivileged thread could overwrite g_arch_current /
# g_clint_msip). The leak is reported per-archive.
#
# Section-level via objdump -h: with -msmall-data-limit=0 no KickOS object may carry a
# .sdata/.sdata2/.sbss section. The section table is the signal, since this toolchain's nm
# reports .sdata symbols with the same 'D'/'B' type as .data/.bss and a symbol-type check
# would pass vacuously.
#
# NECESSARY AND NOT SUFFICIENT. An archive-level scan cannot see gp-relative addressing at
# all: the linker MAKES it out of an ordinary upper/lower pair at LINK time, so every archive
# reads zero gp-relative references while the linked image holds them.
# check_riscv_kernel_gp.sh is what reads the image, and is registered beside this.
#
# usage: check_riscv_no_smalldata.sh <objdump> <kernel.a> <arch.a> <chip.a> <lib.a>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

OBJDUMP="$1"
shift
# remaining args ($@) are the KickOS-owned archives

command -v "$OBJDUMP" >/dev/null 2>&1 || fail "objdump not found: $OBJDUMP"
[ "$#" -gt 0 ] || fail "no archives given (guard would pass vacuously)"

scratch_dir

# The two section-name patterns, and the landmark that says a section table was read at all,
# handed to the readers below so the controls and the real scan read the same rule.
#
# The two patterns reach awk through -v, so their literal dot is a BRACKET EXPRESSION: awk
# processes escape sequences in such an assignment and the awks disagree about an unknown one,
# gawk dropping the backslash so that `\.` matches any character. TEXT_MARK goes to grep
# instead and spells the same dot as an escape; moving it into an awk -v would need
# rebracketing first.
SDATA_ERE='^[.]sdata'
SBSS_ERE='^[.]sbss'
TEXT_MARK='[[:space:]]\.text'

# objdump -h prints a section table per archive member, with the section name in field 2 on
# the numbered rows. The patterns are anchored on the whole field, so a name that merely
# HOLDS one of them (.rela.sdata) is not a small-data section of its own.
section_hits() { # <section-table> <sdata-ere> <sbss-ere>
    awk -v sd="$2" -v sb="$3" '$2 ~ sd || $2 ~ sb { print $2 }' "$1" | sort -u
}

verdict() { # <leaks-text> <archive-count>
    if [ -n "$1" ]; then
        echo "FAIL: KickOS RISC-V archive(s) emit gp small-data (.sdata/.sbss):" >&2
        echo "      these land in the app-granted gp window (privilege-escalation vector)." >&2
        echo "      Ensure every KickOS RISC-V lib is built -msmall-data-limit=0.$1" >&2
        exit 1
    fi
    echo "PASS: $2 KickOS RISC-V archive(s) carry zero .sdata/.sbss small-data"
}

# --- self-test: one control per clause, each a minimal pair -------------------
# THE SECTION TABLES ARE FORGED objdump OUTPUT: there is no compiler here to emit a real
# .sdata section. So what is proven is the READER and the VERDICT, never the invocation: that
# the table read is `objdump -h` of the archives named on the command line, and that those
# archives are the KickOS-owned ones, are outside these controls. The tool's own death is
# controlled against the REAL objdump, which needs no compiler.
ctldir="$TMP/ctl"
mkdir -p "$ctldir"

row() { # <idx> <name> <flags>
    printf '%3d %-13s 00000010  0000000000000000  0000000000000000  00000040  2**0\n' "$1" "$2"
    printf '                  %s\n' "$3"
}
banner() { # <member>
    printf '\n%s:     file format elf64-littleriscv\n\nSections:\n' "$1"
    printf 'Idx Name          Size      VMA               LMA               File off  Algn\n'
}

# FIVE leaking spellings, and the eight names that must not be read as one, in ONE forged
# table so the assertion is an exact COUNT rather than a boolean. .rela.sdata and
# .debug_sdata are the near misses of the anchor; .tbss and .tdata are thread-local and are
# not the gp window; .data and .bss are where a -msmall-data-limit=0 build puts these
# globals instead, so they are the shape a correct build has.
LEAKS='.sdata .sdata2 .sbss .sdata.g_kickos .sbss.g_kickos'
CLEAN='.text .data .bss .rodata .tbss .tdata .rela.sdata .debug_sdata'
{
    printf 'In archive libkickos_kernel.a:\n'
    banner 'aspace.cc.obj'
    IDX=0
    for S in $LEAKS $CLEAN; do
        row "$IDX" "$S" 'CONTENTS, ALLOC, LOAD, DATA'
        IDX=$((IDX + 1))
    done
} > "$ctldir/pos.h"

section_hits "$ctldir/pos.h" "$SDATA_ERE" "$SBSS_ERE" > "$ctldir/pos.hits"
HITS="$(wc -l < "$ctldir/pos.hits" | tr -d ' ')"
[ "$HITS" -eq 5 ] || {
    cat "$ctldir/pos.hits" >&2
    fail "the reader found $HITS of 5 planted small-data section(s), so it would miss a real
      one, or it reads a name that is not one"
}
for S in $LEAKS; do
    grep -qxF "$S" "$ctldir/pos.hits" \
        || fail "the reader did not report the planted section $S"
done

# EACH clean name on its own, so a name silent for the WRONG reason is visible: a
# whole-table count cannot tell "the anchor holds" from "the reader stopped matching".
QUIET=0
for S in $CLEAN; do
    {
        printf 'In archive libkickos_kernel.a:\n'
        banner 'one.cc.obj'
        row 0 '.text' 'CONTENTS, ALLOC, LOAD, READONLY, CODE'
        row 1 "$S" 'CONTENTS, ALLOC, LOAD, DATA'
    } > "$ctldir/one.h"
    _h="$(section_hits "$ctldir/one.h" "$SDATA_ERE" "$SBSS_ERE" | wc -l | tr -d ' ')"
    [ "$_h" -eq 0 ] || fail "clean control $S reports as gp small-data; the patterns reach a
      section that is not in the gp window"
    QUIET=$((QUIET + 1))
done
[ "$QUIET" -eq 8 ] || fail "$QUIET of 8 clean controls ran silent"

# The mutation the controls exist to survive: change one pattern and the count over the
# forged table must MOVE by an exact amount, so each planted name is a near miss of the
# pattern and not merely present.
mutate() { # <what> <sdata-ere> <sbss-ere> <expect-count>
    _m="$(section_hits "$ctldir/pos.h" "$2" "$3" | wc -l | tr -d ' ')"
    [ "$_m" -eq "$4" ] || {
        section_hits "$ctldir/pos.h" "$2" "$3" >&2
        fail "with the $1, the reader finds $_m section(s), expected $4; the control for it is
      not a near miss and proves nothing"
    }
}
mutate "patterns unchanged"     "$SDATA_ERE"  "$SBSS_ERE"  5
# Unanchored, so a name that merely holds the spelling reports too.
mutate "anchor dropped"         'sdata'       'sbss'       7
# The confusion the anchor is against: the kernel-side sections a correct build uses, which
# hold none of the five leaks.
mutate "kernel-side sections"   '^[.]data'    '^[.]bss'    2
mutate "patterns withdrawn"     'KICKOS_NONE' 'KICKOS_NIL' 0

# The verdict, on a leak and on none. The archive that leaks has to be NAMED, or the fix has
# no target.
if ( verdict "
  libkickos_kernel.a: .sdata .sbss " 4 ) > "$ctldir/out" 2>&1; then
    cat "$ctldir/out"
    fail "the verdict passed over a reported leak, so the gate cannot fail at all"
fi
grep -q 'emit gp small-data' "$ctldir/out" \
    || {
        cat "$ctldir/out" >&2
        fail "the verdict reddened for the wrong reason"
    }
grep -q 'libkickos_kernel.a: .sdata .sbss' "$ctldir/out" \
    || {
        cat "$ctldir/out" >&2
        fail "the verdict does not name the archive that leaks, so a red run has no target"
    }

# The tool's own death, which is the case a forged table cannot reach: objdump exits nonzero
# on a file that is not an object, and an empty result leaves every absence-assertion above
# vacuously satisfied. This arm runs the REAL objdump.
printf 'this file is named like an archive and is not one\n' > "$ctldir/notreally.a"
if ( tool_out "$ctldir/dead" "$TEXT_MARK" "$OBJDUMP" -h "$ctldir/notreally.a" ) \
        > "$ctldir/out" 2>&1; then
    fail "objdump read a text file named *.a without complaint, so a run that decoded
      nothing would report every archive clean"
fi
grep -qE 'exit [0-9]+ from:|nothing matching' "$ctldir/out" \
    || {
        cat "$ctldir/out" >&2
        fail "a dead objdump reddened the gate without naming the tool failure, so the run
      reports a clean archive instead of an unread one"
    }
# And the landmark beside it, for the case objdump exits 0 having listed no section at all:
# an archive with no members prints its banner and stops.
printf 'In archive libkickos_kernel.a:\n' > "$ctldir/nomembers.h"
if grep -qE "$TEXT_MARK" "$ctldir/nomembers.h"; then
    fail "the section landmark matches an archive listing with no section table, so a run
      that read no member would pass"
fi
grep -qE "$TEXT_MARK" "$ctldir/pos.h" \
    || fail "the section landmark is absent from a forged table that holds .text, so it
      would refuse every healthy run"

# --- the archives ---------------------------------------------------------------
leaks=""
N=$#
for A in "$@"; do
  [ -f "$A" ] || fail "archive not found: $A"
  # Positive control: objdump reads a text file named *.a with a diagnostic and no section
  # table at all, which the absence-assertion below would call clean. Every KickOS archive
  # carries code, so a run that saw no .text saw nothing.
  tool_out "$TMP/sect" "$TEXT_MARK" "$OBJDUMP" -h "$A"
  hits="$(section_hits "$TMP/sect" "$SDATA_ERE" "$SBSS_ERE")"
  if [ -n "$hits" ]; then
    leaks="$leaks
  $(basename "$A"): $(echo "$hits" | tr '\n' ' ')"
  fi
done

verdict "$leaks" "$N"
