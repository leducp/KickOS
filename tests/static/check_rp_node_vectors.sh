#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A node core must not reach the kernel through a vector, read out of the LINKED IMAGE. The
# reset table funnels every external line into the arch's default IRQ entry, which reaches
# kernel() through kickos_instance_index(), and with multi-instance off that Kernel is the
# primary's LIVE state rather than an inert provisioned copy. The node table is therefore the
# whole guarantee, and it is a property of BYTES IN THE IMAGE rather than of the source that
# emitted them.
#
# What is asserted:
#   routing    every slot of the node table holds the park handler, except the doorbell line,
#              which holds the service body. One wrong word is one line that reaches the
#              kernel, so the count is exact and there is no tolerance.
#   bounded    the park body branches nowhere but into itself and calls nothing.
#   alignment  the table sits where VTOR can point at it: its own byte length rounded up to a
#              power of two, at least 128.
#
# The slot count and the doorbell line are read out of the chip's chip_limits.h rather than
# restated here, so a renumbered part breaks the assertion instead of drifting past it. A chip
# this gate does not know is REFUSED, never skipped: an unlisted one would read as a pass.
#
# usage: check_rp_node_vectors.sh <elf> <nm> <objdump> <chip> <src-dir>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

# The host binutils is localised and prints translated headers, which every parse below reads.
LC_ALL=C
export LC_ALL

_usage="usage: check_rp_node_vectors.sh <elf> <nm> <objdump> <chip> <src-dir>"
elf="${1:?$_usage}"
nm="${2:?$_usage}"
objdump="${3:?$_usage}"
chip="${4:?$_usage}"
src="${5:?$_usage}"

case "$chip" in
    rp2350)
        TABLE=g_node_isr_vector
        PARK=kickos_rp2350_node_park
        SERVICE=kickos_rp2350_doorbell_service
        LIMITS="$src/arch/arm/chip/$chip/include/kickos/chip_limits.h"
        BELL_MACRO=KICKOS_RP2350_SIO_IRQ_BELL
        # Cortex-M: slots 0..15 are the architecturally fixed core exceptions, and external
        # line n is slot 16 + n.
        CORE_SLOTS=16
        # A thumb function's address carries bit 0 set in a vector word; nm prints it clear.
        THUMB=1
        ;;
    *)
        fail "check_rp_node_vectors.sh knows no chip '$chip'. The table symbol, the park
  handler, the service body and the slot numbering are all per part, so an unlisted one would
  find no table, assert nothing, and report a pass" ;;
esac

[ -f "$elf" ] || fail "no image at $elf"
[ -x "$nm" ] || fail "no nm at $nm; the table's address cannot be read out of the image and
  every assertion below would rest on a hard-coded layout"
[ -x "$objdump" ] || fail "no objdump at $objdump; there are no table bytes to read"
[ -r "$LIMITS" ] || fail "cannot read $LIMITS, which is where the slot count and the doorbell
  line come from. Restating them here instead is what this gate is written not to do"

scratch_dir

# --- the numbers, out of the chip's own header --------------------------------
limit_value() { # <macro>
    sed -n "s/^[[:space:]]*#[[:space:]]*define[[:space:]][[:space:]]*$1[[:space:]][[:space:]]*\([0-9][0-9]*\)[[:space:]]*\$/\1/p" \
        "$LIMITS" | tail -n 1
}

MAX_IRQ="$(limit_value KICKOS_MAX_IRQ)"
BELL="$(limit_value "$BELL_MACRO")"
require_number "$MAX_IRQ" "KICKOS_MAX_IRQ in $LIMITS"
require_number "$BELL" "$BELL_MACRO in $LIMITS"
if [ "$BELL" -ge "$MAX_IRQ" ]; then
    fail "$BELL_MACRO is $BELL, at or past the $MAX_IRQ line(s) KICKOS_MAX_IRQ declares: the
  doorbell slot would fall outside the table and this gate would assert over the wrong words"
fi

ENTRIES=$((CORE_SLOTS + MAX_IRQ))
BYTES=$((ENTRIES * 4))
BELL_SLOT=$((CORE_SLOTS + BELL))

# The reader below takes four words per dump line, which holds only while the region is a whole
# number of 16-byte lines. A part that breaks that is REFUSED rather than misread.
if [ $((BYTES % 16)) -ne 0 ]; then
    fail "the node table is $BYTES byte(s), not a multiple of 16, so the last dump line
  carries fewer than four words and the reader would take the ASCII column for one"
fi

# VTOR takes a table aligned to its own length rounded up to a power of two, and to at least
# 128 bytes (Armv8-M / Armv7-M, VTOR.TBLOFF).
ALIGN=128
while [ "$ALIGN" -lt "$BYTES" ]; do
    ALIGN=$((ALIGN * 2))
done

# --- the word reader, and its controls before the image is read ---------------
# objdump -s prints ` <addr> <w0> <w1> <w2> <w3>  <ascii>`, each word four bytes in MEMORY
# order. Emitted here as the value the CPU loads, so the comparison below is against a symbol
# address and not a byte order.
cat > "$TMP/words.awk" <<'AWK'
/^ [0-9a-f][0-9a-f]* / {
    for (i = 2; i <= 5; i++) {
        w = $i
        if (w !~ /^[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]$/) {
            print "BAD " NR
            exit
        }
        print substr(w, 7, 2) substr(w, 5, 2) substr(w, 3, 2) substr(w, 1, 2)
    }
}
AWK

read_words() { # <dump>
    awk -f "$TMP/words.awk" "$1"
}

cat > "$TMP/ctl_dump" <<'EOF'
Contents of section .text:
 10000200 bd120010 bd120010 31140010 bd120010  ................
EOF
ctl="$(read_words "$TMP/ctl_dump" | tr '\n' ' ')"
case "$ctl" in
    "100012bd 100012bd 10001431 100012bd ") ;;
    *) fail "the word reader answered [$ctl] for a planted dump whose four words are
  100012bd 100012bd 10001431 100012bd, so it does not see what a vector slot holds and every
  assertion resting on it is meaningless" ;;
esac

cat > "$TMP/ctl_dump_bad" <<'EOF'
Contents of section .text:
 10000200 bd120010 ....xxxx 31140010 bd120010  ................
EOF
ctl="$(read_words "$TMP/ctl_dump_bad" | tr '\n' ' ')"
case "$ctl" in
    "100012bd BAD 2 ") ;;
    *) fail "the word reader answered [$ctl] for a planted dump carrying a column it cannot
  decode, so a dump whose shape has moved would read as a table full of park handlers" ;;
esac

# --- the symbols --------------------------------------------------------------
tool_out "$TMP/nm" "[0-9a-fA-F]" "$nm" -S --defined-only "$elf"
require_nonempty "$TMP/nm" "$nm printed no symbol at all for $elf, so the corpus is UNKNOWN
  rather than empty and every verdict below it would be vacuous"

sym_addr() { # <name>
    awk -v s="$1" '$NF == s { print $1; exit }' "$TMP/nm"
}
sym_size() { # <name>
    awk -v s="$1" 'NF == 4 && $4 == s { print $2; exit }' "$TMP/nm"
}

for sym in "$TABLE" "$PARK" "$SERVICE"; do
    if [ -z "$(sym_addr "$sym")" ]; then
        fail "no defined symbol '$sym' in $elf. The node table, its park handler and the
  doorbell service are the three things this gate reads; an absent one is a link that dropped
  the table, not a table that is correct"
    fi
done

TABLE_HEX="$(sym_addr "$TABLE")"
PARK_HEX="$(sym_addr "$PARK")"
SERVICE_HEX="$(sym_addr "$SERVICE")"
TABLE_SIZE_HEX="$(sym_size "$TABLE")"
[ -n "$TABLE_SIZE_HEX" ] || fail "'$TABLE' has no size in $elf, so this gate cannot tell a
  whole table from a label the linker kept"

TABLE_DEC=$((0x$TABLE_HEX))
TABLE_SIZE_DEC=$((0x$TABLE_SIZE_HEX))
PARK_WORD="$(printf '%08x' $((0x$PARK_HEX | THUMB)))"
SERVICE_WORD="$(printf '%08x' $((0x$SERVICE_HEX | THUMB)))"

if [ "$TABLE_SIZE_DEC" -ne "$BYTES" ]; then
    fail "'$TABLE' is $TABLE_SIZE_DEC byte(s) in $elf, and $LIMITS says the part has
  $CORE_SLOTS core slot(s) and $MAX_IRQ line(s), which is $BYTES. A table shorter than the
  vector space leaves the slots past its end pointing at whatever the linker put there"
fi

if [ $((TABLE_DEC % ALIGN)) -ne 0 ]; then
    fail "'$TABLE' is at 0x$TABLE_HEX, not a multiple of $ALIGN. VTOR ignores the low bits, so
  the core would fetch this table from a lower address and every slot would be wrong"
fi

echo "== node table '$TABLE' at 0x$TABLE_HEX, $ENTRIES slot(s), $ALIGN-byte aligned, in $elf =="

# --- the table's words --------------------------------------------------------
tool_out "$TMP/dump" "^ [0-9a-f]+ " "$objdump" -s \
    --start-address="$TABLE_DEC" --stop-address="$((TABLE_DEC + BYTES))" "$elf"
read_words "$TMP/dump" > "$TMP/words"
require_nonempty "$TMP/words" "$objdump printed no table bytes for $elf"

if grep -q '^BAD ' "$TMP/words"; then
    fail "the word reader could not decode dump line $(sed -n 's/^BAD //p' "$TMP/words") of
  $objdump -s over '$TABLE'. Its verdict is UNKNOWN, not clean"
fi

words="$(wc -l < "$TMP/words" | tr -d ' ')"
require_number "$words" "the word count of $TABLE"
if [ "$words" -ne "$ENTRIES" ]; then
    fail "read $words word(s) out of '$TABLE' where $LIMITS says $ENTRIES. The region asked
  for and the region decoded disagree, so the slot numbering below would be off"
fi

findings=0
slot=0
while read -r word; do
    if [ "$slot" -eq "$BELL_SLOT" ]; then
        if [ "$word" != "$SERVICE_WORD" ]; then
            echo "FINDING: slot $BELL_SLOT is the doorbell line ($BELL_MACRO = $BELL, plus
  $CORE_SLOTS core slots) and holds 0x$word, not '$SERVICE' at 0x$SERVICE_WORD. The node takes
  its doorbell somewhere else, so the rendezvous its peer waits on is never answered" >&2
            findings=$((findings + 1))
        fi
    elif [ "$word" != "$PARK_WORD" ]; then
        echo "FINDING: slot $slot holds 0x$word, not '$PARK' at 0x$PARK_WORD. That line
  reaches something other than the park on a core with no kernel of its own" >&2
        findings=$((findings + 1))
    fi
    slot=$((slot + 1))
done < "$TMP/words"

echo "   corpus: $words slot(s) read, $((words - 1)) expected to hold '$PARK' and slot
  $BELL_SLOT to hold '$SERVICE'"

# --- the park is bounded ------------------------------------------------------
# Every branch in the body targets the body, and nothing in it is a call: a park that leaves
# passes the routing check above.
cat > "$TMP/park.awk" <<'AWK'
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
    n++
    mnem = text
    sub(/[ \t].*$/, "", mnem)
    if (mnem ~ /^(bl|blx)(\.[nw])?$/) { print "CALL " mnem; leaves++ ; next }
    if (mnem !~ /^(b|bx|bxns|blxns)(\.[nw])?$/ && mnem !~ /^b[a-z][a-z]?(\.[nw])?$/) { next }
    tgt = text
    if (text !~ /</) { print "INDIRECT " mnem; leaves++; next }
    sub(/^[^<]*</, "", tgt)
    sub(/>.*$/, "", tgt)
    sub(/\+0x[0-9a-f]+$/, "", tgt)
    if (tgt != sym) { print "AWAY " tgt; leaves++ }
}
END {
    if (!seen) { print "NOSYM"; exit }
    if (n == 0) { print "NOINSN"; exit }
    if (leaves == 0) { print "BOUNDED " n }
}
AWK

# Planted in the shape the invocation below produces, which is --no-show-raw-insn: a control
# carrying the raw-bytes column would read its first byte group as the mnemonic.
cat > "$TMP/ctl_park_ok" <<EOF
100012bc <$PARK>:
100012bc:      cpsid   i
100012be:      wfi
100012c0:      b.n     100012be <$PARK+0x2>
EOF
ctl="$(awk -v sym="$PARK" -f "$TMP/park.awk" "$TMP/ctl_park_ok" | tr '\n' ' ')"
case "$ctl" in
    "BOUNDED 3 ") ;;
    *) fail "the park reader answered [$ctl] for a planted park that masks, waits and branches
  to itself, so it cannot recognise the shape this gate requires" ;;
esac

cat > "$TMP/ctl_park_bad" <<EOF
100012bc <$PARK>:
100012bc:      cpsid   i
100012be:      bl      100012c4 <kickos_isr_irq>
100012c2:      b.n     100012be <$PARK+0x2>
EOF
ctl="$(awk -v sym="$PARK" -f "$TMP/park.awk" "$TMP/ctl_park_bad" | tr '\n' ' ')"
case "$ctl" in
    "CALL bl ") ;;
    *) fail "the park reader answered [$ctl] for a planted park that calls out of itself. That
  is the defect this half of the gate exists to catch, so a reader that does not report it
  cannot go red" ;;
esac

tool_out "$TMP/dis" "^[0-9a-f]+ <.*>:\$" "$objdump" -d --no-show-raw-insn "$elf"
rec="$(awk -v sym="$PARK" -f "$TMP/park.awk" "$TMP/dis" | tr '\n' ' ')"
case "$rec" in
    "NOSYM "*)
        fail "'$PARK' is a defined symbol in $elf but the disassembly carries no body for it,
  so the park reader started nowhere. The disassembler's output shape has moved" ;;
    "NOINSN "*)
        fail "the body of '$PARK' in $elf disassembles to no instruction at all, so the corpus
  is UNKNOWN rather than empty" ;;
    "BOUNDED "*)
        echo "   corpus: '$PARK' is $(printf '%s' "$rec" | cut -d' ' -f2) instruction(s), none
  of which leaves the body" ;;
    *)
        echo "FINDING: '$PARK' can leave its own body: [$rec]. A node line that reaches the
  park would then reach whatever the park reaches" >&2
        findings=$((findings + 1)) ;;
esac

if [ "$findings" -ne 0 ]; then
    fail "$findings finding(s): node 1's vector table in $elf does not confine the node"
fi

echo "PASS: '$TABLE' routes slot $BELL_SLOT into '$SERVICE' and its other $((ENTRIES - 1))
  slot(s) into '$PARK', which branches nowhere but into itself"
exit 0
