#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The end-to-end span's publication, read out of the LINKED IMAGE. Above one kernel core the
# arm, the park, the raise and the close run on different cores over one state cell, and every
# other cell of the protocol is ordinary data that cell carries: the arm RELEASES the armed
# record, the waiter RELEASES the parked state from inside its own park, the raise ACQUIRES
# that and then RELEASES the opening stamp, and the close ACQUIRES it before it subtracts.
#
# EVERY CLAIM HERE IS POSITIONAL, AND THAT IS NOT PEDANTRY. Each of these bodies reaches some
# OTHER ordered field of the kernel's, the thread's switch count being acquire/release in its
# own right, so a body-wide count of acquires stays at one with the protocol's own acquire
# downgraded to a plain load. The bracket each check reads is the pair of calls the ordering
# has to sit between.
#
# REFUSED: an arm that publishes nothing between naming its waiter and dropping the kernel
# lock, a park mark that publishes nothing once it has named the parking thread, a park mark
# the waiter does not reach between taking the kernel lock and blocking on it, a raise with no
# acquire ahead of its stamp, a raise whose release does not sit between the stamp and the
# injection, a close with no acquire between the closing stamp and the identity test, and a
# body this reader cannot decode.
#
# THE PARK MARK'S PLACEMENT IS THE HALF THAT HOLDS THE MEASUREMENT UP. The raise refuses every
# state but the parked one, so where that mark sits is what decides whether a delivered line
# can reach a thread that is still running. Sited inside the kernel lock the waking post has
# to take, it cannot; sited anywhere past the block, a raise may fire against a waiter that has
# not left its core yet and the LOCALITY SPLIT then follows the accident instead of the sweep's
# placement, with no refusal anywhere and a closed count that does not move. So the claim is
# that one CALL SITE lies between the lock and the block, and not that the call exists.
#
# WHY THIS IS STRUCTURAL AND NOT A RUNTIME BOUND. QEMU's TCG retires one instruction stream at
# a time and models no store buffer, so an image whose every publication is relaxed produces
# the same closed count, the same locality split and the same nanosecond columns on
# qemu-arm64-benchsmp and qemu-riscv64-benchsmp as a correct one. No arm in this tree can go
# red on the defect. So the claim is the INSTRUCTIONS and not the readings.
#
# THE STAMP'S SIDE OF THE RELEASE IS HALF THE CLAIM. A release sited ahead of the clock read
# publishes a stamp that is not yet written, and the closer then subtracts whatever t0 the
# previous pass left: a plausible, larger nanosecond figure and no refusal anywhere.
#
# The per-arch pair is the whole model. AN ARCH THIS FILE DOES NOT CARRY IS A REFUSAL.
#
# usage: check_bench_e2e_publish.sh <elf> <objdump> <arch>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_bench_e2e_publish.sh <elf> <objdump> <arch>"
elf="${1:?$_usage}"
objdump="${2:?$_usage}"
arch="${3:?$_usage}"

# The three bodies, by their MANGLED names: they are C++ and the scope reader keys on the name
# objdump prints.
SYM_ARM=_ZN6kickos13bench_e2e_armEi
SYM_PARK=_ZN6kickos19bench_e2e_park_markEv
SYM_RAISE=_ZN6kickos15bench_e2e_raiseEv
SYM_CLOSE=_ZN6kickos15bench_e2e_closeEv
# The kernel body the park mark has to sit inside, and not a bench one. It is the TIMED body
# and not the entry: kos_irq_wait's untimed form is a forwarder, and a forwarder takes no lock
# and blocks on nothing, so a gate aimed at it reads a clean two-instruction body and reports
# the park mark missing when it is the reader that cannot see it.
SYM_WAIT=_ZN6kickos14irq_wait_timedEPNS_6ThreadEjj

# The calls that bracket each body's ordering. Mangled for the same reason.
C_CURRENT='<_ZN6kickos5sched7currentEv>'
C_UNLOCK='<_ZN6kickos11klock_leaveEv>'
C_CLOCK='<arch_clock_now>'
C_INJECT='<arch_irq_inject>'
C_LOCK='<_ZN6kickos11klock_enterEv>'
C_BLOCK='<_ZN6kickos8wq_blockERNS_4ListENS_8WaitKindEPv>'
C_PARK='<_ZN6kickos19bench_e2e_park_markEv>'

# ACQ/REL: the mnemonic, and the operand where the mnemonic alone does not say the direction.
# An empty operand means the mnemonic decides. PLAIN_*: the same access with no ordering, for
# the planted control that proves the reader reports the downgrade.
case "$arch" in
    armv8a)
        ACQ_MNEM=ldar; ACQ_OPS=""
        REL_MNEM=stlr; REL_OPS=""
        PLAIN_LOAD='ldr	w0, [x19]'
        PLAIN_STORE='str	w0, [x19]'
        ACQ_INSN='ldar	w0, [x19]'
        REL_INSN='stlr	w0, [x19]'
        CALL_ONE='bl	ffffff804001b400 <arch_clock_now>'
        CALL_TWO='bl	ffffff804001b2c8 <arch_irq_inject>'
        CALL_MARK='bl	ffffff8040017b1c <_ZN6kickos19bench_e2e_park_markEv>'
        RET_INSN='ret'
        ;;
    rv64imac)
        # The fence and the access are separate instructions here, so the OPERAND is the whole
        # direction: `rw,rw` is a full barrier and neither half of this pairing.
        ACQ_MNEM=fence; ACQ_OPS='r,rw'
        REL_MNEM=fence; REL_OPS='rw,w'
        PLAIN_LOAD='lw	a5,0(s0)'
        PLAIN_STORE='sw	a5,0(s0)'
        ACQ_INSN='fence	r,rw'
        REL_INSN='fence	rw,w'
        CALL_ONE='jal	ffffffff80012070 <arch_clock_now>'
        CALL_TWO='jal	ffffffff8001077c <arch_irq_inject>'
        CALL_MARK='jal	ffffffff8000fe36 <_ZN6kickos19bench_e2e_park_markEv>'
        RET_INSN='ret'
        ;;
    *)
        fail "arch '$arch' is built above one kernel core with KICKOS_BENCH and this gate
  carries no model of its acquire and release instructions, so nothing here can say whether
  the end-to-end protocol publishes anything. Add the arch's pair to the table in this file" ;;
esac

[ -f "$elf" ] || fail "no image at $elf"
[ -x "$objdump" ] || fail "no objdump at $objdump; there is no instruction stream to decode"

scratch_dir

# --- the reader ---------------------------------------------------------------
# One record per body: the instruction count, the acquires and releases in it, and then the
# four positional counts: acquires ahead of the OPENING call, acquires and releases between the
# opening call and the CLOSING one, and calls to a NAMED callee in that same stretch. A body
# whose bracket calls are not given reads the positional counts as zero; one given no closing
# call brackets from the opening call to the end of the body.
# HALF A PROGRAM: `seen` and the body scope come from gate.sh's scoped_body, which reads
# tests/lib/objdump_scope.awk ahead of this file.
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
    n++
    if (mnem == acq_mnem && (acq_ops == "" || ops == acq_ops))
    {
        acq++
        if (opened == 0) { acq_pre++ }
        else if (closed == 0) { acq_mid++ }
    }
    if (mnem == rel_mnem && (rel_ops == "" || ops == rel_ops))
    {
        rel++
        if (opened != 0 && closed == 0) { rel_mid++ }
    }
    if (call_mark != "" && index(text, call_mark) > 0 && opened != 0 && closed == 0)
    {
        mark_mid++
    }
    if (call_open != "" && index(text, call_open) > 0 && opened == 0) { opened = n }
    if (call_close != "" && index(text, call_close) > 0 && closed == 0 && opened != 0)
    {
        closed = n
    }
}
END {
    if (!seen) { print "NOSYM"; exit }
    if (n == 0) { print "NOINSN"; exit }
    printf "BODY %d %d %d %d %d %d %d\n", n, acq + 0, rel + 0, acq_pre + 0, acq_mid + 0,
           rel_mid + 0, mark_mid + 0
}
AWK

read_body() { # <listing> <symbol> <opening-call> <closing-call> [<counted-call>]
    scoped_body "$TMP/reader.awk" "$1" "$2" \
        -v acq_mnem="$ACQ_MNEM" -v acq_ops="$ACQ_OPS" \
        -v rel_mnem="$REL_MNEM" -v rel_ops="$REL_OPS" \
        -v call_open="$3" -v call_close="$4" -v call_mark="${5:-}"
}

# --- the reader's controls, before the image is read --------------------------
# Planted listings in the shape the invocation below produces, which is --no-show-raw-insn.
plant() { # <file> <insn>...
    _f="$1"
    shift
    _a=4096
    echo "0000000000001000 <planted_body>:" > "$_f"
    for _i in "$@"; do
        printf '    %x:\t%s\n' "$_a" "$_i" >> "$_f"
        _a=$((_a + 4))
    done
}

ctl() { # <file> <expected-record> <prose>
    _got="$(read_body "$1" planted_body "$C_CLOCK" "$C_INJECT")"
    [ "$_got" = "$2" ] || fail "the reader answered [$_got] and not [$2] for $3"
}

# The publishing shape: acquire, then the opening call, then the release, then the closing one.
plant "$TMP/ctl_pub" "$ACQ_INSN" "$PLAIN_LOAD" "$CALL_ONE" "$PLAIN_STORE" "$REL_INSN" \
    "$CALL_TWO" "$RET_INSN"
ctl "$TMP/ctl_pub" "BODY 7 1 1 1 0 1 0" "a planted body that acquires, reads its stamp,
  releases and then injects, so it cannot recognise the shape this gate requires and every
  verdict below is meaningless"

# The consuming shape: the acquire sits INSIDE the bracket, which is what the close owes.
plant "$TMP/ctl_mid" "$PLAIN_LOAD" "$PLAIN_LOAD" "$CALL_ONE" "$ACQ_INSN" "$REL_INSN" \
    "$CALL_TWO" "$RET_INSN"
ctl "$TMP/ctl_mid" "BODY 7 1 1 0 1 1 0" "a planted body whose acquire sits between the two
  calls, which is the close's shape and would otherwise never be read"

# The downgrade: the same body with both orderings dropped to plain accesses.
plant "$TMP/ctl_relaxed" "$PLAIN_LOAD" "$PLAIN_LOAD" "$CALL_ONE" "$PLAIN_STORE" \
    "$PLAIN_STORE" "$CALL_TWO" "$RET_INSN"
ctl "$TMP/ctl_relaxed" "BODY 7 0 0 0 0 0 0" "a planted body whose acquire and release were
  dropped to plain accesses. That is the defect this gate exists to catch, so a reader that
  does not report it cannot go red"

# The ordering present but OUTSIDE the bracket: an acquire and a release the body carries for
# some other field entirely.
plant "$TMP/ctl_outside" "$ACQ_INSN" "$REL_INSN" "$PLAIN_STORE" "$CALL_ONE" "$PLAIN_STORE" \
    "$CALL_TWO" "$RET_INSN"
ctl "$TMP/ctl_outside" "BODY 7 1 1 1 0 0 0" "a planted body whose release sits AHEAD of the
  stamp it is meant to publish. The release then hands over a t0 the closer reads from the
  previous pass, and a reader that does not separate the two proves nothing"

# The open-ended bracket, which is the park mark's shape: no closing call is given, so the
# window runs from the identity test to the end of the body.
plant "$TMP/ctl_tail" "$ACQ_INSN" "$CALL_ONE" "$PLAIN_STORE" "$REL_INSN" "$RET_INSN"
_got="$(read_body "$TMP/ctl_tail" planted_body "$C_CLOCK" "")"
[ "$_got" = "BODY 5 1 1 1 0 1 0" ] || fail "the reader answered [$_got] and not [BODY 5 1 1 1 0
  1 0] for a planted body bracketed from its opening call to its end, which is how the park
  mark is read"

# The counted call, INSIDE the bracket and then on either side of it.
ctl_mark() { # <file> <expected-record> <prose>
    _got="$(read_body "$1" planted_body "$C_CLOCK" "$C_INJECT" "$C_PARK")"
    [ "$_got" = "$2" ] || fail "the reader answered [$_got] and not [$2] for $3"
}

plant "$TMP/ctl_call_in" "$CALL_ONE" "$CALL_MARK" "$CALL_TWO" "$RET_INSN"
ctl_mark "$TMP/ctl_call_in" "BODY 4 0 0 0 0 0 1" "a planted body whose counted call sits
  between the two bracketing ones, which is the only placement the park arm accepts"

plant "$TMP/ctl_call_pre" "$CALL_MARK" "$CALL_ONE" "$CALL_TWO" "$RET_INSN"
ctl_mark "$TMP/ctl_call_pre" "BODY 4 0 0 0 0 0 0" "a planted body whose counted call sits
  AHEAD of the bracket. A reader that counted it would pass a publication made before the lock
  the waking post has to take"

plant "$TMP/ctl_call_post" "$CALL_ONE" "$CALL_TWO" "$CALL_MARK" "$RET_INSN"
ctl_mark "$TMP/ctl_call_post" "BODY 4 0 0 0 0 0 0" "a planted body whose counted call sits PAST
  the bracket. That is the defect this arm exists for: a mark published after the block cannot
  hold a raise off a waiter that is still running"

ctl_dead_reader "$(read_body "$TMP/ctl_pub" a_symbol_no_listing_carries "$C_CLOCK" "$C_INJECT")" \
    "a renamed or inlined body would read as a clean one"

echo "== control: the reader reports the publishing shape, the consuming shape, the relaxed
   downgrade, ordering sited outside the bracket that owes it, an open-ended bracket, and a
   counted call inside that bracket and on either side of it"

# --- the instruction stream ---------------------------------------------------
tool_out "$TMP/dis" "^[0-9a-f]+ <.*>:\$" "$objdump" -d --no-show-raw-insn "$elf"
require_nonempty "$TMP/dis" "$objdump printed no disassembly for $elf"

field() { printf '%s\n' "$1" | cut -d' ' -f"$2"; }

record() { # <symbol> <opening-call> <closing-call> <what it carries> [<counted-call>]
    _r="$(read_body "$TMP/dis" "$1" "$2" "$3" "${5:-}")"
    case "$_r" in
        NOSYM)
            fail "the disassembly of $elf carries no body for '$1', which is $4. It was
  renamed, inlined or compiled out, and a reader that started nowhere would otherwise report
  a clean body" ;;
        NOINSN)
            fail "the body of '$1' in $elf disassembles to no instruction at all, so the
  corpus is UNKNOWN rather than empty" ;;
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
    echo "   $1: $REC_N instruction(s); acquires $REC_ACQ ($REC_PRE before / $REC_MID inside
     the bracket), releases $REC_REL ($REC_RMID inside), counted calls $REC_MARK"
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
