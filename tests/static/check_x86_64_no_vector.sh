#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Refuse any vector or x87 instruction in the x86_64 freestanding text.
#
#   tests/static/check_x86_64_no_vector.sh <objdump> <cc> <build-dir>
#
# A vector or x87 instruction here is live register state no context switch preserves:
# arch/x86/x86_64/switch.S and trap_x86_64.S save no XMM and no FPU state, and
# arch/x86/x86_64/entry_x86_64.cc sets CR0.EM and CR0.TS and clears CR4.OSFXSR and CR4.OSXSAVE,
# so it is also a #UD or an #NM at ring 0.
#
# The corpus is DERIVED from the build tree, never listed here: every object CMake compiled for
# a target of this project, plus every PE32+ image linked out of them. The images join their own
# inputs because the linker is free to synthesise text of its own, and an archive-level sweep is
# evidence about archives only.
#
# THREE FLOORS, because a gate handed nothing must go red rather than clean: an unbuilt tree, a
# wrong directory and a corpus of archives alone each go red. Every run prints all three
# counts.
#
# A per-input read check beside them, because a corpus-wide floor cannot see one input go
# unread. `objdump -d` prints nothing for an input whose text it could not read AND for one that
# legitimately carries none, and this build has both. The section headers are the second oracle:
# an input carrying an executable section of nonzero size must decode at least one instruction,
# and an input carrying no such section must decode none; either way round refuses.
#
# INSN_FLOOR is held ABOVE the image floor. The IMG_FLOOR images a passing run guarantees can be
# the SMALLEST ones in the build, so a floor beneath that guarantee could never fire, and a floor
# that cannot fire reads exactly like one that can. Re-check that ordering, not the totals,
# whenever a floor moves.

set -u
. "$(dirname "$0")/../lib/gate.sh"

# Every objdump below reads its own output back, so the locale is part of the contract: this
# box's binutils is localised and prints `format de fichier` where the parses here read
# `file format`.
LC_ALL=C
export LC_ALL

# An unbuilt tree and a walk that found the wrong directory are what this refuses.
OBJ_FLOOR=64
# A tree whose objects compiled and whose links did not leaves the text the linker contributes
# unread.
IMG_FLOOR=8
# Held ABOVE what IMG_FLOOR images alone already guarantee, so a disassembler that opened every
# input and produced almost nothing is refused.
INSN_FLOOR=64000
# The host-binary control's floor. An ordinary distribution binary carries hundreds.
HOST_FLOOR=32

if [ "$#" -ne 3 ]; then
    fail "usage: check_x86_64_no_vector.sh <objdump> <cc> <build-dir>"
fi
OBJDUMP="$1"
CC="$2"
BUILD="${3%/}"

command -v "$OBJDUMP" >/dev/null 2>&1 || [ -x "$OBJDUMP" ] || fail "no objdump at $OBJDUMP"
command -v "$CC" >/dev/null 2>&1 || [ -x "$CC" ] || fail "no compiler at $CC"
[ -d "$BUILD" ] || fail "no build directory at $BUILD"

scratch_dir

# The instruction prefixes objdump prints as words of their own, dropped so that the word after
# them is read as the mnemonic. A prefix not listed here hides the mnemonic behind it.
PREFIXES='^(lock|rep|repz|repnz|repe|repne|bnd|notrack|data16|data32|addr32|rex([.][A-Z]+)?|cs|ds|es|fs|gs|ss)$'

# The vector and x87 state instructions that name no such register in their operands. Every
# other x87 mnemonic begins with f, which the awk below matches as a prefix.
STATEOPS='^(emms|femms|ldmxcsr|stmxcsr|xsave|xsaveopt|xsavec|xsaves|xrstor|xrstors|vzeroupper|vzeroall)$'

# Disassemble one input into <hits> (one `<mnemonic> <text>` line each) and append the number
# of instructions decoded to $TMP/insn, which is the corpus-wide control that the tool ran.
census() { # <input> <hits>
    "$OBJDUMP" -d --no-show-raw-insn "$1" 2>/dev/null \
        | awk -v pfx="$PREFIXES" -v ops="$STATEOPS" '
            /^[ \t]*[0-9a-f]+:/ {
                seen++
                text = $0
                sub(/^[^:]*:[ \t]*/, "", text)
                sub(/[ \t]*#.*$/, "", text)
                if (text == "") { next }
                rest = text
                mnem = rest
                sub(/[ \t].*$/, "", mnem)
                while (mnem ~ pfx) {
                    if (!match(rest, /[ \t]/)) { mnem = ""; break }
                    rest = substr(rest, RSTART + 1)
                    sub(/^[ \t]*/, "", rest)
                    mnem = rest
                    sub(/[ \t].*$/, "", mnem)
                }
                if (mnem == "") { next }
                hit = 0
                if (text ~ /%(x|y|z)mm[0-9]/) { hit = 1 }
                if (text ~ /%mm[0-7]/) { hit = 1 }
                if (text ~ /%st/) { hit = 1 }
                if (mnem ~ /^f/) { hit = 1 }
                if (mnem ~ ops) { hit = 1 }
                if (hit) { print "hit " mnem " " text }
            }
            END { print "insn " seen + 0 }' > "$TMP/census.raw"
    sed -n 's/^hit //p' "$TMP/census.raw" > "$2"
    KOS_INSN_THIS="$(sed -n 's/^insn //p' "$TMP/census.raw")"
    require_number "$KOS_INSN_THIS" "the instruction count for $1"
    printf '%s\n' "$KOS_INSN_THIS" >> "$TMP/insn"
}

# Executable sections of nonzero size in one input, from the section headers. The size test is
# textual because a hex field is what objdump prints and a zero-length .text is exactly the
# case being separated; awk's strtonum is a gawk extension and is not available here.
code_sections() { # <input>
    "$OBJDUMP" -h "$1" > "$TMP/hdrs" 2>/dev/null \
        || fail "$OBJDUMP -h failed on $1, so whether that input carries text is UNKNOWN and
      an empty disassembly of it cannot be told from a clean one"
    awk '
        /^[ \t]*[0-9]+[ \t]+[^ \t]+[ \t]+[0-9a-f]+/ { sz = $3; hold = 1; next }
        hold { if (index($0, "CODE") > 0 && sz !~ /^0+$/) { n++ } hold = 0 }
        END { print n + 0 }' "$TMP/hdrs"
}

lines() { # <file>
    wc -l < "$1" | tr -d ' '
}

# --- the detector, before it is asked to report an absence --------------------
# An absence-assertion whose detector has never fired is not evidence. Both legs must fire: a
# `double` and a struct copy for the vector one, a `long double` for the x87 one. Built at the
# compiler's own defaults rather than the board's posture, which is the point of a control.
cat > "$TMP/dirty.c" <<'EOF'
struct kos_ctl_blob { char b[64]; };
void kos_ctl_copy(struct kos_ctl_blob* d, const struct kos_ctl_blob* s) { *d = *s; }
double kos_ctl_sse(double a, double b) { return a * b + a / b; }
long double kos_ctl_x87(long double a, long double b) { return a * b + a / b; }
EOF
cat > "$TMP/clean.c" <<'EOF'
static unsigned long kos_ctl_state;
unsigned long kos_ctl_read(unsigned long a) { kos_ctl_state += a; return kos_ctl_state; }
EOF
: > "$TMP/insn"
for n in dirty clean; do
    "$CC" -O2 -ffreestanding -c -o "$TMP/$n.o" "$TMP/$n.c" \
        || fail "$CC could not compile $TMP/$n.c"
done

census "$TMP/dirty.o" "$TMP/dirty.hits"
N_VEC="$(grep -cE '%(x|y|z)mm[0-9]' "$TMP/dirty.hits" || true)"
N_X87="$(grep -cE '^f' "$TMP/dirty.hits" || true)"
[ "$N_VEC" -gt 0 ] \
    || fail "no vector instruction in the control object reached this detector, so the vector
      leg would report an absence it cannot measure"
[ "$N_X87" -gt 0 ] \
    || fail "no x87 instruction in the control object reached this detector, so the x87 leg
      would report an absence it cannot measure"

census "$TMP/clean.o" "$TMP/clean.hits"
[ ! -s "$TMP/clean.hits" ] \
    || fail "the detector reported $(lines "$TMP/clean.hits") hit(s) in an integer-only
      object, so it refuses text that is neither vector nor x87"

# The same detector over a real host binary, where the scale shows: an ordinary distribution
# build of this very disassembler is full of them.
HOSTBIN="$(command -v "$OBJDUMP" 2>/dev/null)"
if [ -z "$HOSTBIN" ]; then
    HOSTBIN="$OBJDUMP"
fi
census "$HOSTBIN" "$TMP/host.hits"
N_HOSTHIT="$(lines "$TMP/host.hits")"
[ "$N_HOSTHIT" -ge "$HOST_FLOOR" ] \
    || fail "the detector found $N_HOSTHIT hit(s) in $HOSTBIN, under the floor of $HOST_FLOOR:
      an ordinary host binary carries hundreds, so the detector is broken"

# The section oracle, both ways round, before the loop below is allowed to read a silent
# disassembly as an absence. An object built from data alone carries a .text of length zero,
# which is the shape the sixteen data-only objects of this build have.
cat > "$TMP/dataonly.c" <<'EOF'
const unsigned long kos_ctl_table[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
EOF
"$CC" -O2 -ffreestanding -c -o "$TMP/dataonly.o" "$TMP/dataonly.c" \
    || fail "$CC could not compile $TMP/dataonly.c"
N_CS_DIRTY="$(code_sections "$TMP/dirty.o")"
N_CS_DATA="$(code_sections "$TMP/dataonly.o")"
require_number "$N_CS_DIRTY" "the code-section count of the control object"
require_number "$N_CS_DATA" "the code-section count of the data-only control object"
[ "$N_CS_DIRTY" -gt 0 ] \
    || fail "the section oracle reports no executable section in an object full of them, so
      every input below would be excused from decoding anything"
[ "$N_CS_DATA" -eq 0 ] \
    || fail "the section oracle reports $N_CS_DATA executable section(s) in a data-only
      object, so it would demand instructions of every data-only object in the build"
census "$TMP/dataonly.o" "$TMP/dataonly.hits"
[ "$KOS_INSN_THIS" -eq 0 ] \
    || fail "the disassembler decoded $KOS_INSN_THIS instruction(s) out of a data-only object,
      so the two oracles below disagree on the case they exist to separate"

echo "== control: $N_VEC vector and $N_X87 x87 hit(s) in a purpose-built object, 0 in an integer-only one, $N_HOSTHIT in $HOSTBIN =="
echo "== control: $N_CS_DIRTY code section(s) in that object and $N_CS_DATA in a data-only one, which decodes to 0 instruction(s) =="

# --- the corpus ---------------------------------------------------------------
# An object of a target of this project lives under CMakeFiles/<target>.dir/, which is what
# separates it from anything CMake leaves beside its own compiler probes.
find "$BUILD" -path '*.dir/*' \( -name '*.o' -o -name '*.obj' \) -type f \
    | sort > "$TMP/objects" || fail "the object walk of $BUILD failed"
find "$BUILD" -name '*.efi' -type f | sort > "$TMP/images" \
    || fail "the image walk of $BUILD failed"

N_OBJ="$(lines "$TMP/objects")"
N_IMG="$(lines "$TMP/images")"
[ "$N_OBJ" -ge "$OBJ_FLOOR" ] \
    || fail "$N_OBJ object(s) under $BUILD, beneath the floor of $OBJ_FLOOR: this tree is
      unbuilt, or the walk found the wrong directory. A corpus that size asserts nothing."
[ "$N_IMG" -ge "$IMG_FLOOR" ] \
    || fail "$N_IMG image(s) under $BUILD, beneath the floor of $IMG_FLOOR: the links did not
      run, so whatever the linker put in the image is unread."

cat "$TMP/objects" "$TMP/images" > "$TMP/corpus"
N_INPUT=$((N_OBJ + N_IMG))

# --- the scan -----------------------------------------------------------------
: > "$TMP/insn"
: > "$TMP/findings"
N_HIT=0
N_DIRTY=0
N_NOCODE=0
while IFS= read -r input; do
    # objdump names the architecture of a file it opened. One it could not read yields no
    # instruction at all, which an absence-assertion takes for clean, so this is per input
    # and never a total over the loop.
    "$OBJDUMP" -f "$input" 2>/dev/null | grep -q '^architecture: i386:x86-64' \
        || fail "$OBJDUMP reports no i386:x86-64 architecture for $input, so the scan of that
      input read a dead tool, or a file of another machine, as clean"
    N_CS="$(code_sections "$input")"
    require_number "$N_CS" "the code-section count for $input"
    census "$input" "$TMP/hits"
    if [ "$N_CS" -gt 0 ] && [ "$KOS_INSN_THIS" -eq 0 ]; then
        fail "$input carries $N_CS executable section(s) of nonzero size and decoded to no
      instruction at all, so that input went UNREAD and its verdict is not clean"
    fi
    if [ "$N_CS" -eq 0 ] && [ "$KOS_INSN_THIS" -gt 0 ]; then
        fail "$input carries no sized executable section and yet decoded to $KOS_INSN_THIS
      instruction(s): the section headers and the disassembly disagree, so neither can be
      used to tell an unread input from a data-only one"
    fi
    if [ "$N_CS" -eq 0 ]; then
        N_NOCODE=$((N_NOCODE + 1))
    fi
    if [ -s "$TMP/hits" ]; then
        N_THIS="$(lines "$TMP/hits")"
        N_DIRTY=$((N_DIRTY + 1))
        N_HIT=$((N_HIT + N_THIS))
        MNEMS="$(awk '{ print $1 }' "$TMP/hits" | sort | uniq -c | sort -rn | tr '\n' ' ')"
        echo "${input#"$BUILD"/}: $N_THIS hit(s), by mnemonic: $MNEMS" >> "$TMP/findings"
        # The mnemonic leads each record so the tally above can count it; the sample prints
        # the instruction text alone.
        sed -n '1,6p' "$TMP/hits" | sed 's/^[^ ]* /    /' >> "$TMP/findings"
    fi
done < "$TMP/corpus"

N_INSN="$(awk '{ n += $1 } END { print n + 0 }' "$TMP/insn")"
[ "$N_INSN" -ge "$INSN_FLOOR" ] \
    || fail "$N_INSN instruction(s) decoded over $N_INPUT input(s), beneath the floor of
      $INSN_FLOOR: the disassembler produced almost nothing, so this scan read no text."

echo "== $N_OBJ object(s) and $N_IMG image(s) under $BUILD, $N_INSN instruction(s) decoded; $N_NOCODE input(s) carry no text and decoded none =="

if [ -s "$TMP/findings" ]; then
    cat "$TMP/findings" >&2
    echo "" >&2
    fail "$N_HIT vector or x87 instruction(s) in $N_DIRTY of $N_INPUT input(s).
      arch/x86/x86_64/switch.S and trap_x86_64.S save no XMM and no FPU state, and
      arch/x86/x86_64/entry_x86_64.cc refuses the whole family in CR0 and CR4, so each of
      these is a live register a context switch drops and a fault at ring 0. Restore
      -mno-sse -mno-mmx -mno-80387 on the freestanding posture in
      cmake/toolchain-x86_64-uefi.cmake, or take the float out of the source that made the
      compiler reach for one."
fi

echo "PASS: no vector or x87 instruction in the x86_64 freestanding objects or images"
