#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# kickos_panic_stack_enter really masks, and really moves the stack pointer to the seat its
# caller passed, on every backend that has one.
#
# Run from the repo root, no arguments: tests/static/check_panic_stack_seat.sh
#
# WHY A SOURCE GATE. check_trap_redzone.sh measures the trap descents by walking gcc's
# -fcallgraph-info output, and no .ci file describes assembly, so the walk stops at this entry.
# The qemu panicgate cases run it and still pass with both instructions deleted: the panic line
# reaches the wire from whichever stack the reporter stands on. RXV3 and LX6 have no runtime arm
# at all.
#
# WHAT IT PROVES. Each assembly entry contains the mask its ISA spells and the ONE instruction
# that writes its stack pointer FROM THE FOURTH ARGUMENT REGISTER, which is where arch.h says the
# caller puts the seat. Naming the whole instruction refuses the near misses a looser pattern
# accepts: a test of sp (`cmp sp, r0`), an adjustment of the live sp (`sub sp, sp, #32`), and a
# move from the wrong register. A backend whose definition is C is DECLINING the switch, and
# must say so in Kconfig with a `default 0 if ARCH_<X>` under KICKOS_PANIC_STACK_SIZE. That every
# backend defines the entry exactly once is the linker's: it is a mandatory seam with no fallback
# body.
#
# WHAT IT DOES NOT PROVE. It is TEXT. It cannot show the instruction is REACHED, that the mask
# precedes it, or that nothing after it moves sp again.
#
# COMMENTS ARE BLANKED PER ISA: `;` on RX, `#` on RISC-V, Xtensa and x86_64 AT&T, `@` on ARM
# where `#` is an immediate. A blanker that knew only `/* */` and `//` passes an rxv3 body whose
# seat is commented out with `;`.

set -u
set -f
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root
scratch_dir

ENTRY=kickos_panic_stack_enter
# Every backend with an assembly entry. Each must be read, so a walk that missed one, or a
# backend that left, refuses rather than passes.
SWITCHING="rv32imac rv64imac armv6m armv7m armv8a rxv3 lx6 x86_64"

# The FOURTH argument is the seat, per arch.h: a3 on RISC-V, r3 on ARM, x3 on AArch64, R4 on RX
# (whose first argument is R1), a5 on Xtensa (whose arguments are a2-a5 once `entry` has run),
# and rcx under the SysV AMD64 ABI.
seat_ere() { # <arch>
    case "$1" in
        rv32imac|rv64imac) printf '%s' '^[[:space:]]*mv[[:space:]]+sp,[[:space:]]*a3[[:space:]]*$' ;;
        armv6m|armv7m)     printf '%s' '^[[:space:]]*mov[[:space:]]+sp,[[:space:]]*r3[[:space:]]*$' ;;
        armv8a)            printf '%s' '^[[:space:]]*mov[[:space:]]+sp,[[:space:]]*x3[[:space:]]*$' ;;
        # R0 IS the stack pointer on RX, and RX mnemonics write their LAST operand.
        rxv3)              printf '%s' '^[[:space:]]*mov\.l[[:space:]]+r4,[[:space:]]*r0[[:space:]]*$' ;;
        # The windowed stack pointer is a1. `entry a1, N` also writes a1 and is the frame
        # opening this ISA forces on any windowed callee, so the pattern names the move.
        lx6)               printf '%s' '^[[:space:]]*mov[[:space:]]+a1,[[:space:]]*a5[[:space:]]*$' ;;
        # AT&T writes the last operand.
        x86_64)            printf '%s' '^[[:space:]]*movq[[:space:]]+%rcx,[[:space:]]*%rsp[[:space:]]*$' ;;
        *) return 1 ;;
    esac
}

# Requirement 1 of the seam: interrupts off BEFORE the move, named whole for the same reason.
mask_ere() { # <arch>
    case "$1" in
        rv32imac)          printf '%s' '^[[:space:]]*csrci[[:space:]]+mstatus,[[:space:]]*8[[:space:]]*$' ;;
        rv64imac)          printf '%s' '^[[:space:]]*csrci[[:space:]]+sstatus,[[:space:]]*2[[:space:]]*$' ;;
        armv6m|armv7m)     printf '%s' '^[[:space:]]*cpsid[[:space:]]+i[[:space:]]*$' ;;
        armv8a)            printf '%s' '^[[:space:]]*msr[[:space:]]+daifset,[[:space:]]*#0xf[[:space:]]*$' ;;
        rxv3)              printf '%s' '^[[:space:]]*clrpsw[[:space:]]+i[[:space:]]*$' ;;
        lx6)               printf '%s' '^[[:space:]]*rsil[[:space:]]+a[0-9]+,[[:space:]]*15[[:space:]]*$' ;;
        x86_64)            printf '%s' '^[[:space:]]*cli[[:space:]]*$' ;;
        *) return 1 ;;
    esac
}

line_comment() { # <arch>
    case "$1" in
        rv32imac|rv64imac|lx6|x86_64) printf '%s' '#' ;;
        armv6m|armv7m|armv8a)         printf '%s' '@' ;;
        rxv3)                         printf '%s' ';' ;;
        *) return 1 ;;
    esac
}

# The entry's own body out of an assembly file: from its label to the next label at column 0 or
# the first directive that closes a symbol. A local numeric label (`1:`) is part of the body,
# which is what keeps armv6m's literal pool inside it.
asm_body() { # <file> <line-comment-char> <outfile>
    awk -v SYM="$ENTRY" -v CC="$2" '
        { line = $0 }
        {
            while (match(line, /\/\*[^*]*\*+([^\/*][^*]*\*+)*\//)) {
                line = substr(line, 1, RSTART - 1) " " substr(line, RSTART + RLENGTH)
            }
            sub(/\/\/.*/, "", line)
            i = index(line, CC)
            if (i > 0) { line = substr(line, 1, i - 1) }
        }
        !inblock && line ~ ("^_?" SYM ":") { inblock = 1; next }
        inblock && line ~ /^[A-Za-z_.][A-Za-z_0-9.]*:/ { inblock = 0 }
        inblock && line ~ /^[[:space:]]*\.(size|end|section|text|data)([[:space:]]|$)/ { inblock = 0 }
        inblock { print line }
    ' "$1" > "$3"
}

# Every definition of the entry under arch/ in <filelist>, judged by its backend directory,
# findings one per line into <out> and each assembly backend read into <out>.read.
judge() { # <root> <filelist> <kconfig> <out>
    : > "$4"
    : > "$4.read"
    while IFS= read -r f; do
        case "$f" in
            arch/*.S) grep -qE "^_?$ENTRY:" "$1/$f" || continue ;;
            arch/*.cc|arch/*.c)
                grep -qE "^[A-Za-z_].*[^A-Za-z_0-9]$ENTRY[[:space:]]*\(" "$1/$f" \
                    && ! grep -qE "$ENTRY[^;]*\)[[:space:]]*;" "$1/$f" || continue ;;
            *) continue ;;
        esac
        _d="${f%/*}"
        _arch="${_d##*/}"
        case "$f" in
            *.c|*.cc)
                _sym="ARCH_$(printf '%s' "$_arch" | tr '[:lower:]' '[:upper:]')"
                grep -qE "^[[:space:]]*default[[:space:]]+0[[:space:]]+if[[:space:]]+$_sym[[:space:]]*$" \
                    "$3" || echo "$f defines $ENTRY in C, so $_arch declines the switch, and \
Kconfig carries no 'default 0 if $_sym' under KICKOS_PANIC_STACK_SIZE to say so" >> "$4"
                continue ;;
        esac
        if ! _seat="$(seat_ere "$_arch")" || ! _mask="$(mask_ere "$_arch")" \
           || ! _cc="$(line_comment "$_arch")"; then
            echo "$f defines $ENTRY in assembly and this gate declares no seat, mask or \
comment record for $_arch" >> "$4"
            continue
        fi
        printf '%s\n' "$_arch" >> "$4.read"
        asm_body "$1/$f" "$_cc" "$TMP/body"
        if [ ! -s "$TMP/body" ]; then
            echo "$f: the body of $ENTRY came out empty, so its verdict is UNKNOWN" >> "$4"
            continue
        fi
        grep -qE "$_mask" "$TMP/body" \
            || echo "$f: $ENTRY carries no $_arch interrupt mask" >> "$4"
        grep -qE "$_seat" "$TMP/body" \
            || echo "$f: $ENTRY does not move the $_arch stack pointer from the fourth \
argument register" >> "$4"
    done < "$2"
}

# The planted pair: an rxv3 body whose seat a `;` comment hides, and a C body with no declining
# default, each beside its correct twin.
W="$TMP/world"
mkdir -p "$W/arch/rx/rxv3" "$W/arch/sim"
printf '_%s:\n    clrpsw  i\n    mov.l   r4, r0\n' "$ENTRY" > "$W/arch/rx/rxv3/good.S"
printf '_%s:\n    clrpsw  i\n  ; mov.l   r4, r0\n' "$ENTRY" > "$W/arch/rx/rxv3/bad.S"
printf 'void %s(char const* m)\n{\n}\n' "$ENTRY" > "$W/arch/sim/sim.cc"
printf 'arch/rx/rxv3/good.S\narch/sim/sim.cc\n' > "$W/good"
printf 'arch/rx/rxv3/bad.S\narch/sim/sim.cc\n' > "$W/bad"
printf '\tdefault 0 if ARCH_SIM\n' > "$W/kconfig"
judge "$W" "$W/good" "$W/kconfig" "$TMP/jf"
[ ! -s "$TMP/jf" ] || fail "the planted correct backends report: $(cat "$TMP/jf")"
judge "$W" "$W/bad" /dev/null "$TMP/jf"
grep -q 'bad.S: .* does not move the rxv3 stack pointer' "$TMP/jf" \
    && grep -q "no 'default 0 if ARCH_SIM'" "$TMP/jf" \
    || fail "the seat in a ';' comment or the silent decline went unreported: $(cat "$TMP/jf")"

corpus_all "$TMP/tracked"
# arch/Kconfig declares the arch symbols and the top-level one carries KICKOS_PANIC_STACK_SIZE.
: > "$TMP/kconfig"
while IFS= read -r f; do
    case "$f" in
        Kconfig|*/Kconfig) cat "$f" >> "$TMP/kconfig" ;;
    esac
done < "$TMP/tracked"
require_nonempty "$TMP/kconfig" "no tracked Kconfig, so the panic-stack knob could not be read"

judge . "$TMP/tracked" "$TMP/kconfig" "$TMP/findings"
for a in $SWITCHING; do
    grep -qx "$a" "$TMP/findings.read" || echo "no assembly $ENTRY was read for $a: the \
walk missed it, or the backend left and its record here stays" >> "$TMP/findings"
done
if [ -s "$TMP/findings" ]; then
    sed 's/^/      /' "$TMP/findings" >&2
    fail "$(wc -l < "$TMP/findings" | tr -d ' ') panic-stack finding(s). An assembly entry masks
      and moves its stack pointer from the fourth argument register; a C one declines the
      switch in Kconfig."
fi
echo "PASS: $(wc -l < "$TMP/findings.read" | tr -d ' ') assembly panic entries mask and seat from
      their ABI's fourth argument, and every C one declines in Kconfig"
