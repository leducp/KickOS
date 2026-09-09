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
# That is exactly what takes the panic reporter's console tail out of every red-zone figure,
# and it is also why that gate cannot see what the entry DOES: deleting the move from the
# rv32imac body leaves all nine rv32imac figures green while the reporter runs on the caller's
# stack, spending a descent nothing reserves for any more.
#
# WHAT IT PROVES. Each entry contains the mask its ISA spells and the ONE instruction that
# writes its stack pointer FROM THE FOURTH ARGUMENT REGISTER, which is where arch.h says the
# caller puts the seat. Naming the whole instruction rather than "something writes sp" is what
# refuses the near misses that a looser pattern accepts: a test of sp (`cmp sp, r0`), an
# adjustment of the live sp (`sub sp, sp, #32`), and a move from the wrong register.
#
# WHAT IT DOES NOT PROVE. It is TEXT. It cannot show the instruction is REACHED, that the mask
# precedes it, or that nothing after it moves sp again. Only a disassembler could. The running
# half is the qemu panicgate suite, and that suite does not reach every backend either:
# kickos_add_qemu_test registers it for armv7m, armv6m, rv32imac, rv64imac, armv8a and x86_64,
# so RXV3 AND LX6 HAVE NO RUNTIME ARM AT ALL and this gate is the whole of their witness.
#
# THREE CLAIMS. 1: every backend directory defines the entry exactly once. 2: a backend whose
# definition is assembly carries its ISA's mask and its ISA's seat move, both inside the entry's
# own body and neither in a comment. 3: a backend whose definition is C is DECLINING the switch,
# and must say so in Kconfig with a `default 0 if ARCH_<X>` under KICKOS_PANIC_STACK_SIZE, or it
# keeps a reporter on a stack the red-zone figures no longer reserve for.
#
# COMMENTS ARE BLANKED PER ISA, and that is load-bearing rather than tidy. The comment character
# is not the same everywhere: `;` on RX, `#` on RISC-V, Xtensa and x86_64 AT&T, `@` on ARM where
# `#` is an immediate. A blanker that knew only `/* */` and `//` passed an rxv3 body whose seat
# was commented out with `;` and an x86_64 body whose seat was commented out with `#`, which are
# precisely the two arches with no runtime arm behind them.

set -u
# Findings accumulate over every backend, so one run names all of them.
set -f
. "$(dirname "$0")/../lib/gate.sh"

export LC_ALL=C
scratch_dir

ENTRY=kickos_panic_stack_enter

# --- what each ISA spells, one record per backend that switches ---------------
# The FOURTH argument is the seat, per arch.h: a3 on RISC-V, r3 on ARM, x3 on AArch64, R4 on RX
# (whose first argument is R1), a5 on Xtensa (whose arguments are a2-a5 once `entry` has run),
# and rcx under the SysV AMD64 ABI. Each pattern names the WHOLE instruction and is anchored at
# both ends, so a body that merely mentions the register does not satisfy it.
seat_ere() { # <arch>
    case "$1" in
        rv32imac|rv64imac) printf '%s' '^[[:space:]]*mv[[:space:]]+sp,[[:space:]]*a3[[:space:]]*$' ;;
        armv6m|armv7m)     printf '%s' '^[[:space:]]*mov[[:space:]]+sp,[[:space:]]*r3[[:space:]]*$' ;;
        armv8a)            printf '%s' '^[[:space:]]*mov[[:space:]]+sp,[[:space:]]*x3[[:space:]]*$' ;;
        # R0 IS the stack pointer on RX, and RX mnemonics write their LAST operand.
        rxv3)              printf '%s' '^[[:space:]]*mov\.l[[:space:]]+r4,[[:space:]]*r0[[:space:]]*$' ;;
        # The windowed stack pointer is a1. `entry a1, N` also writes a1 and is the frame
        # opening this ISA forces on any windowed callee, so the pattern names the move and not
        # the register: a looser one passed a body that opened its frame and seated nothing.
        lx6)               printf '%s' '^[[:space:]]*mov[[:space:]]+a1,[[:space:]]*a5[[:space:]]*$' ;;
        # AT&T writes the last operand.
        x86_64)            printf '%s' '^[[:space:]]*movq[[:space:]]+%rcx,[[:space:]]*%rsp[[:space:]]*$' ;;
        *) return 1 ;;
    esac
}

# Requirement 1 of the seam: interrupts off BEFORE the move. Same shape, same reason for naming
# the instruction: an ISA where the mask is a register write would otherwise be satisfied by any
# mention of that register.
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

# The line-comment character, per assembler. Empty where the ISA has none beyond the two C
# forms every one of these files is preprocessed with.
line_comment() { # <arch>
    case "$1" in
        rv32imac|rv64imac|lx6|x86_64) printf '%s' '#' ;;
        armv6m|armv7m)                printf '%s' '@' ;;
        armv8a)                       printf '%s' '@' ;;
        rxv3)                         printf '%s' ';' ;;
        *) return 1 ;;
    esac
}

# The entry's own body out of an assembly file: from its label to the next label at column 0 or
# the first directive that closes a symbol, whichever comes first. A local numeric label (`1:`)
# is part of the body, which is what keeps armv6m's literal pool inside it.
asm_body() { # <file> <line-comment-char> <outfile>
    awk -v SYM="$ENTRY" -v CC="$2" '
        BEGIN { inblock = 0 }
        { line = $0 }
        {
            while (match(line, /\/\*[^*]*\*+([^\/*][^*]*\*+)*\//)) {
                line = substr(line, 1, RSTART - 1) " " substr(line, RSTART + RLENGTH)
            }
            sub(/\/\/.*/, "", line)
            if (CC != "") {
                i = index(line, CC)
                if (i > 0) { line = substr(line, 1, i - 1) }
            }
        }
        !inblock && line ~ ("^_?" SYM ":") { inblock = 1; next }
        inblock && line ~ /^[A-Za-z_.][A-Za-z_0-9.]*:/ { inblock = 0 }
        inblock && line ~ /^[[:space:]]*\.(size|end|section|text|data)([[:space:]]|$)/ { inblock = 0 }
        inblock { print line }
    ' "$1" > "$3"
}

# --- self-test: prove every claim fires on planted input ----------------------
# The world is PLANTED, so nothing here is tracked and no claim reads the real tree. Each
# control is one edit away from the clean world and the finding COUNT is asserted, not a colour.
# TWO assembly backends are planted, not one: they carry DIFFERENT comment characters, and the
# blanker was wrong for exactly the ISAs the single planted one did not represent.
world() { # <dir>
    _w="$1"
    rm -rf "$_w"
    mkdir -p "$_w/arch/ff/rv32imac/include/kickos/arch" \
             "$_w/arch/ff/rxv3/include/kickos/arch" \
             "$_w/arch/ff/sim/include/kickos/arch"
    : > "$_w/arch/ff/rv32imac/include/kickos/arch/context.h"
    : > "$_w/arch/ff/rxv3/include/kickos/arch/context.h"
    : > "$_w/arch/ff/sim/include/kickos/arch/context.h"
    cat > "$_w/arch/ff/rv32imac/switch.S" <<'PLANTED'
    .global kickos_panic_stack_enter
kickos_panic_stack_enter:
    csrci   mstatus, 8
    mv      sp, a3
    tail    kickos_panic_report
PLANTED
    cat > "$_w/arch/ff/rxv3/switch.S" <<'PLANTED'
    .global _kickos_panic_stack_enter
_kickos_panic_stack_enter:
    clrpsw  i
    setpsw  u
    mov.l   r4, r0
    bra.a   _kickos_panic_report
PLANTED
    cat > "$_w/arch/ff/sim/sim.cc" <<'PLANTED'
void kickos_panic_stack_enter(char const* msg, char const* file, unsigned line, uintptr_t top)
{
    (void)top;
    kickos_panic_report(msg, file, line);
}
PLANTED
    printf 'config ARCH_RV32IMAC\nconfig ARCH_RXV3\nconfig ARCH_SIM\nconfig KICKOS_PANIC_STACK_SIZE\n\tint\n\tdefault 0 if ARCH_SIM\n\tdefault 448 if ARCH_RV32IMAC\n' \
        > "$_w/kconfig"
    {
        printf 'arch/ff/rv32imac/include/kickos/arch/context.h\n'
        printf 'arch/ff/rv32imac/switch.S\n'
        printf 'arch/ff/rxv3/include/kickos/arch/context.h\n'
        printf 'arch/ff/rxv3/switch.S\n'
        printf 'arch/ff/sim/include/kickos/arch/context.h\n'
        printf 'arch/ff/sim/sim.cc\n'
    } > "$_w/files"
}

# --- the judge: every claim, over one root and one file list ------------------
judge() { # <root> <filelist> <kconfig> <findings-out>
    _root="$1"; _files="$2"; _kconfig="$3"; _out="$4"
    : > "$_out"
    # A finding's text spans lines, so its FIRST line carries a marker and the count is the
    # number of markers. Counting lines would read one finding as several.
    report() { echo ">>$*" >> "$_out"; }

    # Claim 1's corpus: a backend is a directory shipping include/kickos/arch/context.h, which
    # every backend does and nothing else does. DERIVED, so a new backend joins the corpus by
    # existing rather than by being listed here. The DEPTH IS NOT FIXED: every MCU backend sits
    # at arch/<family>/<arch>/ and the sim at arch/sim/, and a pattern spelling two components
    # dropped the one backend that declines the switch, which is the only subject of claim 3.
    sed -n 's|^\(arch/.*\)/include/kickos/arch/context\.h$|\1|p' "$_files" \
        | sort -u > "$TMP/backends"
    if [ ! -s "$TMP/backends" ]; then
        report "no arch/<family>/<arch>/include/kickos/arch/context.h is tracked, so this gate
      found no backend at all and would pass over a tree with none of them ported"
        return 0
    fi

    while IFS= read -r d; do
        _arch="${d##*/}"
        _sym="ARCH_$(printf '%s' "$_arch" | tr '[:lower:]' '[:upper:]')"
        # A renamed directory would otherwise leave this gate reading a backend Kconfig does not
        # have, and the declining clause comparing against a symbol nothing resolves.
        grep -qE "^config[[:space:]]+$_sym[[:space:]]*$" "$_kconfig" \
            || report "$d maps to $_sym, which the tracked Kconfig declares nowhere; an arch
      directory is named for its Kconfig symbol, so one of the two moved"
        : > "$TMP/defs"
        while IFS= read -r f; do
            case "$f" in
                "$d"/*) ;;
                *) continue ;;
            esac
            [ -r "$_root/$f" ] || continue
            case "$f" in
                *.S)
                    grep -qE "^_?$ENTRY:" "$_root/$f" && printf 'S\t%s\n' "$f" >> "$TMP/defs" ;;
                *.cc|*.c)
                    grep -qE "^[A-Za-z_].*[^A-Za-z_0-9]$ENTRY[[:space:]]*\(" "$_root/$f" \
                        && ! grep -qE "$ENTRY[^;]*\)[[:space:]]*;" "$_root/$f" \
                        && printf 'C\t%s\n' "$f" >> "$TMP/defs" ;;
            esac
        done < "$_files"

        _n="$(wc -l < "$TMP/defs" | tr -d ' ')"
        if [ "$_n" -eq 0 ]; then
            report "$d defines $ENTRY nowhere. It is a mandatory seam with no fallback body, so
      this backend cannot link a kernel; if it is meant to DECLINE the switch it still owes the
      C body that calls kickos_panic_report"
            continue
        fi
        if [ "$_n" -gt 1 ]; then
            report "$d defines $ENTRY $_n times, so which body a link takes is a question this
      gate will not answer: $(cut -f2 "$TMP/defs" | tr '\n' ' ')"
            continue
        fi

        _kind="$(cut -f1 "$TMP/defs")"
        _file="$(cut -f2 "$TMP/defs")"
        if [ "$_kind" = C ]; then
            # Claim 3. A C body cannot move its own stack pointer and keep running, so this
            # backend is declining, and the only honest way to decline is to say so in the knob.
            grep -qE "^[[:space:]]*default[[:space:]]+0[[:space:]]+if[[:space:]]+$_sym[[:space:]]*$" \
                "$_kconfig" \
                || report "$_file defines $ENTRY in C, which cannot move a stack pointer and
      continue, so this backend does not switch; Kconfig carries no 'default 0 if $_sym' under
      KICKOS_PANIC_STACK_SIZE to say so, and the board would carve an array the reporter never
      stands on while every red-zone figure stops reserving for it"
            continue
        fi

        # Claim 2.
        if ! _seat="$(seat_ere "$_arch")" || ! _mask="$(mask_ere "$_arch")" \
           || ! _cc="$(line_comment "$_arch")"; then
            report "$_file defines $ENTRY in assembly and this gate declares no seat, mask or
      comment record for $_arch, so it would read the body and conclude nothing. Add all three
      in seat_ere, mask_ere and line_comment, naming the register this ABI puts the fourth
      argument in and the character this assembler starts a comment with"
            continue
        fi
        asm_body "$_root/$_file" "$_cc" "$TMP/body"
        if [ ! -s "$TMP/body" ]; then
            report "$_file: the body of $ENTRY came out empty, so its verdict is UNKNOWN"
            continue
        fi
        grep -qE "$_mask" "$TMP/body" \
            || report "$_file: $ENTRY carries no $_arch interrupt mask, so an interrupt taken
      between the entry and the move lands on the stack this exists to leave"
        grep -qE "$_seat" "$TMP/body" \
            || report "$_file: $ENTRY does not move the $_arch stack pointer from the fourth
      argument register, so the reporter runs where the assertion fired and spends a descent no
      red-zone figure reserves for. A test of the stack pointer, an adjustment of the live one,
      or a move from another register all read this way"
    done < "$TMP/backends"
    return 0
}

W="$TMP/world"
findings() { awk '/^>>/ { n++ } END { print n + 0 }' "$TMP/jf"; }
run_judge() {
    judge "$W" "$W/files" "$W/kconfig" "$TMP/jf" > "$TMP/jo" 2>&1 \
        || { cat "$TMP/jo" >&2; fail "the judge itself failed on the planted world"; }
}
one() { # <label> <expect-ere>
    run_judge
    [ "$(findings)" -eq 1 ] || {
        sed 's/^>>/FAIL: /' "$TMP/jf" >&2
        fail "control '$1' reports $(findings) finding(s), expected exactly 1"
    }
    grep -qE "$2" "$TMP/jf" || {
        sed 's/^>>/FAIL: /' "$TMP/jf" >&2
        fail "control '$1' reports, but not for its own clause; expected /$2/. A control
      another clause catches proves nothing about the clause it plants for"
    }
}
none() { # <label>
    run_judge
    [ "$(findings)" -eq 0 ] || {
        sed 's/^>>/FAIL: /' "$TMP/jf" >&2
        fail "negative control '$1' reports; the gate would redden a correct tree"
    }
}
edit() { # <file> <sed-script>
    sed "$2" "$W/$1" > "$W/t" && mv "$W/t" "$W/$1"
}

world "$W"
none "two switching backends and a declining one, all correct"

# Claim 2's seat half, and the one this gate exists for: the move is deleted and the body still
# branches, which is what the callgraph gate cannot see.
world "$W"
grep -v 'mv      sp, a3' "$W/arch/ff/rv32imac/switch.S" > "$W/t"
mv "$W/t" "$W/arch/ff/rv32imac/switch.S"
one seat_gone 'does not move the rv32imac stack pointer'

# THE NEAR MISS PER ISA, and the reason the blanker is per arch: a `#` comment on RISC-V and a
# `;` comment on RX are the two an ISA-blind blanker left standing.
world "$W"
edit arch/ff/rv32imac/switch.S 's|^    mv      sp, a3$|  # mv      sp, a3|'
one seat_in_a_hash_comment 'does not move the rv32imac stack pointer'

world "$W"
edit arch/ff/rxv3/switch.S 's|^    mov.l   r4, r0$|  ; mov.l   r4, r0|'
one seat_in_a_semicolon_comment 'does not move the rxv3 stack pointer'

# A TEST of the stack pointer is not a write to it.
world "$W"
edit arch/ff/rv32imac/switch.S 's|^    mv      sp, a3$|    beq     sp, a3, .|'
one seat_is_only_a_test 'does not move the rv32imac stack pointer'

# An ADJUSTMENT of the live stack pointer is not a seat.
world "$W"
edit arch/ff/rv32imac/switch.S 's|^    mv      sp, a3$|    addi    sp, sp, -32|'
one seat_is_only_an_adjustment 'does not move the rv32imac stack pointer'

# A move from the WRONG register is the shape a port gets wrong, the seat being the fourth
# argument and not the first.
world "$W"
edit arch/ff/rv32imac/switch.S 's|^    mv      sp, a3$|    mv      sp, a0|'
one seat_from_the_wrong_register 'does not move the rv32imac stack pointer'

# Requirement 1 of the seam has the same standing as requirement 2.
world "$W"
grep -v 'csrci   mstatus, 8' "$W/arch/ff/rv32imac/switch.S" > "$W/t"
mv "$W/t" "$W/arch/ff/rv32imac/switch.S"
one mask_gone 'carries no rv32imac interrupt mask'

# Claim 1: a backend directory with no definition anywhere.
world "$W"
rm "$W/arch/ff/rv32imac/switch.S"
grep -v '^arch/ff/rv32imac/switch.S$' "$W/files" > "$W/t" && mv "$W/t" "$W/files"
one no_definition 'defines kickos_panic_stack_enter nowhere'

# Claim 1 again: two definitions in one backend.
world "$W"
cp "$W/arch/ff/rv32imac/switch.S" "$W/arch/ff/rv32imac/second.S"
printf 'arch/ff/rv32imac/second.S\n' >> "$W/files"
one two_definitions 'defines kickos_panic_stack_enter 2 times'

# Claim 3: a C body with no declining default in Kconfig.
world "$W"
grep -v 'default 0 if ARCH_SIM' "$W/kconfig" > "$W/t" && mv "$W/t" "$W/kconfig"
one declines_silently "carries no 'default 0 if ARCH_SIM'"

# The near miss for claim 3: the same knob line, but for another arch.
world "$W"
edit kconfig 's/default 0 if ARCH_SIM/default 0 if ARCH_OTHER/'
one declines_for_another_arch "carries no 'default 0 if ARCH_SIM'"

# The directory and its Kconfig symbol have to be the same name, or the declining clause
# compares against a symbol nothing resolves.
world "$W"
grep -v '^config ARCH_RV32IMAC$' "$W/kconfig" > "$W/t" && mv "$W/t" "$W/kconfig"
one arch_symbol_missing 'ARCH_RV32IMAC, which the tracked Kconfig declares nowhere'

# A backend this gate has no records for must SAY so rather than read the body and conclude
# nothing. The Kconfig symbol is renamed with the directory so the clause above stays quiet.
world "$W"
mv "$W/arch/ff/rv32imac" "$W/arch/ff/rv32imax"
edit files 's|/rv32imac/|/rv32imax/|'
edit kconfig 's|^config ARCH_RV32IMAC$|config ARCH_RV32IMAX|'
one no_records 'declares no seat, mask or'

echo "== control: every claim fires on planted input, and the clean world reports nothing =="

# --- the tree ------------------------------------------------------------------
[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

git ls-files > "$TMP/tracked" || fail "git ls-files failed; the corpus would be short"
require_nonempty "$TMP/tracked" "git ls-files listed nothing"

# The two facts a claim reads live in DIFFERENT Kconfig files: arch/Kconfig declares the arch
# symbols and the top-level one carries KICKOS_PANIC_STACK_SIZE, so the corpus is every tracked
# Kconfig rather than one of them. Reading only arch/Kconfig reported the sim as declining
# without saying so, against a default that was there all along.
: > "$TMP/kconfig_all"
while IFS= read -r f; do
    case "$f" in
        Kconfig|*/Kconfig) cat "$f" >> "$TMP/kconfig_all" ;;
    esac
done < "$TMP/tracked"
require_nonempty "$TMP/kconfig_all" "no tracked Kconfig, so neither the arch symbols nor the
      panic-stack knob could be read"

judge . "$TMP/tracked" "$TMP/kconfig_all" "$TMP/findings"

NB="$(sed -n 's|^\(arch/.*\)/include/kickos/arch/context\.h$|\1|p' "$TMP/tracked" \
      | sort -u | wc -l | tr -d ' ')"
echo "== $NB backend(s), each read for its own definition of $ENTRY =="

if [ -s "$TMP/findings" ]; then
    sed 's/^>>/FAIL: /' "$TMP/findings" >&2
    fail "$(awk '/^>>/ { n++ } END { print n + 0 }' "$TMP/findings") panic-stack finding(s).
      Every backend defines kickos_panic_stack_enter once; an assembly body carries its ISA's
      mask and moves its stack pointer from the fourth argument register, and a C body declines
      the switch in Kconfig. What no source gate can prove is that either instruction is
      REACHED: the qemu panicgate cases carry that half on six of the eight backends, and rxv3
      and lx6 have no runtime arm at all."
fi
echo "PASS: every backend's panic entry masks and seats from its own ABI's fourth argument, or
      declines the switch where Kconfig says it does"
