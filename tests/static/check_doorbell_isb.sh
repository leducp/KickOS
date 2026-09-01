#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The doorbell service body's instruction barrier, read out of the LINKED IMAGE, and its POSITION
# relative to the answer it publishes.
#
# REFUSED: a service body with no ISB, one whose ISB follows the release store that publishes the
# answer, and a body this reader cannot decode. A64 broadcasts translation and instruction cache
# invalidation but no operation causes a Context synchronization event on another PE (DDI 0487 M.b,
# Glossary, "Context Synchronization event"), and until a PE takes one, instructions it has already
# fetched may be re-executed with no bound (section B2.7.4.2), so each PE executing changed code
# must execute its own ISB (section B2.2.5, step 3). An initiator learns the barrier ran by reading
# the answer sequence, so an ISB after that store lets an initiator return from arch_ipi_wait
# before the peer has synchronized.
#
# The ordering is asserted STRUCTURALLY because QEMU's TCG models no prefetch queue and
# invalidates translated blocks when a flush happens: a runtime arm shaped "the peer stopped
# executing the revoked text" stays green on an image carrying no ISB at all. The architectural
# effect rests on the specification and is recorded as unwitnessed in docs/design-multicore.md
# section 7.
#
# The reader goes through three planted listings before the image is read, the reversed one
# included: a reader that cannot report that case cannot go red.
#
# AN EMPTY CORPUS IS A FAILURE, not a pass. A body with no ISB, or none with a release store, says
# the symbol moved or the disassembly shape changed, and that is UNKNOWN.
#
# usage: check_doorbell_isb.sh <elf> <nm> <objdump>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

# The host binutils is localised and prints translated headers, which every parse below reads.
LC_ALL=C
export LC_ALL

_usage="usage: check_doorbell_isb.sh <elf> <nm> <objdump>"
elf="${1:?$_usage}"
nm="${2:?$_usage}"
objdump="${3:?$_usage}"

# The far side of the doorbell: the one body a peer runs on behalf of an initiator.
SYM=kickos_arm64_doorbell_service
# A defined-symbol count below this says nm was read wrong, whatever it printed.
SYM_FLOOR=100

[ -f "$elf" ] || fail "no image at $elf"
[ -x "$nm" ] || fail "no nm at $nm; the symbol table cannot be read out of the image and every
  assertion below would rest on a hard-coded layout"
[ -x "$objdump" ] || fail "no objdump at $objdump; there is no instruction stream to decode"

scratch_dir

# --- the reader ---------------------------------------------------------------
# Ordinals, not addresses: the verdict is an ORDER. Emits exactly one record.
#
# A release store publishes the answer on this backend (kickos::Atomic with Order::RELEASE lowers
# to STLR), matched by prefix: STLR, STLRB and STLRH all publish.
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
    n++
    if (mnem == "isb" && isb == 0) { isb = n }
    if (mnem ~ /^stlr/ && store == 0) { store = n }
}
END {
    if (!seen) { print "NOSYM"; exit }
    if (n == 0) { print "NOINSN"; exit }
    if (isb == 0) { print "NOISB " n; exit }
    if (store == 0) { print "NOSTORE " n; exit }
    print "ORDER " isb " " store " " n
}
AWK

read_body() { # <listing> <symbol>
    awk -v sym="$2" -f "$TMP/reader.awk" "$1"
}

# --- the reader's controls, before the image is read --------------------------
# Planted listings in the shape the invocation below produces, which is --no-show-raw-insn: a
# control carrying the raw-bytes column would read its first byte group as the mnemonic and prove
# the reader against input the gate never hands it.
cat > "$TMP/ctl_ok" <<'EOF'
0000000000001000 <planted_service>:
    1000:	nop
    1004:	isb
    1008:	stlr	w1, [x2]
    100c:	ret
EOF
cat > "$TMP/ctl_bad" <<'EOF'
0000000000001000 <planted_service>:
    1000:	nop
    1004:	stlr	w1, [x2]
    1008:	isb
    100c:	ret
EOF
cat > "$TMP/ctl_noisb" <<'EOF'
0000000000001000 <planted_service>:
    1000:	stlr	w1, [x2]
    1004:	ret
EOF

ctl="$(read_body "$TMP/ctl_ok" planted_service)"
case "$ctl" in
    "ORDER 2 3 4") ;;
    *) fail "the reader answered [$ctl] for a planted body whose ISB precedes its release store,
  so it cannot recognise the shape this gate requires and every verdict below is meaningless" ;;
esac

ctl="$(read_body "$TMP/ctl_bad" planted_service)"
case "$ctl" in
    "ORDER 3 2 4") ;;
    *) fail "the reader answered [$ctl] for a planted body whose ISB comes AFTER its release
  store. That is the defect this gate exists to catch, so a reader that does not report it
  cannot go red" ;;
esac

ctl="$(read_body "$TMP/ctl_noisb" planted_service)"
case "$ctl" in
    "NOISB 2") ;;
    *) fail "the reader answered [$ctl] for a planted body carrying no ISB at all, so an image
  whose barrier was deleted would not be reported as such" ;;
esac

ctl="$(read_body "$TMP/ctl_ok" a_symbol_no_listing_carries)"
case "$ctl" in
    NOSYM) ;;
    *) fail "the reader answered [$ctl] for a symbol the listing does not carry, so a renamed
  service body would read as a clean one" ;;
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
    fail "no sized defined symbol '$SYM' in $elf. The far side of the doorbell was renamed,
  made static or inlined away, so this gate has no body to read and reports an absence it
  cannot tell apart from a failure to read"
fi
case "$size" in
    *[!0]*) ;;
    *) fail "'$SYM' has size 0 in $elf: the symbol survived as a label but its body is gone" ;;
esac

# --- the instruction stream ---------------------------------------------------
tool_out "$TMP/dis" "^[0-9a-f]+ <.*>:\$" "$objdump" -d --no-show-raw-insn "$elf"
require_nonempty "$TMP/dis" "$objdump printed no disassembly for $elf"

echo "== the doorbell service body's ISB in $elf =="

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
    NOISB)
        fail "the body of '$SYM' in $elf carries NO ISB across its $f2 instruction(s). A peer
  serviced through this body then takes no Context synchronization event, and an initiator that
  removed an executable mapping is told the rendezvous completed when nothing synchronized
  (DDI 0487 M.b section B2.2.5, step 3)" ;;
    NOSTORE)
        fail "the body of '$SYM' in $elf carries no release store across its $f2 instruction(s),
  so the answer an initiator reads is not published from here and this gate has nothing to
  order the ISB against. That is UNKNOWN, not a pass" ;;
    ORDER) ;;
    *)
        fail "the reader emitted [$rec] for '$SYM', a record this gate does not model" ;;
esac

isb="$f2"
store="$f3"
total="$f4"
require_number "$isb" "the ISB's ordinal in $SYM"
require_number "$store" "the first release store's ordinal in $SYM"
require_number "$total" "the instruction count of $SYM"
echo "   corpus: $total instruction(s) in $SYM, ISB at #$isb, first release store at #$store"

if [ "$isb" -ge "$store" ]; then
    awk -v sym="$SYM" '
        /^[0-9a-f]+ <.*>:$/ { name = $2; gsub(/[<>:]/, "", name); f = (name == sym); next }
        f { print "      " $0 }
        f && /^$/ { exit }' "$TMP/dis" >&2
    fail "in '$SYM' the ISB is instruction #$isb and the first release store is #$store, so the
  barrier does NOT precede the answer. An initiator polling that answer can return from its wait
  before this core has taken its Context synchronization event, which is the whole of what the
  rendezvous is for"
fi

echo "PASS: '$SYM' takes its ISB at instruction #$isb, ahead of the release store at #$store
  that publishes the answer an initiator waits on"
exit 0
