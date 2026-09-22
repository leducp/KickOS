#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Require the direction flag to be cleared in the x86_64 interrupt entry before it calls C.
#
#   tests/static/check_x86_64_entry_cld.sh <objdump> <cc> <build-dir>
#
# Delivery through an interrupt or trap gate clears TF, NT and RF, and an interrupt gate also
# clears IF (Intel SDM Vol 3 section 7.12.1.3; AMD APM Vol 2 section 8.9.2). The direction flag
# is on neither list, so it arrives holding whatever the interrupted code left in it, and `std`
# is unprivileged. The C half of the entry is compiled to the SysV convention, which the
# compiler is free to lower into a string operation: this build's own kernel archive carries ten
# `rep stos`.
#
# The SYSCALL leg is covered by IA32_FMASK: arch/x86/x86_64/ring3_x86_64.cc puts bit 10 in that
# mask, so the flag is already clear when that entry's first instruction runs (AMD APM Vol 3,
# SYSCALL).
#
# THE WINDOW IS FROM THE BODY'S FIRST INSTRUCTION TO ITS FIRST CALL, and a body that reaches no
# call is UNKNOWN rather than a body that reaches C dirty. The refusal comes from
# tests/lib/objdump_window.awk so it cannot be forgotten here.
#
# A TAIL JUMP INTO C IS NOT MODELLED: the closing landmark is a call, because a `jmp` to a label
# inside this body would close the window early and manufacture a finding. An entry rewritten to
# reach C by jump therefore reads as NOCLOSE, which is the refusal and not a pass.

set -u
. "$(dirname "$0")/../lib/gate.sh"

# Against the one trap_x86_64.S object this board's libraries produce. An unbuilt tree and a
# walk that found the wrong directory are what this refuses.
OBJ_FLOOR=1

if [ "$#" -ne 3 ]; then
    fail "usage: check_x86_64_entry_cld.sh <objdump> <cc> <build-dir>"
fi
OBJDUMP="$1"
CC="$2"
BUILD="${3%/}"

command -v "$OBJDUMP" >/dev/null 2>&1 || [ -x "$OBJDUMP" ] || fail "no objdump at $OBJDUMP"
command -v "$CC" >/dev/null 2>&1 || [ -x "$CC" ] || fail "no compiler at $CC"
[ -d "$BUILD" ] || fail "no build directory at $BUILD"

scratch_dir

ENTRY=kickos_x86_64_trap_common
# The closing landmark and what must stand inside the window, as EREs over the normalised
# instruction text tests/lib/objdump_window.awk matches against.
C_CALL='^(call|callq)( |$)'
N_CLD='^cld$'

# --- the reader ---------------------------------------------------------------
# One record: the body's instruction count, the ordinal of the call that closes the window, and
# the direction-flag clears standing strictly inside it.
# HALF A PROGRAM: `seen`, the body scope and every refusal below come from gate.sh's
# scoped_body, which reads tests/lib/objdump_scope.awk and tests/lib/objdump_window.awk ahead
# of this file.
cat > "$TMP/reader.awk" <<'AWK'
win_in && win_text ~ clear_re { clear++ }
END { printf "WIN %d %d %d\n", win_n, win_close_n + 0, clear + 0 }
AWK

read_entry() { # <listing> <symbol>
    scoped_body "$TMP/reader.awk" "$1" "$2" -v win_close="$C_CALL" -v clear_re="$N_CLD"
}

# The listing of one object, refusing a disassembler that printed nothing.
listing() { # <object> <outfile>
    tool_out "$2" "^[0-9a-f]+ <.*>:\$" "$OBJDUMP" -d --no-show-raw-insn "$1"
}

# --- the detector, before it is asked to report a presence --------------------
# A reader that answered "cleared" for everything would pass this gate over any object at all,
# so every verdict is produced from purpose-built text first, assembled by the real compiler
# rather than planted, because the mnemonic spelling this keys on is the assembler's.
cat > "$TMP/ctl.s" <<'EOF'
    .text
    .globl kos_ctl_clean
kos_ctl_clean:
    pushq   %rax
    cld
    call    kos_ctl_target
    popq    %rax
    ret
    .globl kos_ctl_dirty
kos_ctl_dirty:
    pushq   %rax
    call    kos_ctl_target
    popq    %rax
    ret
    .globl kos_ctl_late
kos_ctl_late:
    pushq   %rax
    call    kos_ctl_target
    cld
    popq    %rax
    ret
    .globl kos_ctl_nocall
kos_ctl_nocall:
    pushq   %rax
    cld
    popq    %rax
    ret
    .globl kos_ctl_fwd
kos_ctl_fwd:
    jmp     kos_ctl_target
    .globl kos_ctl_target
kos_ctl_target:
    ret
EOF
"$CC" -c -o "$TMP/ctl.o" "$TMP/ctl.s" || fail "$CC could not assemble $TMP/ctl.s"
listing "$TMP/ctl.o" "$TMP/ctl.dis"

expect() { # <symbol> <wanted> <what a wrong answer means>
    _got="$(read_entry "$TMP/ctl.dis" "$1")"
    [ "$_got" = "$2" ] || fail "the reader answered [$_got] and not [$2] for $1, so $3"
}

expect kos_ctl_clean "WIN 5 3 1" "it cannot recognise the shape this gate requires and every
  verdict below is meaningless"
expect kos_ctl_dirty "WIN 4 2 0" "this gate cannot go red on the defect it exists to catch"
expect kos_ctl_late "WIN 5 2 0" "a clear placed AFTER the call satisfies the rule, and the C
  half runs with the flag the interrupted code left"
expect kos_ctl_nocall "NOCLOSE 4" "a body that reaches no call at all reads as one that reaches
  C without clearing the flag, which is a finding this gate never observed"
expect kos_ctl_fwd "NOCLOSE 1" "a two-instruction forwarder over the real entry reads as the
  entry itself reaching C dirty, and the arch nobody disassembles by hand is reported broken on
  a body this reader never saw"
ctl_dead_reader "$(read_entry "$TMP/ctl.dis" kos_ctl_nothing)" \
    "a renamed or inlined entry would be reported as a direction-flag violation rather than as
  an absence"

echo "== control: the reader separates a clear inside the window from one past it, from a body
   with no call, from a two-instruction forwarder, and from a symbol the object does not carry"

# --- the corpus ---------------------------------------------------------------
find "$BUILD" -path '*.dir/*' -name 'trap_x86_64.S*' \( -name '*.o' -o -name '*.obj' \) \
     -type f | sort > "$TMP/objects" || fail "the object walk of $BUILD failed"
N_OBJ="$(wc -l < "$TMP/objects" | tr -d ' ')"
[ "$N_OBJ" -ge "$OBJ_FLOOR" ] \
    || fail "$N_OBJ assembled trap_x86_64.S object(s) under $BUILD, beneath the floor of
      $OBJ_FLOOR: this tree is unbuilt, or the walk found the wrong directory."

rc=0
N_DIRTY=0
while IFS= read -r obj; do
    "$OBJDUMP" -f "$obj" 2>/dev/null | grep -q '^architecture: i386:x86-64' \
        || fail "$OBJDUMP reports no i386:x86-64 architecture for $obj, so the read of that
      object took a dead tool for a clean answer"
    rel="${obj#"$BUILD"/}"
    listing "$obj" "$TMP/dis"
    rec="$(read_entry "$TMP/dis" "$ENTRY")"
    case "$rec" in
        NOSYM)
            bad "$rel carries no body for '$ENTRY'. It was renamed, made local or inlined away,
      so what this gate has is UNKNOWN and not an entry that reaches C dirty" ;;
        NOINSN)
            bad "the body of '$ENTRY' in $rel disassembles to no instruction at all, so the
      corpus is UNKNOWN rather than empty" ;;
        "NOCLOSE "*)
            bad "the body of '$ENTRY' in $rel reaches no call across its
      $(printf '%s\n' "$rec" | cut -d' ' -f2) instruction(s), so the window this rule is stated
      over never closes. That is UNKNOWN and not a pass: the entry now reaches C by some route
      this gate carries no model of, or the symbol names a forwarder over the real body" ;;
        "WIN "*)
            n="$(printf '%s\n' "$rec" | cut -d' ' -f2)"
            at="$(printf '%s\n' "$rec" | cut -d' ' -f3)"
            clears="$(printf '%s\n' "$rec" | cut -d' ' -f4)"
            require_number "$n" "the instruction count of $ENTRY in $rel"
            require_number "$at" "the ordinal of the first call in $ENTRY in $rel"
            require_number "$clears" "the direction-flag clear count in $ENTRY in $rel"
            echo "   $rel: $ENTRY calls C at instruction #$at of $n, with $clears clear(s)
     ahead of it"
            if [ "$clears" -eq 0 ]; then
                N_DIRTY=$((N_DIRTY + 1))
                bad "the body of '$ENTRY' in $rel reaches its first call at instruction #$at
      with no cld before it"
            fi ;;
        *)
            fail "the reader emitted [$rec] for '$ENTRY' in $rel, a record this gate does not
      model" ;;
    esac
done < "$TMP/objects"

if [ "$N_DIRTY" -ne 0 ]; then
    echo "" >&2
    echo "      An interrupt gate leaves the direction flag as the interrupted code set it," >&2
    echo "      the instruction that sets it is unprivileged, and the kernel archive on this" >&2
    echo "      board carries string operations, so restore the cld ahead of the first call" >&2
    echo "      in $ENTRY (arch/x86/x86_64/trap_x86_64.S)." >&2
fi
if [ "$rc" -ne 0 ]; then
    exit 1
fi

echo "PASS: $ENTRY clears the direction flag before its first call in $N_OBJ object(s)"
