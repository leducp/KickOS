#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# No kernel access resolves through gp on a split RISC-V image. __global_pointer$ anchors the
# APP's small-data window there, and gp is an ordinary register an unprivileged thread writes:
# a kernel load or store reached through it lands wherever that thread chose.
#
# gp addressing does not exist in an object file: the linker MAKES it, relaxing an ordinary
# upper/lower pair whose target lands within gp +/- 0x800. An object-level sweep reports zero gp
# references on a tree full of them, so the corpus is the LINKED image.
#
# A hit is a kernel reference to an app-half symbol that the linker could reach through gp. The
# fix is a relocated 64-bit word in kernel data (see kernel/mem/aspace.cc).
#
# The one allowed shape is the anchor pair: kernel text seats gp from a link-time word, spelled
# `auipc gp,0x0` followed by `ld gp,N(gp)`. The exemption is tested on the PRECEDING
# instruction, not on the load's own shape; an `ld gp,N(gp)` reached any other way
# dereferences whatever gp holds. `auipc gp,` is permitted at offset 0x0 and nowhere else, and
# `li gp,` and `mv gp,` seat the register; any other instruction naming gp is refused.
#
# gp is matched as a REGISTER TOKEN in the operands, with objdump's trailing `#` annotation
# removed first, so a symbol whose name merely contains those two letters is not a hit.
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

# One image's instruction stream, classified IN ORDER. Prints the paired-anchor count; appends
# every refused line to <bad>, which the caller truncates first.
classify() { # <insns> <bad>
    awk -v bad="$2" '
        {
            line = $0
            sub(/[ \t]*#.*$/, "", line)
            sub(/[ \t]+$/, "", line)
            mnem = line
            sub(/[ \t].*$/, "", mnem)
            ops = line
            sub(/^[^ \t]*[ \t]*/, "", ops)
            anchor = (mnem == "auipc" && ops == "gp,0x0")
            ok = 0
            if (anchor) {
                ok = 1
            } else if ((mnem == "li" || mnem == "mv") && ops ~ /^gp,/) {
                ok = 1
            } else if (mnem == "ld" && ops ~ /^gp,-?[0-9]*\(gp\)$/ && prev_anchor) {
                ok = 1
                anchors++
            }
            if (! ok && ops ~ /(^|[^0-9A-Za-z_])gp([^0-9A-Za-z_]|$)/) {
                print NR ":" line >> bad
            }
            prev_anchor = anchor
        }
        END { print anchors + 0 }
    ' "$1"
}

# --- the detector, before it is asked to report an absence --------------------
# An absence-assertion whose detector has never fired is not evidence, and the pairing leg is
# the one no shape test can reach: the refused load and the permitted one are the SAME
# instruction and differ only in what precedes them.
cat > "$TMP/ctl.insns" <<'KOS_CTL_INSNS_END'
auipc gp,0x0
ld gp,196(gp)
li gp,64
mv gp,a0
ld a0,8(sp)
ret
jal ra,ffffffff80001234 <kos_ctl_gp_helper>
ld gp,8(gp)
auipc gp,0x1000
ld a0,8(gp)
sd a0,16(gp)
addi a0,gp,8
KOS_CTL_INSNS_END
# objdump prints an operand-less instruction with a trailing separator, so line 13 carries one.
# It cannot live in the heredoc above: tests/static/check_whitespace.sh reads this file.
printf 'ret \n' >> "$TMP/ctl.insns"
: > "$TMP/ctl.bad"
CTL_A="$(classify "$TMP/ctl.insns" "$TMP/ctl.bad")"
[ "$CTL_A" -eq 1 ] \
    || fail "the classifier counted $CTL_A paired anchor(s) in a control stream carrying one,
      so the anchor floor below counts something other than the shape it names"
for want in 8 9 10 11 12; do
    grep -q "^$want:" "$TMP/ctl.bad" \
        || fail "the classifier does not report control line $want, so it would read that shape
      as permitted everywhere in the corpus below"
done
for quiet in 1 2 3 4 5 6 7 13; do
    if grep -q "^$quiet:" "$TMP/ctl.bad"; then
        fail "the classifier reports control line $quiet, which is a permitted shape or names
      no gp at all, so it would refuse every image in the corpus below"
    fi
done
CTL_N="$(wc -l < "$TMP/ctl.bad" | tr -d ' ')"
[ "$CTL_N" -eq 5 ] || fail "the classifier reported $CTL_N control line(s), expected 5"

echo "control: 1 paired anchor, 5 refused shape(s) of 13 planted instruction(s)"

TOTAL_IMAGES=0
TOTAL_INSNS=0
TOTAL_ANCHORS=0
: > "$TMP/hits"

for IMG in "$@"; do
    [ -f "$IMG" ] || fail "image not found: $IMG"
    "$OBJDUMP" -d --section="$SECTION" "$IMG" > "$TMP/dis" 2>/dev/null \
        || fail "objdump refused $IMG, so its verdict is UNKNOWN rather than clean"

    # Instruction lines only, and two shapes matter: objdump separates address, bytes,
    # MNEMONIC and OPERANDS with TABS, so keying on field 3 alone drops every operand and the
    # scan below matches nothing; and it PADS the address column with LEADING SPACES for an
    # address narrower than the widest in the section, so an anchor of `^[0-9a-f]` reads zero
    # instructions out of a low-linked half.
    awk -F"$TAB" '$1 ~ /^[[:space:]]*[0-9a-f]+:$/ { print $3 " " $4 }' "$TMP/dis" > "$TMP/insns"
    N="$(wc -l < "$TMP/insns" | tr -d ' ')"
    [ "$N" -ge "$MIN_INSNS" ] \
        || fail "$IMG: section $SECTION disassembled to $N instruction(s), floor $MIN_INSNS; the corpus is wrong, not clean"

    # The allowed anchor PAIR, counted so a build that stopped emitting it cannot make this
    # gate quiet by removing the only thing it recognises.
    : > "$TMP/bad"
    A="$(classify "$TMP/insns" "$TMP/bad")"
    [ "$A" -ge "$MIN_ANCHORS" ] \
        || fail "$IMG: $A paired gp anchor load(s), floor $MIN_ANCHORS; kernel text no longer seats gp the way this gate recognises"

    if [ -s "$TMP/bad" ]; then
        while IFS= read -r L; do
            printf '%s: %s\n' "$IMG" "$L" >> "$TMP/hits"
        done < "$TMP/bad"
    fi

    TOTAL_IMAGES=$((TOTAL_IMAGES + 1))
    TOTAL_INSNS=$((TOTAL_INSNS + N))
    TOTAL_ANCHORS=$((TOTAL_ANCHORS + A))
done

echo "corpus: $TOTAL_IMAGES image(s), $TOTAL_INSNS instruction(s) in $SECTION, $TOTAL_ANCHORS paired gp anchor load(s)"

if [ -s "$TMP/hits" ]; then
    echo "FAIL: kernel text reaches memory through gp, which an unprivileged thread writes." >&2
    echo "      Each hit is a kernel reference to an app-half symbol the linker relaxed onto" >&2
    echo "      the app's own anchor. Make it a relocated word in kernel data instead." >&2
    sed 's/^/      /' "$TMP/hits" >&2
    exit 1
fi

echo "PASS: no kernel access resolves through gp"
