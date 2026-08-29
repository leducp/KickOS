#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# NO KERNEL ACCESS RESOLVES THROUGH gp ON A SPLIT RISC-V IMAGE (docs/design-m6-mmu.md R2.2).
# __global_pointer$ anchors the APP's small-data window there, and gp is an ordinary register
# an unprivileged thread writes: a kernel load or store reached through it lands wherever that
# thread chose. R1.6 closed the same hole from the other side by re-anchoring gp at the trap
# entry; the split closes it by construction, and this is what proves the construction held.
#
# THE ARCHIVES CANNOT ANSWER THIS AND THAT IS THE WHOLE POINT. gp addressing does not exist in
# an object file: the linker MAKES it, relaxing an ordinary upper/lower pair whose target lands
# within gp +/- 0x800. An object-level sweep reports zero gp references on a tree that has
# twenty-two of them, which is exactly what R1.6 measured. So the corpus is the LINKED image.
#
# WHAT A HIT MEANS is a kernel reference to an app-half symbol that the linker could reach
# through gp instead of refusing. It is the SILENT form of the cross-half reference: the loud
# form is an auipc truncation and fails the link, and this is the one that does not. The fix is
# the same either way, a relocated 64-bit word in kernel data (see kernel/mem/aspace.cc).
#
# THE ANCHOR LOADS ARE THE ONE ALLOWED SHAPE. Kernel text seats gp from a link-time word, and
# the assembler spells that `auipc gp,0x0` followed by `ld gp,N(gp)`: the second instruction
# does read gp, but the value it reads is the auipc's own PC-relative result and not anything a
# thread wrote. The two writes that SEAT the register, `li gp,` and `mv gp,`, are permitted with
# it; any other instruction naming gp is refused.
#
# usage: check_riscv_kernel_gp.sh <objdump> <section> <image>...
#   section  the output section holding KERNEL text (`.text` on this chip; the app's is
#            .apptext and is not scanned, gp being the app's own)

set -eu
. "$(dirname "$0")/../lib/gate.sh"

export LC_ALL=C

if [ "$#" -lt 3 ]; then
    echo "usage: $0 <objdump> <section> <image>..." >&2
    exit 2
fi

OBJDUMP="$1"; shift
SECTION="$1"; shift

command -v "$OBJDUMP" >/dev/null 2>&1 || fail "objdump not found: $OBJDUMP"
[ "$#" -gt 0 ] || fail "no image given (the guard would pass vacuously)"

scratch_dir

# Floors, applied per image. A disassembly that read the wrong section, or none, produces no
# instruction and no anchor, and comparing an empty set against a ban list passes clean.
MIN_INSNS=200
MIN_ANCHORS=2

TOTAL_IMAGES=0
TOTAL_INSNS=0
TOTAL_ANCHORS=0
: > "$TMP/hits"

for IMG in "$@"; do
    [ -f "$IMG" ] || fail "image not found: $IMG"
    "$OBJDUMP" -d --section="$SECTION" "$IMG" > "$TMP/dis" 2>/dev/null \
        || fail "objdump refused $IMG, so its verdict is UNKNOWN rather than clean"

    # Instruction lines only. Two shapes to get right and each was measured wrong first:
    # objdump separates address, bytes, MNEMONIC and OPERANDS with TABS, so keying on field 3
    # alone drops every operand and the scan below then matches nothing; and it PADS the
    # address column with LEADING SPACES for an address narrower than the widest in the
    # section, so an anchor of `^[0-9a-f]` reads zero instructions out of a low-linked half.
    # The floor below is what turned both into a failure instead of a clean verdict.
    awk -F"$TAB" '$1 ~ /^[[:space:]]*[0-9a-f]+:$/ { print $3 " " $4 }' "$TMP/dis" > "$TMP/insns"
    N="$(wc -l < "$TMP/insns" | tr -d ' ')"
    [ "$N" -ge "$MIN_INSNS" ] \
        || fail "$IMG: section $SECTION disassembled to $N instruction(s), floor $MIN_INSNS; the corpus is wrong, not clean"

    # The allowed anchor pair, counted so a build that stopped emitting it cannot make this
    # gate quiet by removing the only thing it recognises.
    A="$(grep -c '^ld[[:space:]]\+gp,[-0-9]*(gp)' "$TMP/insns" || true)"
    [ "$A" -ge "$MIN_ANCHORS" ] \
        || fail "$IMG: $A gp anchor load(s), floor $MIN_ANCHORS; kernel text no longer seats gp the way this gate recognises"

    # Everything naming gp, minus the writes that seat it and minus the anchor load itself.
    grep -n 'gp' "$TMP/insns" \
        | grep -v ':auipc[[:space:]]\+gp,' \
        | grep -v ':ld[[:space:]]\+gp,[-0-9]*(gp)' \
        | grep -v ':li[[:space:]]\+gp,' \
        | grep -v ':mv[[:space:]]\+gp,' \
        > "$TMP/bad" || true
    if [ -s "$TMP/bad" ]; then
        while IFS= read -r L; do
            printf '%s: %s\n' "$IMG" "$L" >> "$TMP/hits"
        done < "$TMP/bad"
    fi

    TOTAL_IMAGES=$((TOTAL_IMAGES + 1))
    TOTAL_INSNS=$((TOTAL_INSNS + N))
    TOTAL_ANCHORS=$((TOTAL_ANCHORS + A))
done

echo "corpus: $TOTAL_IMAGES image(s), $TOTAL_INSNS instruction(s) in $SECTION, $TOTAL_ANCHORS gp anchor load(s)"

if [ -s "$TMP/hits" ]; then
    echo "FAIL: kernel text reaches memory through gp, which an unprivileged thread writes." >&2
    echo "      Each hit is a kernel reference to an app-half symbol the linker relaxed onto" >&2
    echo "      the app's own anchor. Make it a relocated word in kernel data instead." >&2
    sed 's/^/      /' "$TMP/hits" >&2
    exit 1
fi

echo "PASS: no kernel access resolves through gp"
