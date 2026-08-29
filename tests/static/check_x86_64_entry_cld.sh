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

set -u
. "$(dirname "$0")/../lib/gate.sh"

# The parses below read objdump's own output back, and this box's binutils is localised.
LC_ALL=C
export LC_ALL

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

# Print `clear`, `call` or `none` for the window between <symbol> and the first call in its
# disassembly: whether a cld stands in it, whether the window closed on a call with no cld,
# or whether the symbol was never found.
window() { # <object> <symbol>
    "$OBJDUMP" -d --no-show-raw-insn "$1" 2>/dev/null \
        | awk -v sym="$2" '
            /^[0-9a-f]+ <.*>:/ {
                name = $2
                gsub(/[<>:]/, "", name)
                inwin = (name == sym)
                next
            }
            inwin && /^[ \t]*[0-9a-f]+:/ {
                text = $0
                sub(/^[^:]*:[ \t]*/, "", text)
                sub(/[ \t]*#.*$/, "", text)
                mnem = text
                sub(/[ \t].*$/, "", mnem)
                if (mnem == "cld") { print "clear"; found = 1; exit }
                if (mnem == "call" || mnem == "callq") { print "call"; found = 1; exit }
            }
            END { if (!found) { print "none" } }'
}

# --- the detector, before it is asked to report a presence --------------------
# A reader that answers `clear` for everything would pass this gate over any object at all,
# so both answers are produced from purpose-built text first.
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
    .globl kos_ctl_target
kos_ctl_target:
    ret
EOF
"$CC" -c -o "$TMP/ctl.o" "$TMP/ctl.s" || fail "$CC could not assemble $TMP/ctl.s"

CTL_CLEAN="$(window "$TMP/ctl.o" kos_ctl_clean)"
CTL_DIRTY="$(window "$TMP/ctl.o" kos_ctl_dirty)"
CTL_ABSENT="$(window "$TMP/ctl.o" kos_ctl_nothing)"
[ "$CTL_CLEAN" = clear ] \
    || fail "the reader answered '$CTL_CLEAN' for a control body that clears the flag, so a
      pass here would say nothing about the entry"
[ "$CTL_DIRTY" = call ] \
    || fail "the reader answered '$CTL_DIRTY' for a control body that calls without clearing
      the flag, so this gate cannot go red"
[ "$CTL_ABSENT" = none ] \
    || fail "the reader answered '$CTL_ABSENT' for a symbol that is not in the object, so a
      renamed entry would read as clean"

echo "== control: clear/call/none read back from a purpose-built object =="

# --- the corpus ---------------------------------------------------------------
find "$BUILD" -path '*.dir/*' -name 'trap_x86_64.S*' \( -name '*.o' -o -name '*.obj' \) \
     -type f | sort > "$TMP/objects" || fail "the object walk of $BUILD failed"
N_OBJ="$(wc -l < "$TMP/objects" | tr -d ' ')"
[ "$N_OBJ" -ge "$OBJ_FLOOR" ] \
    || fail "$N_OBJ assembled trap_x86_64.S object(s) under $BUILD, beneath the floor of
      $OBJ_FLOOR: this tree is unbuilt, or the walk found the wrong directory."

N_CLEAR=0
: > "$TMP/findings"
while IFS= read -r obj; do
    "$OBJDUMP" -f "$obj" 2>/dev/null | grep -q '^architecture: i386:x86-64' \
        || fail "$OBJDUMP reports no i386:x86-64 architecture for $obj, so the read of that
      object took a dead tool for a clean answer"
    ANSWER="$(window "$obj" "$ENTRY")"
    if [ "$ANSWER" = clear ]; then
        N_CLEAR=$((N_CLEAR + 1))
    else
        echo "${obj#"$BUILD"/}: $ENTRY answered '$ANSWER'" >> "$TMP/findings"
    fi
done < "$TMP/objects"

if [ -s "$TMP/findings" ]; then
    cat "$TMP/findings" >&2
    echo "" >&2
    fail "the interrupt entry reaches C without clearing the direction flag. An interrupt
      gate leaves that flag as the interrupted code set it, the instruction that sets it is
      unprivileged, and the kernel archive on this board carries string operations, so
      restore the cld ahead of the first call in $ENTRY (arch/x86/x86_64/trap_x86_64.S)."
fi

echo "PASS: $ENTRY clears the direction flag before its first call in $N_OBJ object(s)"
