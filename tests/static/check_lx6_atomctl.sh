#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The LX6's per-core seating of ATOMCTL (Special Register 99) in kickos_lx6_init and the
# read-back beside it, read out of the LINKED IMAGE: the write must be present and the read-back
# must follow it. Table 190 of the ISA summary (p.313) leaves a WSR of bits 7:6 or 31:9
# undefined, so a write the part ignored is what the read-back catches.
#
# The binutils on this bench prints no mnemonic for Special Register 99 in this core
# configuration and emits `excw` for both instructions, so the reader keys on the three-byte
# encoding, whose fifth hex digit is the address register and is therefore wildcarded. A
# disassembler that learns the names is matched too.
#
# AN EMPTY CORPUS IS A FAILURE, not a pass.
#
# usage: check_lx6_atomctl.sh <elf> <nm> <objdump>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

# The host binutils is localised and prints translated headers, which every parse below reads.
LC_ALL=C
export LC_ALL

_usage="usage: check_lx6_atomctl.sh <elf> <nm> <objdump>"
elf="${1:?$_usage}"
nm="${2:?$_usage}"
objdump="${3:?$_usage}"

SYM=kickos_lx6_init
# A defined-symbol count below this says nm was read wrong, whatever it printed.
SYM_FLOOR=100

[ -f "$elf" ] || fail "no image at $elf"
[ -x "$nm" ] || fail "no nm at $nm; the symbol table cannot be read out of the image and every
  assertion below would rest on a hard-coded layout"
[ -x "$objdump" ] || fail "no objdump at $objdump; there is no instruction stream to decode"

scratch_dir

# --- the reader ---------------------------------------------------------------
# The verdict is an order, so records carry ordinals. Emits exactly one record.
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
    raw = text
    sub(/[ \t].*$/, "", raw)
    rest = text
    sub(/^[0-9a-f]+[ \t]+/, "", rest)
    mnem = rest
    sub(/[ \t].*$/, "", mnem)
    n++
    # WSR/RSR of Special Register 99: opcode byte, sr byte 0x63, then the address register in
    # the high nibble of the third byte.
    if ((raw ~ /^1363[0-9a-f]0$/ || mnem == "wsr.atomctl") && wsr == 0) { wsr = n }
    if ((raw ~ /^0363[0-9a-f]0$/ || mnem == "rsr.atomctl") && rsr == 0) { rsr = n }
}
END {
    if (!seen) { print "NOSYM"; exit }
    if (n == 0) { print "NOINSN"; exit }
    if (wsr == 0) { print "NOSEAT " n; exit }
    if (rsr == 0) { print "NOREADBACK " n; exit }
    print "ORDER " wsr " " rsr " " n
}
AWK

read_body() { # <listing> <symbol>
    awk -v sym="$2" -f "$TMP/reader.awk" "$1"
}

# --- the reader's controls, before the image is read --------------------------
# Planted listings in the shape the invocation below produces, which shows the raw-byte column.
cat > "$TMP/ctl_ok" <<'EOF'
40000000 <planted_init>:
40000000:	006136               	entry	a1, 48
40000003:	581c                	movi.n	a8, 21
40000005:	136380               	excw
40000008:	002010               	rsync
4000000b:	036360               	excw
4000000e:	f01d                	retw.n
EOF
cat > "$TMP/ctl_rev" <<'EOF'
40000000 <planted_init>:
40000000:	036360               	excw
40000003:	136380               	excw
40000006:	f01d                	retw.n
EOF
cat > "$TMP/ctl_noseat" <<'EOF'
40000000 <planted_init>:
40000000:	036360               	excw
40000003:	f01d                	retw.n
EOF
cat > "$TMP/ctl_nocheck" <<'EOF'
40000000 <planted_init>:
40000000:	136380               	excw
40000003:	f01d                	retw.n
EOF

ctl="$(read_body "$TMP/ctl_ok" planted_init)"
case "$ctl" in
    "ORDER 3 5 6") ;;
    *) fail "the reader answered [$ctl] for a planted body that seats Special Register 99 and
  reads it back after, so it cannot recognise the shape this gate requires and every verdict
  below is meaningless" ;;
esac

ctl="$(read_body "$TMP/ctl_rev" planted_init)"
case "$ctl" in
    "ORDER 2 1 3") ;;
    *) fail "the reader answered [$ctl] for a planted body whose read-back comes BEFORE its
  write. That reads the value the boot path left rather than the value this image seated, so a
  reader that does not report it cannot go red" ;;
esac

ctl="$(read_body "$TMP/ctl_noseat" planted_init)"
case "$ctl" in
    "NOSEAT 2") ;;
    *) fail "the reader answered [$ctl] for a planted body that never writes Special Register
  99, which is the defect this gate exists to catch" ;;
esac

ctl="$(read_body "$TMP/ctl_nocheck" planted_init)"
case "$ctl" in
    "NOREADBACK 2") ;;
    *) fail "the reader answered [$ctl] for a planted body that seats Special Register 99 and
  never reads it back, so an image trusting a write the part ignored would read as a clean one" ;;
esac

ctl="$(read_body "$TMP/ctl_ok" a_symbol_no_listing_carries)"
case "$ctl" in
    NOSYM) ;;
    *) fail "the reader answered [$ctl] for a symbol the listing does not carry, so a renamed
  init body would read as a clean one" ;;
esac

# --- the symbol table, and the body in it -------------------------------------
tool_out "$TMP/nm" "[0-9a-fA-F]" "$nm" -S --defined-only "$elf"
require_nonempty "$TMP/nm" "$nm printed no symbol at all for $elf, so the corpus is UNKNOWN
  rather than empty and every verdict below it would be vacuous"
syms="$(wc -l < "$TMP/nm" | tr -d ' ')"
require_number "$syms" "the defined-symbol count"
if [ "$syms" -lt "$SYM_FLOOR" ]; then
    fail "$nm reports $syms defined symbol(s) in $elf, below the floor of $SYM_FLOOR. A table
  that short is a misread, not a small image, and the corpus is UNKNOWN"
fi

size="$(awk -v s="$SYM" 'NF == 4 && $4 == s { print $2; exit }' "$TMP/nm")"
if [ -z "$size" ]; then
    fail "no sized defined symbol '$SYM' in $elf. The per-core init was renamed, made static or
  inlined away, so this gate has no body to read and reports an absence it cannot tell apart
  from a failure to read"
fi
case "$size" in
    *[!0]*) ;;
    *) fail "'$SYM' has size 0 in $elf: the symbol survived as a label but its body is gone" ;;
esac

# --- the instruction stream ---------------------------------------------------
tool_out "$TMP/dis" "^[0-9a-f]+ <.*>:\$" "$objdump" -d "$elf"
require_nonempty "$TMP/dis" "$objdump printed no disassembly for $elf"

echo "== the per-core ATOMCTL seat and its read-back in $elf =="

rec="$(read_body "$TMP/dis" "$SYM")"
kind="$(printf '%s\n' "$rec" | cut -d' ' -f1)"
f2="$(printf '%s\n' "$rec" | cut -d' ' -f2)"
f3="$(printf '%s\n' "$rec" | cut -d' ' -f3)"
f4="$(printf '%s\n' "$rec" | cut -d' ' -f4)"
case "$kind" in
    NOSYM)
        fail "'$SYM' is a sized symbol in $elf but the disassembly carries no body for it, so
  the reader started nowhere. The disassembler's output shape has moved" ;;
    NOINSN)
        fail "the body of '$SYM' in $elf disassembles to no instruction at all, so the corpus
  is UNKNOWN rather than empty" ;;
    NOSEAT)
        fail "the body of '$SYM' in $elf never writes Special Register 99 across its $f2
  instruction(s). Every core then reaches arch_kernel_lock with ATOMCTL at whatever its boot
  path left, and the reset value 0x28 selects the core-local arm for both cacheable classes: the
  kernel lock's S32C1I would exclude each core from itself and the two cores from nothing" ;;
    NOREADBACK)
        fail "the body of '$SYM' in $elf writes Special Register 99 and never reads it back
  across its $f2 instruction(s). Table 190 does not guarantee the fields are writable, so a part
  that ignored the write leaves an image that looks seated and excludes nothing, with no fault
  anywhere. That is UNKNOWN, not a pass" ;;
    ORDER) ;;
    *)
        fail "the reader emitted [$rec] for '$SYM', a record this gate does not model" ;;
esac

wsr="$f2"
rsr="$f3"
total="$f4"
require_number "$wsr" "the ATOMCTL write's ordinal in $SYM"
require_number "$rsr" "the ATOMCTL read-back's ordinal in $SYM"
require_number "$total" "the instruction count of $SYM"
echo "   corpus: $total instruction(s) in $SYM, SR 99 written at #$wsr, read back at #$rsr"

if [ "$rsr" -le "$wsr" ]; then
    awk -v sym="$SYM" '
        /^[0-9a-f]+ <.*>:$/ { name = $2; gsub(/[<>:]/, "", name); f = (name == sym); next }
        f { print "      " $0 }
        f && /^$/ { exit }' "$TMP/dis" >&2
    fail "in '$SYM' Special Register 99 is read at instruction #$rsr and written at #$wsr, so the
  read-back does NOT follow the seat. What that reads is the value the boot path left, which is
  the one thing this check exists not to trust"
fi

echo "PASS: '$SYM' seats Special Register 99 at instruction #$wsr and verifies it at #$rsr,
  so no core reaches arch_kernel_lock on an ATOMCTL this image did not both set and confirm"
exit 0
