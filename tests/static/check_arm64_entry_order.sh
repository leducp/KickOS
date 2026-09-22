#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Three orderings on the arm64 entry and timer paths, read out of the LINKED IMAGE. Each is one
# instruction that must stand between two others, and each defect is INVISIBLE to a running arm:
#
#   _start                        `msr SPSel, #1` before the first write to SP. Entered with
#                                 SPSel 0, `mov sp` loads SP_EL0 and leaves SP_ELx as reset left
#                                 it, so selecting SP_ELx afterwards hands the EL3 and EL2
#                                 refusal paths a stack pointer nothing wrote (DDI 0487 M.b,
#                                 SPSel and the AArch64 stack pointer selection). Reset sets
#                                 PSTATE.SP to 1, so on a machine that starts this image at reset
#                                 the wrong order costs nothing; an image entered from firmware
#                                 that left SPSel 0 takes the fault.
#
#   arch_timer_disarm             ISB between `msr CNTP_CTL_EL0` and the Device write that clears
#                                 the GIC's pending state. The disable governs the timer's output
#                                 only past a Context synchronization event, and nothing orders a
#                                 system-register write against a subsequent Device store, so a
#                                 level still asserted re-pends the line behind the clear and the
#                                 disarm does not mean no callback.
#
#   kickos_armv8a_percore_init    the same ISB, before the GIC enables this core's timer PPI. The
#                                 write it follows exists because CNTP_CTL_EL0's reset value is
#                                 architecturally UNKNOWN, so the case it covers is exactly an
#                                 output already asserted.
#
# NONE OF THE THREE IS WITNESSABLE BY RUNNING THE IMAGE. QEMU enters at reset with PSTATE.SP
# already 1, and its timer model deasserts on the register write with no pipeline to drain, so an
# image carrying neither the SPSel order nor either barrier boots green on every arm64 machine in
# the fleet. The effects rest on the architecture, which is why the assertion is structural.
#
# The reader goes through planted listings for every verdict it can emit, the out-of-order one
# included: a reader that cannot report the defect cannot go red.
#
# AN EMPTY WINDOW IS A FAILURE, not a pass. A body whose opening instruction is absent, or whose
# window never closes, says the symbol moved or the code shape changed, and that is UNKNOWN.
# Those three refusals are tests/lib/objdump_window.awk's, shared with every other landmark
# reader in this tree rather than invented here.
#
# usage: check_arm64_entry_order.sh <elf> <nm> <objdump>

set -eu
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_arm64_entry_order.sh <elf> <nm> <objdump>"
elf="${1:?$_usage}"
nm="${2:?$_usage}"
objdump="${3:?$_usage}"

# A defined-symbol count below this says nm was read wrong, whatever it printed.
SYM_FLOOR=100

[ -f "$elf" ] || fail "no image at $elf"
[ -x "$nm" ] || fail "no nm at $nm; the symbol table cannot be read out of the image and every
  assertion below would rest on a hard-coded layout"
[ -x "$objdump" ] || fail "no objdump at $objdump; there is no instruction stream to decode"

scratch_dir

# --- the reader ---------------------------------------------------------------
# Ordinals, not addresses: every verdict is an ORDER. Emits exactly one record.
#
# The window's own two landmarks are the shared ones: win_open is the instruction the window
# starts after, empty for the body's first, which is how a rule of the form "before the first X"
# is spelled; win_close is the instruction whose effect the need protects. THE NEED IS SEARCHED
# OVER THE WHOLE BODY PAST THE OPEN AND NOT INSIDE THE WINDOW, because a need standing past the
# close is the DEFECT this gate reports and not an absence: restricted to the window it would
# come back as NONEED, and a barrier in the wrong place would be filed as a barrier deleted.
#
# Matched against the instruction TEXT and not the mnemonic alone: `mov sp, x0` is a write to SP
# and `mov x0, sp` is not, and a mnemonic-only reader cannot tell them apart.
# HALF A PROGRAM: `seen`, the body scope, win_text and every refusal but NONEED come from
# gate.sh's scoped_body, which reads tests/lib/objdump_scope.awk and
# tests/lib/objdump_window.awk ahead of this file.
cat > "$TMP/reader.awk" <<'AWK'
win_n > win_open_n && need_at == 0 && win_text ~ needre { need_at = win_n }
END {
    if (need_at == 0) { print "NONEED " win_close_n " " win_n; exit }
    print "ORDER " need_at " " win_close_n " " win_n
}
AWK

read_body() { # <listing> <symbol> <open-ere> <need-ere> <close-ere>
    scoped_body "$TMP/reader.awk" "$1" "$2" \
        -v win_open="$3" -v needre="$4" -v win_close="$5"
}

# --- the reader's controls, before the image is read --------------------------
# Planted listings in the shape the invocation below produces, which is --no-show-raw-insn: a
# control carrying the raw-bytes column would read its first byte group as the mnemonic and prove
# the reader against input the gate never hands it.
cat > "$TMP/ctl_ok" <<'EOF'
0000000000001000 <planted_body>:
    1000:	mov	x0, #0x0
    1004:	msr	cntp_ctl_el0, x0
    1008:	isb
    100c:	mov	w0, #0x1e
    1010:	b	1200 <planted_callee>
EOF
cat > "$TMP/ctl_bad" <<'EOF'
0000000000001000 <planted_body>:
    1000:	mov	x0, #0x0
    1004:	msr	cntp_ctl_el0, x0
    1008:	b	1200 <planted_callee>
    100c:	isb
EOF
cat > "$TMP/ctl_noneed" <<'EOF'
0000000000001000 <planted_body>:
    1000:	mov	x0, #0x0
    1004:	msr	cntp_ctl_el0, x0
    1008:	b	1200 <planted_callee>
EOF
cat > "$TMP/ctl_noopen" <<'EOF'
0000000000001000 <planted_body>:
    1000:	isb
    1004:	b	1200 <planted_callee>
EOF
cat > "$TMP/ctl_noclose" <<'EOF'
0000000000001000 <planted_body>:
    1000:	msr	cntp_ctl_el0, x0
    1004:	isb
    1008:	nop
EOF
# The forwarder: neither landmark, which is one record and not two absences.
cat > "$TMP/ctl_nobracket" <<'EOF'
0000000000001000 <planted_body>:
    1000:	mov	x0, x19
    1004:	nop
EOF
# The close standing only AHEAD of the open, so the window would run to the end of the body.
cat > "$TMP/ctl_closefirst" <<'EOF'
0000000000001000 <planted_body>:
    1000:	b	1200 <planted_callee>
    1004:	msr	cntp_ctl_el0, x0
    1008:	isb
EOF
# The SP rule's own shape: no opening instruction, and the closing one distinguished by its
# DESTINATION. The second listing is the defect, `mov sp` standing ahead of the select.
cat > "$TMP/ctl_sp_ok" <<'EOF'
0000000000001000 <planted_body>:
    1000:	ldr	x9, 1100 <planted_body+0x100>
    1004:	msr	spsel, #0x1
    1008:	isb
    100c:	mov	sp, x0
EOF
cat > "$TMP/ctl_sp_bad" <<'EOF'
0000000000001000 <planted_body>:
    1000:	mov	x0, sp
    1004:	mov	sp, x0
    1008:	msr	spsel, #0x1
EOF

O_TIMER='^msr cntp_ctl_el0,'
N_ISB='^isb'
C_DEVICE='^(b|bl|blr|br|str|strb|strh|stp|stlr|stlrb|stlrh)( |$)'
N_SPSEL='^msr spsel,'
C_SPWRITE='^(mov|add|sub|and|orr|eor|mvn) sp,'

expect() { # <label> <record> <wanted> <what a wrong answer means>
    if [ "$2" != "$3" ]; then
        fail "the reader answered [$2] rather than [$3] for the planted $1, so $4"
    fi
}

expect "in-order body" \
    "$(read_body "$TMP/ctl_ok" planted_body "$O_TIMER" "$N_ISB" "$C_DEVICE")" \
    "ORDER 3 5 5" \
    "it cannot recognise the shape this gate requires and every verdict below is meaningless"

expect "out-of-order body" \
    "$(read_body "$TMP/ctl_bad" planted_body "$O_TIMER" "$N_ISB" "$C_DEVICE")" \
    "ORDER 4 3 4" \
    "the defect this gate exists to catch goes unreported and it cannot go red"

expect "body with no barrier at all" \
    "$(read_body "$TMP/ctl_noneed" planted_body "$O_TIMER" "$N_ISB" "$C_DEVICE")" \
    "NONEED 3 3" \
    "an image whose barrier was deleted would not be reported as such"

expect "body whose window never opens" \
    "$(read_body "$TMP/ctl_noopen" planted_body "$O_TIMER" "$N_ISB" "$C_DEVICE")" \
    "NOOPEN 2" \
    "a body that stopped writing the register this ordering is about would read as clean"

expect "body whose window never closes" \
    "$(read_body "$TMP/ctl_noclose" planted_body "$O_TIMER" "$N_ISB" "$C_DEVICE")" \
    "NOCLOSE 3" \
    "a body that no longer reaches the write the barrier protects would read as clean"

expect "two-instruction forwarder carrying neither landmark" \
    "$(read_body "$TMP/ctl_nobracket" planted_body "$O_TIMER" "$N_ISB" "$C_DEVICE")" \
    "NOBRACKET 2" \
    "a body moved behind a forwarder would be reported as one missing its barrier"

expect "body whose closing instruction stands only AHEAD of the opening one" \
    "$(read_body "$TMP/ctl_closefirst" planted_body "$O_TIMER" "$N_ISB" "$C_DEVICE")" \
    "NOCLOSE 3" \
    "the window would run to the end of the body and count a barrier the rule does not reach"

expect "renamed body" \
    "$(read_body "$TMP/ctl_ok" a_symbol_no_listing_carries "$O_TIMER" "$N_ISB" "$C_DEVICE")" \
    "NOSYM" \
    "a renamed body would read as a clean one"

expect "in-order SP select" \
    "$(read_body "$TMP/ctl_sp_ok" planted_body "" "$N_SPSEL" "$C_SPWRITE")" \
    "ORDER 2 4 4" \
    "the SP rule's shape is not recognised and its verdict below is meaningless"

expect "SP written before the select" \
    "$(read_body "$TMP/ctl_sp_bad" planted_body "" "$N_SPSEL" "$C_SPWRITE")" \
    "ORDER 3 2 3" \
    "a body writing SP ahead of the select would not be reported, which is the defect"

echo "== three orderings in $elf =="

# --- the symbol table ---------------------------------------------------------
tool_out "$TMP/nm" "[0-9a-fA-F]" "$nm" --defined-only "$elf"
require_nonempty "$TMP/nm" "$nm printed no symbol at all for $elf, so the corpus is UNKNOWN
  rather than empty and every verdict below it would be vacuous"
syms="$(wc -l < "$TMP/nm" | tr -d ' ')"
require_number "$syms" "the defined-symbol count"
if [ "$syms" -lt "$SYM_FLOOR" ]; then
    fail "$nm reports $syms defined symbol(s) in $elf, below the floor of $SYM_FLOOR. A table
  that short is a misread, not a small image, and the corpus is UNKNOWN"
fi

# --- the instruction stream ---------------------------------------------------
tool_out "$TMP/dis" "^[0-9a-f]+ <.*>:\$" "$objdump" -d --no-show-raw-insn "$elf"
require_nonempty "$TMP/dis" "$objdump printed no disassembly for $elf"

# `_start` is an assembly label carrying no .size, so presence in the symbol table is all the
# table can say about it and the body comes from the listing.
have_sym() { # <symbol>
    awk -v s="$1" '
        NF == 3 && $3 == s { found = 1 }
        END { if (found) { exit 0 } ; exit 1 }' "$TMP/nm"
}

# <symbol> <open> <need> <close> <what the need is> <what the close is> <why it is owed>
judge() {
    _sym="$1"
    _open="$2"
    _need="$3"
    _close="$4"
    _needwhat="$5"
    _closewhat="$6"
    _why="$7"

    have_sym "$_sym" || fail "no defined symbol '$_sym' in $elf. It was renamed, made static or
  inlined away, so this gate has no body to read and reports an absence it cannot tell apart
  from a failure to read"

    _rec="$(read_body "$TMP/dis" "$_sym" "$_open" "$_need" "$_close")"
    _kind="${_rec%% *}"
    _f2="$(printf '%s\n' "$_rec" | cut -s -d' ' -f2)"
    _f3="$(printf '%s\n' "$_rec" | cut -s -d' ' -f3)"
    _f4="$(printf '%s\n' "$_rec" | cut -s -d' ' -f4)"
    case "$_kind" in
        NOSYM)
            fail "'$_sym' is in the symbol table of $elf but the disassembly carries no body for
  it, so the reader started nowhere. The disassembler's output shape has moved" ;;
        NOINSN)
            fail "the body of '$_sym' in $elf disassembles to no instruction at all, so the
  corpus is UNKNOWN rather than empty" ;;
        NOBRACKET)
            fail "across the $_f2 instruction(s) of '$_sym' in $elf nothing matches /$_open/ and
  nothing matches /$_close/, so this gate can see neither end of the window the ordering sits
  in. That is UNKNOWN, not a pass: the symbol names a forwarder over the real body, or both
  landmarks moved at once" ;;
        NOOPEN)
            fail "across the $_f2 instruction(s) of '$_sym' in $elf nothing matches /$_open/, so
  the window this ordering is about never opens. That is UNKNOWN, not a pass: the body no longer
  has the shape the rule is stated over" ;;
        NOCLOSE)
            fail "across the $_f2 instruction(s) of '$_sym' in $elf nothing matches /$_close/
  past the opening instruction, so the body never reaches $_closewhat and there is nothing to
  order $_needwhat against. That is UNKNOWN, not a pass" ;;
        NONEED)
            fail "the body of '$_sym' in $elf reaches $_closewhat at instruction #$_f2 with NO
  $_needwhat before it across $_f3 instruction(s). $_why" ;;
        ORDER) ;;
        *)
            fail "the reader emitted [$_rec] for '$_sym', a record this gate does not model" ;;
    esac

    require_number "$_f2" "the ordinal of $_needwhat in $_sym"
    require_number "$_f3" "the ordinal of $_closewhat in $_sym"
    require_number "$_f4" "the instruction count of $_sym"
    if [ "$_f2" -ge "$_f3" ]; then
        awk -v sym="$_sym" '
            /^[0-9a-f]+ <.*>:$/ { name = $2; gsub(/[<>:]/, "", name); f = (name == sym); next }
            f && /^$/ { exit }
            f { print "      " $0 }' "$TMP/dis" >&2
        fail "in '$_sym' the $_needwhat is instruction #$_f2 and $_closewhat is #$_f3, so it does
  NOT stand between them. $_why"
    fi
    echo "   $_sym: $_needwhat at #$_f2, ahead of $_closewhat at #$_f3, in $_f4 instruction(s)"
}

judge _start "" "$N_SPSEL" "$C_SPWRITE" \
    "SPSel select" "the first write to SP" \
    "A boot entered with SPSel 0 then writes SP_EL0 and runs the EL3 and EL2 refusal paths on an
  SP_ELx nothing initialised"

judge arch_timer_disarm "$O_TIMER" "$N_ISB" "$C_DEVICE" \
    "ISB" "the pending-state clear" \
    "The timer's output is not guaranteed deasserted when the clear is written, so a still
  asserted level re-pends the line and the disarm does not mean no callback"

judge kickos_armv8a_percore_init "$O_TIMER" "$N_ISB" "$C_DEVICE" \
    "ISB" "the GIC's per-core init" \
    "CNTP_CTL_EL0's reset value is architecturally UNKNOWN, so the disable this barrier follows
  exists precisely for an output already asserted when the PPI is enabled"

echo "PASS: in $elf the SPSel select precedes the first write to SP, and the ISB after each
  CNTP_CTL_EL0 disable precedes the Device write it protects, in both bodies that write it"
exit 0
