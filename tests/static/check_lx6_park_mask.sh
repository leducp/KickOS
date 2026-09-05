#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The interrupt posture the LX6 secondary park holds across the test that decides whether to
# sleep, read out of the LINKED IMAGE. REFUSED: an RSIL to level 0 anywhere ahead of the first
# WAITI, a WAITI with nothing having raised the level, and a body carrying no WAITI. WAITI takes
# PS.INTLEVEL from its own immediate, so `waiti 0` is the unmask and the sleep together.
#
# This reader builds no control-flow graph: what it proves is that the body contains no unmask
# ahead of its sleep and does contain a raise ahead of it, not that the raise dominates the
# sleep on every path. The raise may be an inlined RSIL to a nonzero level or a call to
# arch_irq_save, and both count.
#
# AN EMPTY CORPUS IS A FAILURE, not a pass.
#
# usage: check_lx6_park_mask.sh <elf> <nm> <objdump>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

# The host binutils is localised and prints translated headers, which every parse below reads.
LC_ALL=C
export LC_ALL

_usage="usage: check_lx6_park_mask.sh <elf> <nm> <objdump>"
elf="${1:?$_usage}"
nm="${2:?$_usage}"
objdump="${3:?$_usage}"

SYM=kickos_lx6_doorbell_park
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
    sub(/^[0-9a-f]+[ \t]+/, "", text)
    mnem = text
    sub(/[ \t].*$/, "", mnem)
    ops = text
    sub(/^[^ \t]*[ \t]*/, "", ops)
    n++
    if (mnem == "waiti" && waiti == 0) { waiti = n }
    if (mnem == "rsil") {
        imm = ops
        sub(/^.*,[ \t]*/, "", imm)
        gsub(/[ \t]/, "", imm)
        if (imm == "0") {
            if (open == 0) { open = n }
        }
        else {
            if (mask == 0) { mask = n }
        }
    }
    if (mnem ~ /^call/ && ops ~ /<arch_irq_save>/) {
        if (mask == 0) { mask = n }
    }
}
END {
    if (!seen) { print "NOSYM"; exit }
    if (n == 0) { print "NOINSN"; exit }
    if (waiti == 0) { print "NOWAITI " n; exit }
    if (mask == 0) { print "NOMASK " waiti " " n; exit }
    print "ORDER " mask " " (open + 0) " " waiti " " n
}
AWK

read_body() { # <listing> <symbol>
    awk -v sym="$2" -f "$TMP/reader.awk" "$1"
}

# --- the reader's controls, before the image is read --------------------------
# Planted listings in the shape the invocation below produces, which shows the raw-byte column.
cat > "$TMP/ctl_ok" <<'EOF'
40000000 <planted_park>:
40000000:	004136               	entry	a1, 32
40000003:	084265               	call8	40095cf4 <arch_irq_save>
40000006:	0888                	l32i.n	a8, a8, 0
40000008:	007000               	waiti	0
4000000b:	006080               	rsil	a8, 0
4000000e:	f01d                	retw.n
EOF
cat > "$TMP/ctl_open" <<'EOF'
40000000 <planted_park>:
40000000:	004136               	entry	a1, 32
40000003:	006080               	rsil	a8, 0
40000006:	084265               	call8	40095cf4 <arch_irq_save>
40000009:	007000               	waiti	0
4000000c:	f01d                	retw.n
EOF
cat > "$TMP/ctl_inline" <<'EOF'
40000000 <planted_park>:
40000000:	006380               	rsil	a8, 3
40000003:	007000               	waiti	0
EOF
cat > "$TMP/ctl_nomask" <<'EOF'
40000000 <planted_park>:
40000000:	0888                	l32i.n	a8, a8, 0
40000002:	007000               	waiti	0
EOF
cat > "$TMP/ctl_nowaiti" <<'EOF'
40000000 <planted_park>:
40000000:	084265               	call8	40095cf4 <arch_irq_save>
40000003:	f01d                	retw.n
EOF

ctl="$(read_body "$TMP/ctl_ok" planted_park)"
case "$ctl" in
    "ORDER 2 5 4 6") ;;
    *) fail "the reader answered [$ctl] for a planted body that masks, tests and then sleeps with
  its only unmask after the WAITI, so it cannot recognise the shape this gate requires and every
  verdict below is meaningless" ;;
esac

ctl="$(read_body "$TMP/ctl_open" planted_park)"
case "$ctl" in
    "ORDER 3 2 4 5") ;;
    *) fail "the reader answered [$ctl] for a planted body that lowers PS.INTLEVEL to 0 ahead of
  its WAITI. That is the lost wake this gate exists to catch, so a reader that does not report it
  cannot go red" ;;
esac

ctl="$(read_body "$TMP/ctl_inline" planted_park)"
case "$ctl" in
    "ORDER 1 0 2 2") ;;
    *) fail "the reader answered [$ctl] for a planted body whose mask is an inlined RSIL to a
  nonzero level rather than a call, so a build that inlined arch_irq_save would be reported as
  carrying no mask at all" ;;
esac

ctl="$(read_body "$TMP/ctl_nomask" planted_park)"
case "$ctl" in
    "NOMASK 2 2") ;;
    *) fail "the reader answered [$ctl] for a planted body that reaches a WAITI having raised no
  level at all, so a park whose mask was deleted outright would read as a clean one" ;;
esac

ctl="$(read_body "$TMP/ctl_nowaiti" planted_park)"
case "$ctl" in
    "NOWAITI 2") ;;
    *) fail "the reader answered [$ctl] for a planted body carrying no WAITI, so a restructured
  park would be reported as passing a check it never reached" ;;
esac

ctl="$(read_body "$TMP/ctl_ok" a_symbol_no_listing_carries)"
case "$ctl" in
    NOSYM) ;;
    *) fail "the reader answered [$ctl] for a symbol the listing does not carry, so a renamed
  park would read as a clean one" ;;
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
    fail "no sized defined symbol '$SYM' in $elf. The park was renamed, made static or inlined
  away, so this gate has no body to read and reports an absence it cannot tell apart from a
  failure to read"
fi
case "$size" in
    *[!0]*) ;;
    *) fail "'$SYM' has size 0 in $elf: the symbol survived as a label but its body is gone" ;;
esac

# --- the instruction stream ---------------------------------------------------
tool_out "$TMP/dis" "^[0-9a-f]+ <.*>:\$" "$objdump" -d "$elf"
require_nonempty "$TMP/dis" "$objdump printed no disassembly for $elf"

echo "== the park's interrupt posture across its sleep decision in $elf =="

rec="$(read_body "$TMP/dis" "$SYM")"
kind="$(printf '%s\n' "$rec" | cut -d' ' -f1)"
f2="$(printf '%s\n' "$rec" | cut -d' ' -f2)"
f3="$(printf '%s\n' "$rec" | cut -d' ' -f3)"
f4="$(printf '%s\n' "$rec" | cut -d' ' -f4)"
f5="$(printf '%s\n' "$rec" | cut -d' ' -f5)"
case "$kind" in
    NOSYM)
        fail "'$SYM' is a sized symbol in $elf but the disassembly carries no body for it, so
  the reader started nowhere. The disassembler's output shape has moved" ;;
    NOINSN)
        fail "the body of '$SYM' in $elf disassembles to no instruction at all, so the corpus
  is UNKNOWN rather than empty" ;;
    NOWAITI)
        fail "the body of '$SYM' in $elf carries no WAITI across its $f2 instruction(s). The park
  no longer sleeps, or it was restructured out from under this reader, and either way this gate
  asserts nothing. That is UNKNOWN, not a pass" ;;
    NOMASK)
        fail "the body of '$SYM' in $elf reaches its WAITI at instruction #$f2 having raised
  PS.INTLEVEL nowhere across its $f3 instruction(s). The cells that decide whether to sleep are
  then read with this core's interrupts open, so a raise serviced between the read and the WAITI
  is a wake the core sleeps through, and the core-start raise is a single edge" ;;
    ORDER) ;;
    *)
        fail "the reader emitted [$rec] for '$SYM', a record this gate does not model" ;;
esac

mask="$f2"
open="$f3"
waiti="$f4"
total="$f5"
require_number "$mask" "the first mask's ordinal in $SYM"
require_number "$open" "the first unmask-to-0's ordinal in $SYM"
require_number "$waiti" "the first WAITI's ordinal in $SYM"
require_number "$total" "the instruction count of $SYM"
echo "   corpus: $total instruction(s) in $SYM, first mask at #$mask, first WAITI at #$waiti,
  first RSIL to level 0 at #$open (0 means none)"

if [ "$mask" -ge "$waiti" ]; then
    awk -v sym="$SYM" '
        /^[0-9a-f]+ <.*>:$/ { name = $2; gsub(/[<>:]/, "", name); f = (name == sym); next }
        f { print "      " $0 }
        f && /^$/ { exit }' "$TMP/dis" >&2
    fail "in '$SYM' the first mask is instruction #$mask and the first WAITI is #$waiti, so
  nothing raises PS.INTLEVEL ahead of the sleep decision and the test is made with this core's
  interrupts open"
fi

if [ "$open" -ne 0 ] && [ "$open" -lt "$waiti" ]; then
    awk -v sym="$SYM" '
        /^[0-9a-f]+ <.*>:$/ { name = $2; gsub(/[<>:]/, "", name); f = (name == sym); next }
        f { print "      " $0 }
        f && /^$/ { exit }' "$TMP/dis" >&2
    fail "in '$SYM' an RSIL to level 0 is instruction #$open, ahead of the WAITI at #$waiti. The
  park then tests the cells that decide whether to sleep with its interrupts already open: the
  level-1 dispatch can take and clear the core-start raise between that test and the WAITI, and
  the core sleeps on a wake that is gone. WAITI takes PS.INTLEVEL from its immediate, so the
  unmask belongs in the WAITI and nowhere ahead of it"
fi

echo "PASS: '$SYM' raises PS.INTLEVEL at instruction #$mask and reaches its WAITI at #$waiti
  with no unmask between, so the sleep decision is made under this core's own mask and the
  WAITI's immediate is the only unmask ahead of the sleep"
exit 0
