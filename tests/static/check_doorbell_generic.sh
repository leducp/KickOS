#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The doorbell carries a rendezvous and a reschedule over one raise, and this reads the LINKED
# IMAGE for the boundary between them: the send publishes no reschedule, and the dispatch enters
# the scheduler only when the cell says one stands.
#
# REFUSED: a send that branches to the reschedule publisher, an instruction-side rendezvous that
# does, a dispatch whose transfer into the scheduler is not guarded by the take that consumes the
# cell, and a publisher with no caller at all. The last one is the vacuity trap: with the
# mechanism deleted every other assertion here holds over an image that cannot reschedule a peer.
#
# WHY THE PUBLISHER'S CALLER SIDE IS ASSERTED RATHER THAN ITS BODY. A raise is an edge and the
# kernel lock's acquire loop absorbs it by POLLING, which acknowledges the raise and enters no
# scheduler, so the reschedule has to be state published ahead of the raise. Nothing stops a
# backend from publishing that state for every raise it makes, and then a rendezvous the caller
# wanted no switch out of costs each target a scheduler entry and a contended kernel lock. What
# separates the two is which side of the seam names the publisher, and that IS readable here: a
# caller whose name begins with arch_ is a backend publishing scheduling intent again.
#
# The two readers go through planted listings before the image is read, the failing shapes
# included: a reader that cannot report the defect cannot go red.
#
# AN EMPTY CORPUS IS A FAILURE, not a pass. A body this cannot decode, or a symbol that moved,
# is UNKNOWN.
#
# usage: check_doorbell_generic.sh <elf> <nm> <objdump>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

# The host binutils is localised and prints translated headers, which every parse below reads.
LC_ALL=C
export LC_ALL

_usage="usage: check_doorbell_generic.sh <elf> <nm> <objdump>"
elf="${1:?$_usage}"
nm="${2:?$_usage}"
objdump="${3:?$_usage}"

# The raise, and the one rendezvous in the tree that goes over it.
SEND=arch_ipi_send
RDV=kickos_arm64_instruction_side_rendezvous
# The interrupt dispatch, the only body that may enter the scheduler off a doorbell.
DISPATCH=kickos_armv8a_gic_dispatch
# The cell's publisher, its consumer, and the scheduler entry the consumer guards.
OWE=kickos_kernel_core_resched_owe
TAKE=kickos_kernel_core_resched_take
RESCHED=kickos_kernel_core_resched
# A defined-symbol count below this says nm was read wrong, whatever it printed.
SYM_FLOOR=100

[ -f "$elf" ] || fail "no image at $elf"
[ -x "$nm" ] || fail "no nm at $nm; the symbol table cannot be read out of the image and every
  assertion below would rest on a hard-coded layout"
[ -x "$objdump" ] || fail "no objdump at $objdump; there is no instruction stream to decode"

scratch_dir

# --- reader one: the branch targets inside one body --------------------------
# Emits one NAME per line, deduplicated, or a single NOSYM / NOINSN record. Direct branches
# only: an indirect one names no symbol and contributes nothing either way.
cat > "$TMP/calls.awk" <<'AWK'
/^[0-9a-f]+ <.*>:$/ {
    name = $2
    gsub(/[<>:]/, "", name)
    inbody = (name == sym)
    if (inbody) { seen = 1; self = name }
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
    if (mnem != "b" && mnem != "bl" && mnem !~ /^b\./) { next }
    if (text !~ /</) { next }
    tgt = text
    sub(/^[^<]*</, "", tgt)
    sub(/>.*$/, "", tgt)
    sub(/\+0x[0-9a-f]+$/, "", tgt)
    if (tgt == self) { next }
    if (!(tgt in got)) { got[tgt] = 1; order[++k] = tgt }
}
END {
    if (!seen) { print "NOSYM"; exit }
    if (n == 0) { print "NOINSN"; exit }
    for (i = 1; i <= k; i++) { print order[i] }
}
AWK

body_calls() { # <listing> <symbol>
    awk -v sym="$2" -f "$TMP/calls.awk" "$1"
}

# --- reader two: the guard between the take and the scheduler entry ----------
# Ordinals, not addresses: the verdict is an ORDER. Emits exactly one record.
#
# A conditional branch is what a guard lowers to on this backend: cbz/cbnz and tbz/tbnz test a
# register outright, b.<cond> tests the flags a compare set.
cat > "$TMP/guard.awk" <<'AWK'
/^[0-9a-f]+ <.*>:$/ {
    name = $2
    gsub(/[<>:]/, "", name)
    inbody = (name == sym)
    if (inbody) { seen = 1; self = name }
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
    tgt = ""
    if (text ~ /</) {
        tgt = text
        sub(/^[^<]*</, "", tgt)
        sub(/>.*$/, "", tgt)
        sub(/\+0x[0-9a-f]+$/, "", tgt)
    }
    if ((mnem == "b" || mnem == "bl" || mnem ~ /^b\./) && tgt == take && takeat == 0) {
        takeat = n
        next
    }
    if ((mnem == "b" || mnem == "bl" || mnem ~ /^b\./) && tgt == resched && reschedat == 0) {
        reschedat = n
    }
    if (takeat != 0 && guard == 0 && reschedat == 0) {
        if (mnem ~ /^cbn?z$/ || mnem ~ /^tbn?z$/ || mnem ~ /^b\./) { guard = n }
    }
}
END {
    if (!seen) { print "NOSYM"; exit }
    if (n == 0) { print "NOINSN"; exit }
    if (takeat == 0) { print "NOTAKE " n; exit }
    if (reschedat == 0) { print "NORESCHED " n; exit }
    if (guard == 0) { print "UNGUARDED " takeat " " reschedat " " n; exit }
    print "GUARDED " takeat " " guard " " reschedat " " n
}
AWK

body_guard() { # <listing> <symbol>
    awk -v sym="$2" -v take="$TAKE" -v resched="$RESCHED" -f "$TMP/guard.awk" "$1"
}

# --- the readers' controls, before the image is read --------------------------
# Planted listings in the shape the invocation below produces, which is --no-show-raw-insn: a
# control carrying the raw-bytes column would read its first byte group as the mnemonic and prove
# the reader against input the gate never hands it.
cat > "$TMP/ctl_send_clean" <<EOF
0000000000001000 <planted_send>:
    1000:	stp	x29, x30, [sp, #-32]!
    1004:	bl	2000 <arch_cpu_id>
    1008:	bl	2100 <planted_poll>
    100c:	b	2200 <kickos_gicv2_doorbell_send>
EOF
cat > "$TMP/ctl_send_dirty" <<EOF
0000000000001000 <planted_send>:
    1000:	stp	x29, x30, [sp, #-32]!
    1004:	bl	2000 <$OWE>
    1008:	b	2200 <kickos_gicv2_doorbell_send>
EOF
cat > "$TMP/ctl_guarded" <<EOF
0000000000001000 <planted_dispatch>:
    1000:	bl	2000 <kickos_isr_timer>
    1004:	bl	2100 <$TAKE>
    1008:	cbz	w0, 1014 <planted_dispatch+0x14>
    100c:	b	2200 <$RESCHED>
    1010:	ret
EOF
cat > "$TMP/ctl_unguarded" <<EOF
0000000000001000 <planted_dispatch>:
    1000:	bl	2000 <kickos_isr_timer>
    1004:	bl	2100 <$TAKE>
    1008:	ldp	x29, x30, [sp], #48
    100c:	b	2200 <$RESCHED>
EOF

ctl="$(body_calls "$TMP/ctl_send_clean" planted_send | tr '\n' ' ')"
case "$ctl" in
    "arch_cpu_id planted_poll kickos_gicv2_doorbell_send ") ;;
    *) fail "the branch reader answered [$ctl] for a planted send, so it does not see the
  targets a body branches to and every assertion resting on it is meaningless" ;;
esac

ctl="$(body_calls "$TMP/ctl_send_dirty" planted_send | grep -c -x "$OWE" || ctl=0)"
if [ "$ctl" != "1" ]; then
    fail "the branch reader found $ctl call(s) to '$OWE' in a planted send that branches to it
  once. That is the defect this gate exists to catch, so a reader that does not report it
  cannot go red"
fi

ctl="$(body_calls "$TMP/ctl_send_clean" a_symbol_no_listing_carries)"
case "$ctl" in
    NOSYM) ;;
    *) fail "the branch reader answered [$ctl] for a symbol the listing does not carry, so a
  renamed body would read as a clean one" ;;
esac

ctl="$(body_guard "$TMP/ctl_guarded" planted_dispatch)"
case "$ctl" in
    "GUARDED 2 3 4 5") ;;
    *) fail "the guard reader answered [$ctl] for a planted dispatch whose scheduler entry is
  guarded by a conditional branch on the take's answer, so it cannot recognise the shape this
  gate requires" ;;
esac

ctl="$(body_guard "$TMP/ctl_unguarded" planted_dispatch)"
case "$ctl" in
    "UNGUARDED 2 4 4") ;;
    *) fail "the guard reader answered [$ctl] for a planted dispatch that enters the scheduler
  unconditionally after the take. That is the defect this gate exists to catch, so a reader that
  does not report it cannot go red" ;;
esac

# --- the symbol table ---------------------------------------------------------
tool_out "$TMP/nm" "[0-9a-fA-F]" "$nm" -S --defined-only "$elf"
require_nonempty "$TMP/nm" "$nm printed no symbol at all for $elf, so the corpus is UNKNOWN
  rather than empty and every verdict below it would be vacuous"
syms="$(wc -l < "$TMP/nm" | tr -d ' ')"
require_number "$syms" "the defined-symbol count"
if [ "$syms" -lt "$SYM_FLOOR" ]; then
    fail "$nm reports $syms defined symbol(s) in $elf, below the floor of $SYM_FLOOR. A table
  that short is a misread, not a small image, and the corpus is UNKNOWN"
fi

for sym in "$SEND" "$RDV" "$DISPATCH" "$OWE" "$TAKE" "$RESCHED"; do
    size="$(awk -v s="$sym" 'NF == 4 && $4 == s { print $2; exit }' "$TMP/nm")"
    if [ -z "$size" ]; then
        fail "no sized defined symbol '$sym' in $elf: it was renamed, made static or inlined
  away, so this gate reports an absence it cannot tell apart from a failure to read"
    fi
    case "$size" in
        *[!0]*) ;;
        *) fail "'$sym' has size 0 in $elf: the symbol survived as a label but its body is
  gone" ;;
    esac
done

# --- the instruction stream ---------------------------------------------------
tool_out "$TMP/dis" "^[0-9a-f]+ <.*>:\$" "$objdump" -d --no-show-raw-insn "$elf"
require_nonempty "$TMP/dis" "$objdump printed no disassembly for $elf"

echo "== the doorbell's scheduling boundary in $elf =="

findings=0

# The raise, and the rendezvous over it, publish no reschedule and enter no scheduler.
for sym in "$SEND" "$RDV"; do
    rec="$(body_calls "$TMP/dis" "$sym")"
    case "$rec" in
        NOSYM)
            fail "'$sym' is a sized symbol in $elf but the disassembly carries no body for it,
  so the reader started nowhere. The disassembler's output shape has moved" ;;
        NOINSN)
            fail "the body of '$sym' in $elf disassembles to no instruction at all, so the
  corpus is UNKNOWN rather than empty" ;;
    esac
    printf '%s\n' "$rec" > "$TMP/targets"
    reached="$(wc -l < "$TMP/targets" | tr -d ' ')"
    require_number "$reached" "the branch-target count of $sym"
    if [ "$reached" -eq 0 ]; then
        fail "the body of '$sym' in $elf branches to no symbol at all. A leaf there means the
  raise or the rendezvous was restructured and this reader is looking at the wrong body"
    fi
    echo "   corpus: '$sym' branches to $reached symbol(s)"
    for banned in "$OWE" "$RESCHED"; do
        if grep -q -x -F -e "$banned" "$TMP/targets"; then
            findings=$((findings + 1))
            echo "FINDING: '$sym' branches to '$banned'" >&2
            sed -n '1,20p' "$TMP/targets" | sed 's/^/      /' >&2
        fi
    done
done

# The dispatch enters the scheduler only behind the take.
rec="$(body_guard "$TMP/dis" "$DISPATCH")"
kind="$(printf '%s\n' "$rec" | cut -d' ' -f1)"
f2="$(printf '%s\n' "$rec" | cut -d' ' -f2)"
f3="$(printf '%s\n' "$rec" | cut -d' ' -f3)"
f4="$(printf '%s\n' "$rec" | cut -d' ' -f4)"
f5="$(printf '%s\n' "$rec" | cut -d' ' -f5)"
case "$kind" in
    NOSYM)
        fail "'$DISPATCH' is a sized symbol in $elf but the disassembly carries no body for it,
  so the reader started nowhere. The disassembler's output shape has moved" ;;
    NOINSN)
        fail "the body of '$DISPATCH' in $elf disassembles to no instruction at all, so the
  corpus is UNKNOWN rather than empty" ;;
    NOTAKE)
        fail "the body of '$DISPATCH' in $elf branches to '$TAKE' nowhere across its $f2
  instruction(s). Nothing consumes the cell in the one body that may, so a reschedule owed to
  this core stands forever and the core re-raises a doorbell at itself on every release" ;;
    NORESCHED)
        fail "the body of '$DISPATCH' in $elf branches to '$RESCHED' nowhere across its $f2
  instruction(s). A reschedule owed to this core is consumed and never acted on, which is the
  starvation the cell exists to prevent, and it is also UNKNOWN rather than a pass" ;;
    UNGUARDED)
        echo "FINDING: in '$DISPATCH' the take is instruction #$f2 and the transfer to
  '$RESCHED' is #$f3, with no conditional branch between them across $f4 instruction(s): every
  doorbell enters the scheduler, a rendezvous the initiator wanted no switch out of included" >&2
        findings=$((findings + 1)) ;;
    GUARDED)
        echo "   corpus: $f5 instruction(s) in '$DISPATCH', take at #$f2, guard at #$f3,
  scheduler entry at #$f4"
        require_number "$f2" "the take's ordinal in $DISPATCH"
        require_number "$f3" "the guard's ordinal in $DISPATCH"
        require_number "$f4" "the scheduler entry's ordinal in $DISPATCH" ;;
    *)
        fail "the guard reader emitted [$rec] for '$DISPATCH', a record this gate does not
  model" ;;
esac

# --- the publisher's callers, which is the vacuity trap ----------------------
awk '/^[0-9a-f]+ <.*>:$/ { name = $2; gsub(/[<>:]/, "", name); next }
     $0 !~ /^[ \t]*[0-9a-f]+:/ { next }
     {
         text = $0
         sub(/^[^:]*:[ \t]*/, "", text)
         mnem = text
         sub(/[ \t].*$/, "", mnem)
         if (mnem != "b" && mnem != "bl" && mnem !~ /^b\./) { next }
         if (text !~ /</) { next }
         tgt = text
         sub(/^[^<]*</, "", tgt)
         sub(/>.*$/, "", tgt)
         sub(/\+0x[0-9a-f]+$/, "", tgt)
         if (tgt != owe || name == owe) { next }
         print name
     }' owe="$OWE" "$TMP/dis" | sort -u > "$TMP/publishers" || true
publishers="$(wc -l < "$TMP/publishers" | tr -d ' ')"
require_number "$publishers" "the caller count of $OWE"
if [ "$publishers" -eq 0 ]; then
    fail "nothing in $elf branches to '$OWE'. With no publisher every assertion above holds
  over an image that cannot ask a peer to reschedule at all, so this is the vacuous pass the
  trap exists to refuse, not a clean tree"
fi
echo "   corpus: $publishers symbol(s) publish through '$OWE'"
while read -r caller; do
    case "$caller" in
        arch_*)
            echo "FINDING: '$caller' branches to '$OWE': a backend body publishes scheduling
  intent, so every raise it makes carries a reschedule and a rendezvous over the same doorbell
  costs each target a scheduler entry and a contended kernel lock" >&2
            findings=$((findings + 1)) ;;
        *)
            echo "      $caller" ;;
    esac
done < "$TMP/publishers"

if [ "$findings" -ne 0 ]; then
    fail "$findings finding(s): the doorbell's rendezvous half and its scheduling half are not
  separated in $elf"
fi

echo "PASS: '$SEND' and '$RDV' publish no reschedule, '$DISPATCH' enters the scheduler only
  behind '$TAKE', and '$OWE' is published from above the arch seam alone"
exit 0
