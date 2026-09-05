#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# arch_ipi_fence's barrier, read out of the LINKED IMAGE. Nothing follows the plain calls from
# amp::window_init and from the raise path, and QEMU's TCG models no store buffer, so a barrier
# deleted, weakened to one half of an acquire/release pair, or optimised away leaves every arm
# in the tree green (docs/design-multicore.md section 7).
#
# The barrier owed is a FULL one (N6f): the publication and the seat read are a store then a
# load, and so are the peer's seating and its drain. Release and acquire order the OPPOSITE
# direction and leave store-then-load free, which is the one a store buffer reorders. With one
# side unbarriered both may read the older value, and the message stands with no notice and no
# later scan.
#
# REFUSED: a body with no barrier at all, a body carrying a one-directional barrier where the
# full one is owed, and a body this reader cannot decode. AN EMPTY CORPUS IS A FAILURE.
#
# usage: check_ipi_fence.sh <elf> <nm> <objdump> <full-barrier-operand> <refused-operands>
#   <full-barrier-operand>  the operand the full barrier carries here, e.g. `ish` or `rw,rw`
#   <refused-operands>      space-separated one-directional operands, e.g. `ishld ishst`

set -eu
. "$(dirname "$0")/../lib/gate.sh"

# The host binutils is localised and prints translated headers, which every parse below reads.
LC_ALL=C
export LC_ALL

_usage="usage: check_ipi_fence.sh <elf> <nm> <objdump> <full-operand> <refused-operands>"
elf="${1:?$_usage}"
nm="${2:?$_usage}"
objdump="${3:?$_usage}"
full="${4:?$_usage}"
refused="${5:-}"

SYM=arch_ipi_fence
SYM_FLOOR=100

[ -f "$elf" ] || fail "no image at $elf"
[ -x "$nm" ] || fail "no nm at $nm; the symbol table cannot be read out of the image"
[ -x "$objdump" ] || fail "no objdump at $objdump; there is no instruction stream to decode"

scratch_dir

# --- the reader ---------------------------------------------------------------
# One record: the barrier's operand, and how many instructions the body holds. The operand is
# what separates a full barrier from a half, so a mnemonic-only reader would see neither.
cat > "$TMP/reader.awk" <<'AWK'
/^[0-9a-f]+ <.*>:$/ {
    name = $2
    gsub(/[<>:]/, "", name)
    inbody = (name == sym)
    if (inbody) { seen = 1 }
    next
}
!inbody { next }
$0 !~ /^[ \t]*[0-9a-f]+:/ { next }
{
    text = $0
    sub(/^[^:]*:[ \t]*/, "", text)
    sub(/[ \t]*\/\/.*$/, "", text)
    sub(/[ \t]*#.*$/, "", text)
    mnem = text
    sub(/[ \t].*$/, "", mnem)
    ops = text
    sub(/^[^ \t]*[ \t]*/, "", ops)
    gsub(/[ \t]/, "", ops)
    n++
    if ((mnem == "dmb" || mnem == "fence") && bar == "") { bar = ops; if (bar == "") bar = "-" }
}
END {
    if (!seen) { print "NOSYM"; exit }
    if (n == 0) { print "NOINSN"; exit }
    if (bar == "") { print "NOBARRIER " n; exit }
    print "BARRIER " bar " " n
}
AWK

read_body() { # <listing> <symbol>
    awk -v sym="$2" -f "$TMP/reader.awk" "$1"
}

# --- the reader's controls, before the image is read --------------------------
# In the shape the invocation below produces (--no-show-raw-insn): a control carrying the
# raw-bytes column would read its first byte group as the mnemonic.
cat > "$TMP/ctl_full" <<EOF
0000000000001000 <$SYM>:
    1000:	dmb	$full
    1004:	ret
EOF
cat > "$TMP/ctl_gone" <<EOF
0000000000001000 <$SYM>:
    1000:	ret
EOF
cat > "$TMP/ctl_other" <<EOF
0000000000001000 <somebody_else>:
    1000:	dmb	$full
    1004:	ret
EOF

_p="$(read_body "$TMP/ctl_full" "$SYM")"
case "$_p" in
    "BARRIER $full "*) : ;;
    *) fail "the reader did not find a planted full barrier (got '$_p')" ;;
esac
_p="$(read_body "$TMP/ctl_gone" "$SYM")"
case "$_p" in
    NOBARRIER*) : ;;
    *) fail "the reader did not report a planted body whose barrier was DELETED (got '$_p').
  That is the case this gate exists for, so a reader that cannot see it proves nothing." ;;
esac
_p="$(read_body "$TMP/ctl_other" "$SYM")"
[ "$_p" = NOSYM ] || fail "the reader accepted a listing that defines another symbol (got '$_p')"

for _bad in $refused; do
    cat > "$TMP/ctl_half" <<EOF
0000000000001000 <$SYM>:
    1000:	dmb	$_bad
    1004:	ret
EOF
    _p="$(read_body "$TMP/ctl_half" "$SYM")"
    case "$_p" in
        "BARRIER $_bad "*) : ;;
        *) fail "the reader did not read back a planted '$_bad' operand (got '$_p')" ;;
    esac
done
echo "== control: the reader reports a full barrier, a deleted one, a foreign symbol, and"
echo "   each of the one-directional operands this arch refuses"

# --- the image ----------------------------------------------------------------
defined="$("$nm" --defined-only "$elf" | wc -l)"
[ "$defined" -ge "$SYM_FLOOR" ] || fail "nm reported only $defined defined symbol(s) in $elf,
  below the floor of $SYM_FLOOR: the symbol table was not read, whatever it printed"

"$objdump" -d --no-show-raw-insn "$elf" > "$TMP/listing" \
    || fail "objdump could not disassemble $elf"

verdict="$(read_body "$TMP/listing" "$SYM")"
case "$verdict" in
    NOSYM)
        fail "$elf defines no $SYM. It is reached through a plain call that no callgraph gate
  follows, so an image without it is exactly the silent deletion this gate exists to refuse." ;;
    NOINSN)
        fail "$SYM decodes to no instruction in $elf" ;;
    NOBARRIER*)
        fail "$SYM carries NO barrier instruction in $elf ($verdict).
  The store-then-load pairing a node's bring-up rests on is unordered without it, and no run in
  this tree can tell: QEMU models no store buffer, so every arm resting on the pairing stays
  green. See docs/design-multicore.md N6f." ;;
esac

got="$(echo "$verdict" | cut -d' ' -f2)"
count="$(echo "$verdict" | cut -d' ' -f3)"
for _bad in $refused; do
    [ "$got" != "$_bad" ] || fail "$SYM carries a ONE-DIRECTIONAL barrier '$got' in $elf.
  Release and acquire order the opposite direction to the one this pairing needs and leave
  store-then-load free, which is the direction a store buffer reorders."
done
[ "$got" = "$full" ] || fail "$SYM carries barrier operand '$got', and this arch owes '$full'"

echo "PASS: $SYM carries a full '$got' barrier across $count instruction(s) of its body"
