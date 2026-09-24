#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The end-to-end benchmark's publication ordering, read out of the linked image. ARM releases
# the record, the waiter releases PARKED under the kernel lock, RAISE acquires that then
# releases t0 before injection, and CLOSE acquires t0. Each arm counts a position between two
# named calls, so ordering sited elsewhere in the same body cannot stand in for it. The park
# marker must follow the lock and precede the block, or delivery can reach a waiter still
# running. QEMU does not expose missing memory ordering, which is why this is read statically.
#
# An ordered load and an ordered store, however the arch spells them; the spelling itself is
# not the claim. Three realisations are modelled below:
#
#   mnemonic   the direction is in the instruction itself: armv8a ldar and stlr.
#   operand    the direction is in a separate fence's operands: rv64imac `fence r,rw` and
#              `fence rw,w`. `rw,rw` is a full barrier and neither half of this pairing.
#   pair       the arch has one barrier and it carries no direction at all, so the direction is
#              the access beside it: an acquire is a load then the barrier, a release is the
#              barrier then a store. lx6, whose only ordering instruction is memw.
#
# Under `pair` an acquire and a release cannot be told apart by mnemonic, so an arm asking for
# one of them is answered by the shape and not by a count of barriers. Every realisation still
# refuses the relaxed downgrade (the ordering instruction removed outright) and ordering sited
# outside the bracket that owes it.
#
# An arch not in the table below is refused rather than skipped.
#
# usage: check_bench_e2e_publish.sh <elf> <objdump> <arch>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_bench_e2e_publish.sh <elf> <objdump> <arch>"
elf="${1:?$_usage}"
objdump="${2:?$_usage}"
arch="${3:?$_usage}"

# The bodies, by their MANGLED names: they are C++ and the scope reader keys on the name
# objdump prints.
SYM_ARM=_ZN6kickos13bench_e2e_armEi
SYM_PARK=_ZN6kickos19bench_e2e_park_markEv
SYM_RAISE=_ZN6kickos15bench_e2e_raiseEv
SYM_CLOSE=_ZN6kickos15bench_e2e_closeEv

# The calls that bracket each body's ordering. Mangled for the same reason.
C_CURRENT='<_ZN6kickos5sched7currentEv>'
C_UNLOCK='<_ZN6kickos11klock_leaveEv>'
C_CLOCK='<arch_clock_now>'
C_INJECT='<arch_irq_inject>'
C_LOCK='<_ZN6kickos11klock_enterEv>'
# What the notification wait blocks with. It has no queue, so it never reaches wq_block.
C_BLOCK='<_ZN6kickos14park_queuelessEPNS_6ThreadENS_8WaitKindEPv>'
C_PARK='<_ZN6kickos19bench_e2e_park_markEv>'

# ACQ/REL: the mnemonic, and the operand where the mnemonic alone does not say the direction.
# An empty operand means the mnemonic decides. BAR_MNEM set instead selects the `pair` reading
# and names the one barrier. PLAIN_*: the same access with no ordering, for the planted control
# that proves the reader reports the downgrade. BRANCH: the mnemonics whose operand is an
# instruction boundary, for gate.sh's synced_body. U32: how this target's toolchain mangles
# uint32_t, which is `unsigned int` on both LP64 targets here and `unsigned long` on xtensa.
case "$arch" in
    armv8a)
        ACQ_MNEM=ldar; ACQ_OPS=""
        REL_MNEM=stlr; REL_OPS=""
        BAR_MNEM=""; LOAD_RE=""; STORE_RE=""
        PLAIN_LOAD='ldr	w0, [x19]'
        PLAIN_STORE='str	w0, [x19]'
        ACQ_INSN='ldar	w0, [x19]'
        REL_INSN='stlr	w0, [x19]'
        CALL_ONE='bl	ffffff804001b400 <arch_clock_now>'
        CALL_TWO='bl	ffffff804001b2c8 <arch_irq_inject>'
        CALL_MARK='bl	ffffff8040017b1c <_ZN6kickos19bench_e2e_park_markEv>'
        FWD_OP='b	ffffff8040017c40'
        RET_INSN='ret'
        BRANCH='^(b|bl|b[.][a-z]+|cbn?z|tbn?z)$'
        U32=j
        ;;
    rv64imac)
        ACQ_MNEM=fence; ACQ_OPS='r,rw'
        REL_MNEM=fence; REL_OPS='rw,w'
        BAR_MNEM=""; LOAD_RE=""; STORE_RE=""
        PLAIN_LOAD='lw	a5,0(s0)'
        PLAIN_STORE='sw	a5,0(s0)'
        ACQ_INSN='fence	r,rw'
        REL_INSN='fence	rw,w'
        CALL_ONE='jal	ffffffff80012070 <arch_clock_now>'
        CALL_TWO='jal	ffffffff8001077c <arch_irq_inject>'
        CALL_MARK='jal	ffffffff8000fe36 <_ZN6kickos19bench_e2e_park_markEv>'
        FWD_OP='j	ffffffff8000ff10'
        RET_INSN='ret'
        BRANCH='^(j|jal|b(eq|ne|lt|ge|ltu|geu|eqz|nez|lez|gez|ltz|gtz|gt|le|gtu|leu))$'
        U32=j
        ;;
    lx6)
        # A planted acquire and release are TWO instructions here, and the separator below is
        # what lets one control table stand for all three realisations.
        ACQ_MNEM=""; ACQ_OPS=""
        REL_MNEM=""; REL_OPS=""
        BAR_MNEM=memw
        LOAD_RE='^l(8u|16u|32)i([.]n)?$'
        STORE_RE='^s(8|16|32)i([.]n)?$'
        PLAIN_LOAD='l32i.n	a8, a9, 0'
        PLAIN_STORE='s32i.n	a8, a9, 0'
        ACQ_INSN='l32i.n	a8, a9, 0|memw'
        REL_INSN='memw|s32i.n	a8, a9, 0'
        CALL_ONE='call8	40081a40 <arch_clock_now>'
        CALL_TWO='call8	40082390 <arch_irq_inject>'
        CALL_MARK='call8	4008e474 <_ZN6kickos19bench_e2e_park_markEv>'
        FWD_OP='j	4008ca18'
        RET_INSN='retw.n'
        BRANCH='^(b[a-z]*([.]n)?|j|loop[a-z]*)$'
        U32=m
        ;;
    *)
        fail "arch '$arch' is built above one kernel core with KICKOS_BENCH and this gate
  carries no model of its acquire and release, so nothing here can say whether the end-to-end
  protocol publishes anything. Add the arch's row to the table in this file: the mnemonic where
  it carries the direction, the fence operands where they do, or the one barrier and the access
  mnemonics beside it where the barrier carries no direction at all" ;;
esac

# The kernel's notification wait body. There is ONE wait and it is always timed, so nothing
# forwards to it and the name below is the only body that can carry the mark.
SYM_WAIT="_ZN6kickos11notify_waitEPNS_6ThreadE${U32}${U32}${U32}P${U32}"
FWD_INSN="$FWD_OP <$SYM_WAIT>"

[ -f "$elf" ] || fail "no image at $elf"
[ -x "$objdump" ] || fail "no objdump at $objdump; there is no instruction stream to decode"

scratch_dir

# --- the reader ---------------------------------------------------------------
# One record per body: the instruction count, the acquires and releases in it, and then the
# four positional counts: acquires ahead of the opening call, acquires and releases between the
# opening call and the closing one, and calls to a named callee in that same stretch. A body
# given no closing call brackets from the opening call to the end of the body.
#
# A body the bracket calls are not in reports every positional count as zero, the same record a
# body that carries the calls and publishes nothing between them would produce. So a bracket
# call the body was given and does not contain is its own record and never a verdict: NOBRACKET
# for neither of them, NOOPEN and NOCLOSE for one. Without those the reader would answer a
# renamed, inlined or forwarded body with the finding it exists to report.
#
# Under `pair` the ordering is sited at the first instruction of the two, the one the rule is
# stated about: the load an acquire orders, and the barrier a release publishes behind. So the
# window flags of the previous line decide, and the pairing is settled one line late.
#
# `seen`, the body scope, the window and all three of those refusals come from gate.sh's
# scoped_body, which reads tests/lib/objdump_scope.awk and tests/lib/objdump_window.awk ahead of
# this file. The bracket calls are EREs there rather than literals, and no mangled name in the
# table above carries an ERE metacharacter.
cat > "$TMP/reader.awk" <<'AWK'
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
    if (bar_mnem == "")
    {
        if (mnem == acq_mnem && (acq_ops == "" || ops == acq_ops))
        {
            acq++
            if (win_pre) { acq_pre++ }
            else if (win_in) { acq_mid++ }
        }
        if (mnem == rel_mnem && (rel_ops == "" || ops == rel_ops))
        {
            rel++
            if (win_in) { rel_mid++ }
        }
    }
    else
    {
        is_bar = (mnem == bar_mnem)
        if (p_load && is_bar)
        {
            acq++
            if (p_pre) { acq_pre++ }
            else if (p_in) { acq_mid++ }
        }
        if (p_bar && mnem ~ store_re)
        {
            rel++
            if (p_in) { rel_mid++ }
        }
        p_load = (mnem ~ load_re)
        p_bar = is_bar
        p_pre = win_pre
        p_in = win_in
    }
    if (call_mark != "" && win_in && index(text, call_mark) > 0) { mark_mid++ }
}
END {
    printf "BODY %d %d %d %d %d %d %d\n", win_n, acq + 0, rel + 0, acq_pre + 0, acq_mid + 0,
           rel_mid + 0, mark_mid + 0
}
AWK

read_body() { # <listing> <symbol> <opening-call> <closing-call> [<counted-call>]
    scoped_body "$TMP/reader.awk" "$1" "$2" \
        -v acq_mnem="$ACQ_MNEM" -v acq_ops="$ACQ_OPS" \
        -v rel_mnem="$REL_MNEM" -v rel_ops="$REL_OPS" \
        -v bar_mnem="$BAR_MNEM" -v load_re="$LOAD_RE" -v store_re="$STORE_RE" \
        -v win_open="$3" -v win_close="$4" -v call_mark="${5:-}"
}

# --- the reader's controls, before the image is read --------------------------
# Planted listings in the shape the invocation below produces, which is --no-show-raw-insn. An
# element carrying a vertical bar plants several instructions, because an acquire and a release
# are not one instruction on every arch; PLANT_N is what the body came to, so the expected
# records below state the counts alone and never a length that is the table's business.
plant() { # <file> <insn>...
    _f="$1"
    shift
    _a=4096
    PLANT_N=0
    echo "0000000000001000 <planted_body>:" > "$_f"
    for _i in "$@"; do
        _rest="$_i"
        while : ; do
            case "$_rest" in
                *"|"*)
                    _one="${_rest%%|*}"
                    _rest="${_rest#*|}"
                    ;;
                *)
                    _one="$_rest"
                    _rest=""
                    ;;
            esac
            printf '    %x:\t%s\n' "$_a" "$_one" >> "$_f"
            _a=$((_a + 4))
            PLANT_N=$((PLANT_N + 1))
            [ -n "$_rest" ] || break
        done
    done
}

ctl() { # <file> <expected-counts> <prose>
    _got="$(read_body "$1" planted_body "$C_CLOCK" "$C_INJECT")"
    _want="BODY $PLANT_N $2"
    [ "$_got" = "$_want" ] || fail "the reader answered [$_got] and not [$_want] for $3"
}

# The publishing shape: acquire, then the opening call, then the release, then the closing one.
plant "$TMP/ctl_pub" "$ACQ_INSN" "$PLAIN_LOAD" "$CALL_ONE" "$PLAIN_STORE" "$REL_INSN" \
    "$CALL_TWO" "$RET_INSN"
ctl "$TMP/ctl_pub" "1 1 1 0 1 0" "a planted body that acquires, reads its stamp,
  releases and then injects, so it cannot recognise the shape this gate requires and every
  verdict below is meaningless"

# The consuming shape: the acquire sits INSIDE the bracket, which is what the close owes.
plant "$TMP/ctl_mid" "$PLAIN_LOAD" "$PLAIN_LOAD" "$CALL_ONE" "$ACQ_INSN" "$REL_INSN" \
    "$CALL_TWO" "$RET_INSN"
ctl "$TMP/ctl_mid" "1 1 0 1 1 0" "a planted body whose acquire sits between the two
  calls, which is the close's shape and would otherwise never be read"

# The downgrade: the same body with both orderings dropped to plain accesses.
plant "$TMP/ctl_relaxed" "$PLAIN_LOAD" "$PLAIN_LOAD" "$CALL_ONE" "$PLAIN_STORE" \
    "$PLAIN_STORE" "$CALL_TWO" "$RET_INSN"
ctl "$TMP/ctl_relaxed" "0 0 0 0 0 0" "a planted body whose acquire and release were
  dropped to plain accesses. That is the defect this gate exists to catch, so a reader that
  does not report it cannot go red"

# The ordering present but OUTSIDE the bracket: an acquire and a release the body carries for
# some other field entirely.
plant "$TMP/ctl_outside" "$ACQ_INSN" "$REL_INSN" "$PLAIN_STORE" "$CALL_ONE" "$PLAIN_STORE" \
    "$CALL_TWO" "$RET_INSN"
ctl "$TMP/ctl_outside" "1 1 1 0 0 0" "a planted body whose release sits AHEAD of the
  stamp it is meant to publish. The release then hands over a t0 the closer reads from the
  previous pass, and a reader that does not separate the two proves nothing"

# The open-ended bracket, which is the park mark's shape: no closing call is given, so the
# window runs from the identity test to the end of the body.
plant "$TMP/ctl_tail" "$ACQ_INSN" "$CALL_ONE" "$PLAIN_STORE" "$REL_INSN" "$RET_INSN"
_got="$(read_body "$TMP/ctl_tail" planted_body "$C_CLOCK" "")"
[ "$_got" = "BODY $PLANT_N 1 1 1 0 1 0" ] || fail "the reader answered [$_got] and not [BODY
  $PLANT_N 1 1 1 0 1 0] for a planted body bracketed from its opening call to its end, which is
  how the park mark is read"

# The counted call, INSIDE the bracket and then on either side of it.
ctl_mark() { # <file> <expected-counts> <prose>
    _got="$(read_body "$1" planted_body "$C_CLOCK" "$C_INJECT" "$C_PARK")"
    _want="BODY $PLANT_N $2"
    [ "$_got" = "$_want" ] || fail "the reader answered [$_got] and not [$_want] for $3"
}

plant "$TMP/ctl_call_in" "$CALL_ONE" "$CALL_MARK" "$CALL_TWO" "$RET_INSN"
ctl_mark "$TMP/ctl_call_in" "0 0 0 0 0 1" "a planted body whose counted call sits
  between the two bracketing ones, which is the only placement the park arm accepts"

plant "$TMP/ctl_call_pre" "$CALL_MARK" "$CALL_ONE" "$CALL_TWO" "$RET_INSN"
ctl_mark "$TMP/ctl_call_pre" "0 0 0 0 0 0" "a planted body whose counted call sits
  AHEAD of the bracket. A reader that counted it would pass a publication made before the lock
  the waking post has to take"

plant "$TMP/ctl_call_post" "$CALL_ONE" "$CALL_TWO" "$CALL_MARK" "$RET_INSN"
ctl_mark "$TMP/ctl_call_post" "0 0 0 0 0 0" "a planted body whose counted call sits PAST
  the bracket. That is the defect this arm exists for: a mark published after the block cannot
  hold a raise off a waiter that is still running"

ctl_dead_reader "$(read_body "$TMP/ctl_pub" a_symbol_no_listing_carries "$C_CLOCK" "$C_INJECT")" \
    "a renamed or inlined body would read as a clean one"

# The body that carries NEITHER bracket call: a forwarder over the body that does. Every
# positional count is zero there for want of a window, which is the record a body that carries
# the calls and orders nothing also produces.
plant "$TMP/ctl_fwd" "$FWD_INSN" "$RET_INSN"
_got="$(read_body "$TMP/ctl_fwd" planted_body "$C_CLOCK" "$C_INJECT")"
[ "$_got" = "NOBRACKET $PLANT_N" ] || fail "the reader answered [$_got] and not [NOBRACKET
  $PLANT_N] for a planted two-instruction forwarder, which carries neither bracketing call.
  Read as a BODY it reports every ordering this gate asks for as missing, so a symbol that moved
  behind a forwarder goes red as a broken protocol"

# One side of the bracket present and the other absent. The window then runs from the opening
# call to the end of the body, or never opens at all, and neither is the stretch the rule is
# stated over.
plant "$TMP/ctl_open_only" "$ACQ_INSN" "$CALL_ONE" "$REL_INSN" "$RET_INSN"
_got="$(read_body "$TMP/ctl_open_only" planted_body "$C_CLOCK" "$C_INJECT")"
[ "$_got" = "NOCLOSE $PLANT_N" ] || fail "the reader answered [$_got] and not [NOCLOSE
  $PLANT_N] for a planted body carrying the opening call and not the closing one. Read as a
  BODY the window silently runs to the end of the body, which counts ordering the rule does not
  reach and turns a refusal into a pass"

plant "$TMP/ctl_close_only" "$ACQ_INSN" "$CALL_TWO" "$REL_INSN" "$RET_INSN"
_got="$(read_body "$TMP/ctl_close_only" planted_body "$C_CLOCK" "$C_INJECT")"
[ "$_got" = "NOOPEN $PLANT_N" ] || fail "the reader answered [$_got] and not [NOOPEN $PLANT_N]
  for a planted body carrying the closing call and not the opening one. The window never opens,
  so every positional count is zero and the body reads as one that publishes nothing"

# Both calls present and in the WRONG ORDER, which no count of either can see.
plant "$TMP/ctl_close_first" "$CALL_TWO" "$ACQ_INSN" "$CALL_ONE" "$REL_INSN" "$RET_INSN"
_got="$(read_body "$TMP/ctl_close_first" planted_body "$C_CLOCK" "$C_INJECT")"
[ "$_got" = "NOCLOSE $PLANT_N" ] || fail "the reader answered [$_got] and not [NOCLOSE
  $PLANT_N] for a planted body whose closing call stands only AHEAD of the opening one. Both
  calls are present, so a reader counting their presence passes it, and the window then runs
  from the opening call to the end of the body and counts ordering the rule does not reach"

if [ -n "$BAR_MNEM" ]; then
    # The barrier on the wrong SIDE of its access. Both instructions of an acquire are present
    # and both of a release are, in the other order, so a reader counting barriers alone passes
    # a body that publishes its store ahead of the barrier and consumes its load behind one.
    plant "$TMP/ctl_flipped" "$PLAIN_STORE" "$BAR_MNEM" "$CALL_ONE" "$BAR_MNEM" "$PLAIN_LOAD" \
        "$CALL_TWO" "$RET_INSN"
    ctl "$TMP/ctl_flipped" "0 0 0 0 0 0" "a planted body whose barrier follows the store it is
  meant to publish and precedes the load it is meant to consume. Every barrier this arch has is
  in it, so a reader counting the barrier alone cannot tell it from the ordered shape"

    # The barrier shared between an acquire and a release, which is the seq_cst load's own
    # trailing fence ordering the store behind it. Both are real and both are counted.
    plant "$TMP/ctl_shared_bar" "$PLAIN_LOAD" "$CALL_ONE" "$PLAIN_LOAD" "$BAR_MNEM" \
        "$PLAIN_STORE" "$CALL_TWO" "$RET_INSN"
    ctl "$TMP/ctl_shared_bar" "1 1 0 1 1 0" "a planted body whose one barrier stands between
  the load it orders and the store it publishes. Counting it for one direction only drops the
  other, and the arm that owes it then reads an unordered body"
fi

synced_body_control

echo "== control: the reader reports the publishing shape, the consuming shape, the relaxed
   downgrade, ordering sited outside the bracket that owes it, an open-ended bracket, a
   counted call inside that bracket and on either side of it, and a body carrying neither
   bracket call, only the opening one, only the closing one, or both in the wrong order"

# --- the instruction stream ---------------------------------------------------
tool_out "$TMP/dis" "^[0-9a-f]+ <.*>:\$" "$objdump" -d --no-show-raw-insn "$elf"
require_nonempty "$TMP/dis" "$objdump printed no disassembly for $elf"

field() { printf '%s\n' "$1" | cut -d' ' -f"$2"; }

record() { # <symbol> <opening-call> <closing-call> <what it carries> [<counted-call>]
    # A linear sweep of a variable-width listing decodes forward from wherever it last stopped,
    # and the esp32 link leaves two zero bytes behind a relaxed jump. The run behind one of
    # those is instructions that were never in the image, and the call this gate counts is one
    # of the ones it eats, which reads as a body that never makes it.
    synced_body "$TMP/body" "$TMP/dis" "$1" "$elf" "$objdump" "$BRANCH"
    _r="$(read_body "$TMP/body" "$1" "$2" "$3" "${5:-}")"
    case "$_r" in
        NOSYM)
            fail "the disassembly of $elf carries no body for '$1', which is $4. It was
  renamed, inlined or compiled out, and a reader that started nowhere would otherwise report
  a clean body" ;;
        NOINSN)
            fail "the body of '$1' in $elf disassembles to no instruction at all, so the
  corpus is UNKNOWN rather than empty" ;;
        "NOBRACKET "*)
            fail "the body of '$1' in $elf, which is $4, carries NEITHER '$2' nor '$3' across
  its $(field "$_r" 2) instruction(s). This gate cannot see the bracket it reads the ordering
  inside, so what it has is UNKNOWN and not an empty bracket: the symbol names a forwarder, or
  both callees were inlined or renamed. Point it at the body that carries them" ;;
        "NOOPEN "*)
            fail "the body of '$1' in $elf, which is $4, never reaches '$2' across its
  $(field "$_r" 2) instruction(s), so the bracket this ordering sits in never opens and every
  positional count below would read zero. That is UNKNOWN, not a bracket that publishes
  nothing" ;;
        "NOCLOSE "*)
            fail "the body of '$1' in $elf, which is $4, reaches no '$3' PAST '$2' across its
  $(field "$_r" 2) instruction(s), so the bracket runs to the end of the body instead of to the
  call that closes it. That is UNKNOWN, not a pass: ordering counted past the closing call is
  ordering the rule does not reach" ;;
        "BODY "*) ;;
        *)
            fail "the reader emitted [$_r] for '$1', a record this gate does not model" ;;
    esac
    REC_N="$(field "$_r" 2)"
    REC_ACQ="$(field "$_r" 3)"
    REC_REL="$(field "$_r" 4)"
    REC_PRE="$(field "$_r" 5)"
    REC_MID="$(field "$_r" 6)"
    REC_RMID="$(field "$_r" 7)"
    REC_MARK="$(field "$_r" 8)"
    for _f in "$REC_N" "$REC_ACQ" "$REC_REL" "$REC_PRE" "$REC_MID" "$REC_RMID" "$REC_MARK"; do
        require_number "$_f" "a count in the record for $1"
    done
    [ "$REC_N" -gt 0 ] || fail "'$1' in $elf reports zero instructions after decoding"
    echo "   $1: $REC_N instruction(s), $SYNCED_N re-decode(s)"
    echo "     acquires $REC_ACQ ($REC_PRE before / $REC_MID inside the bracket), releases
     $REC_REL ($REC_RMID inside), counted calls $REC_MARK"
}

echo "== the end-to-end protocol's publication in $elf =="

rc=0

# The arm: the record is written between naming the waiter and dropping the kernel lock, and
# the release that publishes it has to sit in that same stretch.
record "$SYM_ARM" "$C_CURRENT" "$C_UNLOCK" "the transition that publishes the armed record"
if [ "$REC_RMID" -lt 1 ]; then
    bad "the body of '$SYM_ARM' in $elf publishes NOTHING between naming its waiter and
  dropping the kernel lock ($REC_REL release(s) in the body, $REC_RMID of them in that
  stretch). The line, the waiter, the epoch and the ISR cell it writes are then handed to a
  raiser on another core with nothing ordering them, and that raiser can inject against a
  record it has not seen"
fi

# The park mark: the waiter names itself and only then publishes, and the publication is a
# release. Nothing else in this body is ordered, so the two counts below are the whole of it.
record "$SYM_PARK" "$C_CURRENT" "" "the transition the waiter publishes from its own park"
if [ "$REC_PRE" -lt 1 ]; then
    bad "the body of '$SYM_PARK' in $elf reads the state with NO acquire ahead of the thread it
  compares against ($REC_ACQ acquire(s) in the body, $REC_PRE of them there). It then answers
  on a state the arm on another core may not have published"
fi
if [ "$REC_RMID" -lt 1 ]; then
    bad "the body of '$SYM_PARK' in $elf publishes NOTHING once it has named the parking thread
  ($REC_REL release(s) in the body, $REC_RMID of them past that test). The raiser on another
  core then reads a parked state with nothing pairing against it, or never reads one at all and
  every raise is refused until the retries run out"
fi

# The park mark's SITE, read out of the kernel body that owes it: between taking the lock and
# blocking on it. The acquire of the waiter's own switch count sits in that same stretch, so
# this arm counts the CALL and nothing else.
record "$SYM_WAIT" "$C_LOCK" "$C_BLOCK" "the park the publication has to sit inside" "$C_PARK"
if [ "$REC_MARK" -lt 1 ]; then
    bad "the body of '$SYM_WAIT' in $elf does not reach '$SYM_PARK' between taking the kernel
  lock and blocking on it ($REC_MARK call(s) there). A raiser can then inject against a waiter
  that has not left its core, and the sample is classified on where that waiter happened to be
  rather than where the sweep placed it: no refusal anywhere, the same closed count, and a
  locality split that measures the accident"
fi

# The raise: acquire the armed record BEFORE the stamp, release the stamp BETWEEN the stamp
# and the injection.
record "$SYM_RAISE" "$C_CLOCK" "$C_INJECT" "the transition that opens the span"
if [ "$REC_PRE" -lt 1 ]; then
    bad "the body of '$SYM_RAISE' in $elf carries NO acquire ahead of its opening stamp. It
  reads the armed record with nothing pairing against the arm's release, so the waiter pointer
  and the line it injects into may be a previous sweep's"
fi
if [ "$REC_RMID" -lt 1 ]; then
    bad "the body of '$SYM_RAISE' in $elf carries no release between its opening stamp and the
  injection ($REC_REL release(s) in the body, $REC_RMID of them in that window). The closer on
  another core then subtracts whatever t0 it happens to see, which is the previous pass's
  stamp and reads as a larger, plausible span with no refusal anywhere"
fi

# The close: the state is read between the closing stamp and the identity test, and that read
# is what makes t0 readable.
record "$SYM_CLOSE" "$C_CLOCK" "$C_CURRENT" "the transition that subtracts the span"
if [ "$REC_MID" -lt 1 ]; then
    bad "the body of '$SYM_CLOSE' in $elf reads its state with NO acquire between the closing
  stamp and the identity test ($REC_ACQ acquire(s) in the body, $REC_MID of them there). It
  consumes the state and the stamp the raiser published with nothing pairing against that
  release, so it can classify a span on a stale state or time one against a stale stamp"
fi

if [ "$rc" -ne 0 ]; then
    exit 1
fi

echo "PASS: the arm releases its record under its own lock, the waiter publishes its park from
  between the lock and the block, the raise acquires that and releases its stamp inside the
  window, and the close acquires before it subtracts"
exit 0
