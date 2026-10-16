#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Require the x86_64 interrupt entry to reset RFLAGS, `push $0x2` then `popf`, before it calls C.
#
#   tests/static/check_x86_64_entry_cld.sh <objdump> <cc> <build-dir>
#
# Delivery clears neither DF nor AC (Intel SDM Vol 3 section 7.12.1.3), ring 3 sets both with
# `popf`, the kernel archive carries `rep stos`, and AC set lifts SMAP. A `cld` alone is red. The
# SYSCALL leg is IA32_FMASK's (ring3_x86_64.cc).
#
# A BRANCH IN THE WINDOW WHOSE TARGET LIES PAST THE RESET SKIPS IT, so the reset counts only
# where no earlier branch reaches beyond it; an indirect branch reaches anywhere.
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
N_PUSH='^(push|pushq) [$]0x2$'
N_POPF='^(popf|popfq)$'

# --- the reader ---------------------------------------------------------------
# One record: the body's instruction count, the ordinal of the call that closes the window, and
# the resets standing strictly inside it: a `popf` whose own predecessor is the `push $0x2`,
# with no earlier branch in the window aimed past that push.
# HALF A PROGRAM: `seen`, the body scope and every refusal below come from gate.sh's
# scoped_body, which reads tests/lib/objdump_scope.awk and tests/lib/objdump_window.awk ahead
# of this file.
cat > "$TMP/reader.awk" <<'AWK'
function hexval(t,    i, c, n) {
    n = 0
    for (i = 1; i <= length(t); i++) {
        c = index("0123456789abcdef", substr(t, i, 1))
        if (c == 0) { return -1 }
        n = n * 16 + c - 1
    }
    return n
}
{ here = $0; sub(/^[ \t]*/, "", here); sub(/:.*$/, "", here); addr = hexval(here) }
win_in && win_text ~ /^j[a-z]* / {
    split(win_text, w, " ")
    t = hexval(w[2])
    if (t < 0) { t = 1e18 }
    if (t > reach) { reach = t }
}
win_in && win_text ~ pop_re && after_push && reach <= push_at { clear++ }
{ after_push = (win_in && win_text ~ push_re); if (after_push) { push_at = addr } }
END { printf "WIN %d %d %d\n", win_n, win_close_n + 0, clear + 0 }
AWK

read_entry() { # <listing> <symbol>
    scoped_body "$TMP/reader.awk" "$1" "$2" -v win_close="$C_CALL" -v push_re="$N_PUSH" \
        -v pop_re="$N_POPF"
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
    pushq   $2
    popfq
    call    kos_ctl_target
    popq    %rax
    ret
    .globl kos_ctl_dirty
kos_ctl_dirty:
    pushq   %rax
    call    kos_ctl_target
    popq    %rax
    ret
    .globl kos_ctl_cld
kos_ctl_cld:
    pushq   %rax
    cld
    call    kos_ctl_target
    popq    %rax
    ret
    .globl kos_ctl_keepac
kos_ctl_keepac:
    pushq   %rax
    pushfq
    popfq
    call    kos_ctl_target
    popq    %rax
    ret
    .globl kos_ctl_apart
kos_ctl_apart:
    pushq   $2
    pushq   %rax
    popfq
    call    kos_ctl_target
    popq    %rax
    ret
    .globl kos_ctl_skip
kos_ctl_skip:
    pushq   %rax
    testb   $3, 8(%rsp)
    jz      1f
    pushq   $2
    popfq
1:
    call    kos_ctl_target
    popq    %rax
    ret
    .globl kos_ctl_join
kos_ctl_join:
    pushq   %rax
    testb   $3, 8(%rsp)
    jz      1f
    nop
1:
    pushq   $2
    popfq
    call    kos_ctl_target
    popq    %rax
    ret
    .globl kos_ctl_late
kos_ctl_late:
    pushq   %rax
    call    kos_ctl_target
    pushq   $2
    popfq
    popq    %rax
    ret
    .globl kos_ctl_nocall
kos_ctl_nocall:
    pushq   %rax
    pushq   $2
    popfq
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

expect kos_ctl_clean "WIN 6 4 1" "it cannot recognise the shape this gate requires and every
  verdict below is meaningless"
expect kos_ctl_dirty "WIN 4 2 0" "this gate cannot go red on the defect it exists to catch"
expect kos_ctl_cld "WIN 5 3 0" "a cld alone satisfies the rule, and the C half runs with the
  alignment-check flag ring 3 left, which lifts SMAP"
expect kos_ctl_keepac "WIN 6 4 0" "any popf satisfies the rule, including one that restores the
  interrupted flags"
expect kos_ctl_apart "WIN 6 4 0" "a popf that does not pop the pushed constant satisfies the
  rule"
expect kos_ctl_skip "WIN 8 6 0" "a reset a conditional branch jumps over satisfies the rule, and
  the path that takes the branch reaches C with the interrupted flags"
expect kos_ctl_join "WIN 9 7 1" "a branch landing ahead of the reset, which is the entry's own
  shape, reads as skipping it"
expect kos_ctl_late "WIN 6 2 0" "a reset placed AFTER the call satisfies the rule, and the C
  half runs with the flags the interrupted code left"
expect kos_ctl_nocall "NOCLOSE 5" "a body that reaches no call at all reads as one that reaches
  C without the reset, which is a finding this gate never observed"
expect kos_ctl_fwd "NOCLOSE 1" "a two-instruction forwarder over the real entry reads as the
  entry itself reaching C dirty, and the arch nobody disassembles by hand is reported broken on
  a body this reader never saw"
ctl_dead_reader "$(read_entry "$TMP/ctl.dis" kos_ctl_nothing)" \
    "a renamed or inlined entry would be reported as a missing reset rather than as
  an absence"

echo "== control: the reader separates a reset inside the window from a cld alone, from a popf
   of anything else, from one a branch skips, from one past the call, from a body with no call,
   from a two-instruction forwarder, and from a symbol the object does not carry"

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
            require_number "$clears" "the flags reset count in $ENTRY in $rel"
            echo "   $rel: $ENTRY calls C at instruction #$at of $n, with $clears reset(s)
     ahead of it"
            if [ "$clears" -eq 0 ]; then
                N_DIRTY=$((N_DIRTY + 1))
                bad "the body of '$ENTRY' in $rel reaches its first call at instruction #$at
      with no flags reset before it"
            fi ;;
        *)
            fail "the reader emitted [$rec] for '$ENTRY' in $rel, a record this gate does not
      model" ;;
    esac
done < "$TMP/objects"

if [ "$N_DIRTY" -ne 0 ]; then
    echo "" >&2
    echo "      An interrupt gate leaves DF and AC as the interrupted code set them, popf sets" >&2
    echo "      both from ring 3, the kernel archive carries string operations and AC lifts" >&2
    echo "      SMAP, so restore the push \$2; popfq ahead of the first call in $ENTRY" >&2
    echo "      (arch/x86/x86_64/trap_x86_64.S)." >&2
fi
if [ "$rc" -ne 0 ]; then
    exit 1
fi

echo "PASS: $ENTRY resets RFLAGS (DF and AC clear) before its first call in $N_OBJ object(s)"
