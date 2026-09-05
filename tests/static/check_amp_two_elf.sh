#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Asserts that the two images of an AMP partition agree, which no single link can see. Each
# node's image is linked on its own, from its own configure, and every link succeeds whatever
# the other one did, so two images built from different partition descriptions BUILD CLEANLY
# AND BOOT. What they disagree about is where the region every node writes sits and how wide it
# is, and that presents as a HANG (docs/design-multicore.md N6b): each side bumps and reads
# cells the other never sees, and nothing in either build can report it.
#
# usage: check_amp_two_elf.sh <cmake> <node0-build-dir> <readelf> <node0.elf> <node1-build-dir>

set -eu
here="$(dirname "$0")"
. "$here/../lib/gate.sh"

CMAKE="${1:?usage: check_amp_two_elf.sh <cmake> <build> <readelf> <node0.elf> <node1-build>}"
BUILD="${2:?}"
READELF="${3:?}"
ELF0="${4:?}"
NODE1_BUILD="${5:?}"

# The artefact target is what BUILDS node 1, so the gate asks for it.
"$CMAKE" --build "$BUILD" --target amp_partition >/dev/null 2>&1 \
    || fail "could not build the partition, so there is no second image to compare"

ELF1="$(find "$NODE1_BUILD" -type f -name 'ampping_n1' -perm -u+x 2>/dev/null | head -1)"
[ -n "$ELF1" ] || fail "no node 1 image under $NODE1_BUILD"
[ -f "$ELF0" ] || fail "no node 0 image at $ELF0"

echo "== node 0: $ELF0"
echo "== node 1: $ELF1"

# EVERY readelf RUNS HERE, at the top level, and nowhere inside the substitutions below.
# tool_out ends the gate on a tool that failed or printed no table, which is the ONE way a
# broken readelf is told apart from an image that lacks what is being read; called from inside
# a `$( )` its exit would end the subshell and leave that same absence to be reported of the
# image. The landmark is the table's own banner, so a readelf that merely emitted a version
# line is caught with it.
scratch_dir
tool_out "$TMP/sect0" 'Section Headers:' env LC_ALL=C "$READELF" -SW "$ELF0"
tool_out "$TMP/sect1" 'Section Headers:' env LC_ALL=C "$READELF" -SW "$ELF1"
tool_out "$TMP/phdr0" 'Program Headers:' env LC_ALL=C "$READELF" -lW "$ELF0"
tool_out "$TMP/phdr1" 'Program Headers:' env LC_ALL=C "$READELF" -lW "$ELF1"

# addr size type, for one section, out of the section header table.
#
# THE BRACKETED INDEX IS STRIPPED BEFORE THE SPLIT. readelf pads it to at least two columns,
# so section 3 arrives as `[ 3]` and splits into TWO fields where `[12]` splits into one. Read
# as a field, every column past it shifts by one and the name matches nothing, which a gate
# reading the result reports as the SECTION BEING ABSENT.
sect() { # <section-header dump> <name>
    awk -v n="$2" '
        {
            row = $0
            sub(/^ *\[ *[0-9]*\] */, "", row)
            if (split(row, f) >= 5 && f[1] == n) { print f[3], f[5], f[2]; exit }
        }' < "$1"
}

W0="$(sect "$TMP/sect0" .amp_shared)"
W1="$(sect "$TMP/sect1" .amp_shared)"
[ -n "$W0" ] || fail "node 0 has no .amp_shared section"
[ -n "$W1" ] || fail "node 1 has no .amp_shared section"

A0="$(echo "$W0" | cut -d' ' -f1)"; S0="$(echo "$W0" | cut -d' ' -f2)"; T0="$(echo "$W0" | cut -d' ' -f3)"
A1="$(echo "$W1" | cut -d' ' -f1)"; S1="$(echo "$W1" | cut -d' ' -f2)"; T1="$(echo "$W1" | cut -d' ' -f3)"

echo "== .amp_shared: node 0 at 0x$A0 size 0x$S0 ($T0); node 1 at 0x$A1 size 0x$S1 ($T1)"

[ "$A0" = "$A1" ] || fail "the two images place .amp_shared at different addresses
  node 0: 0x$A0
  node 1: 0x$A1
  Each node derives it from the partition's own three constants, so a difference means the
  two were configured from different partition descriptions. They would build, boot, and
  never see one another's writes."
[ "$S0" = "$S1" ] || fail "the two images size .amp_shared differently: 0x$S0 against 0x$S1.
  The window is the partition's, so one node believing it wider indexes past what the other
  reserved."
[ "$T0" = NOBITS ] || fail "node 0 LOADS .amp_shared ($T0). The region every node writes must
  carry no initialised bytes: loading it lets whichever node boots later overwrite what the
  first published."
[ "$T1" = NOBITS ] || fail "node 1 LOADS .amp_shared ($T1). See the node 0 clause above."

# The loaded span of an image: lowest and highest physical byte any PT_LOAD with contents
# covers. A NOBITS segment carries no bytes, so it is skipped.
#
# POSIX AWK ONLY, AND THE ARITHMETIC IS THE SHELL'S. awk cannot read a hex field as a number
# without a gawk extension, and a non-gawk awk REFUSES the program before a single record,
# leaving every comparison below reading an empty string: `[ "" -lt 0 ]` is an error in test(1)
# rather than a false condition. tests/static/check_awk_portable.sh holds the rule.
#
# $(( )) is signed 64-bit, exact for a p_paddr on every part in this tree and NOT exact for a
# 64-bit virtual address; field 4 is PhysAddr, and a row at or above 2^63 is refused by name
# rather than wrapping negative.
span() { # <program-header dump> <image label> -> "lo hi", both decimal
    _sp_rows="$(awk '$1 == "LOAD" && $5 !~ /^(0[xX])?0*$/ { print $4, $5 }' < "$1")"
    [ -n "$_sp_rows" ] || fail "no PT_LOAD carrying contents in $2: either the image has none,
  or awk refused the program that reads them, which reads exactly the same from here"
    _sp_min=""
    _sp_max=0
    # A redirected read, never a pipe: a `while` on the right of a pipe runs in a subshell in
    # POSIX sh and the accumulation below would be discarded with it.
    printf '%s\n' "$_sp_rows" > "$TMP/rows"
    while read -r _sp_a _sp_n; do
        case "$_sp_a" in
            0[xX]*) : ;;
            *) fail "PhysAddr [$_sp_a] in $2 is not hex; the span is UNKNOWN, not zero" ;;
        esac
        _sp_lo=$((_sp_a))
        _sp_hi=$((_sp_a + _sp_n))
        if [ "$_sp_lo" -lt 0 ] || [ "$_sp_hi" -lt "$_sp_lo" ]; then
            fail "PhysAddr $_sp_a plus size $_sp_n in $2 is at or above 2^63, which this
  gate's arithmetic cannot carry. The span is UNKNOWN and not an overlap."
        fi
        if [ -z "$_sp_min" ] || [ "$_sp_lo" -lt "$_sp_min" ]; then
            _sp_min="$_sp_lo"
        fi
        if [ "$_sp_hi" -gt "$_sp_max" ]; then
            _sp_max="$_sp_hi"
        fi
    done < "$TMP/rows"
    printf '%s %s' "$_sp_min" "$_sp_max"
}
L0="$(span "$TMP/phdr0" "$ELF0" | cut -d' ' -f1)"
H0="$(span "$TMP/phdr0" "$ELF0" | cut -d' ' -f2)"
L1="$(span "$TMP/phdr1" "$ELF1" | cut -d' ' -f1)"
H1="$(span "$TMP/phdr1" "$ELF1" | cut -d' ' -f2)"
for _v in "$L0" "$H0" "$L1" "$H1"; do
    require_number "$_v" "a loaded-span bound"
done
printf '== loaded spans: node 0 [%#x, %#x)  node 1 [%#x, %#x)\n' "$L0" "$H0" "$L1" "$H1"

if [ "$L0" -lt "$H1" ] && [ "$L1" -lt "$H0" ]; then
    fail "the two node images OVERLAP in physical memory.
  Each node's image is linked at the partition base plus its own index times the node share,
  so an overlap means one of those three constants differs between the two configures. The
  later image to load would silently replace part of the earlier one."
fi

[ "$L0" != "$L1" ] || fail "both images are linked at the same base 0x$(printf '%x' "$L0"):
  node 1 is a second node ZERO. Every arm that runs on node 0 passes on such a partition,
  which is what makes this the collapse docs/design-multicore.md N6c names."

echo "PASS: 2 image(s) agree on .amp_shared (0x$A0, 0x$S0 bytes, NOBITS in both), hold"
echo "      disjoint spans, and are linked as different nodes"
