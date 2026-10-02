#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Refuse any vector or x87 instruction in the x86_64 kernel half but the ones the port owns,
# admitted by site.
#
#   tests/static/check_x86_64_no_vector.sh <objdump> <cc> <build-dir> <pe-script>
#
# Every thread runs with x87, SSE and AVX live and both switch paths save that state
# (arch/x86/x86_64/switch.S, docs/design-m10-toolchain.md section 5). The applications are
# compiled with vectors and the kernel half with -mgeneral-regs-only, and the kernel half must
# never touch the state it saves for the threads. So a vector or x87 instruction there is
# refused unless it is one of the port's own: XSAVE64 and XRSTOR64 in the two switch paths and
# the first-thread start, XSETBV and XGETBV in the enable. Each is admitted by the function
# carrying it and counted wherever that function carries any of them, and every site must be
# found at its count somewhere in the corpus, so a missing site refuses like an extra one. A
# same-named body carrying none, the kernel-free images' declining kickos_x86_64_isr, is not a
# site.
#
# The corpus is DERIVED, never listed here: the objects of the targets <pe-script> claims for
# the kernel's .text, which is what puts them in the kernel half; the executable sections of
# every application image outside its app window, .apptext, the images being where the linker
# is free to synthesise text of its own; and every probe image whole, those having no app half
# of their own. An application image is one kickos_x86_64_link_image linked, which writes its
# map beside it; a probe image carries none.
#
# The split is a control of its own: the app half of the selftest image must decode at least
# one vector instruction outside the arms' own assembly, selftest_vec_* and bench_vec_*, or the
# applications are not compiled with vectors and this gate reads nothing it did not read before.
#
# THREE FLOORS, because a gate handed nothing must go red rather than clean: an unbuilt tree, a
# wrong directory and a corpus of archives alone each go red. Every run prints all three
# counts. Each claimed target must contribute an object as well, or its claim matched nothing.
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

if [ "$#" -ne 4 ]; then
    fail "usage: check_x86_64_no_vector.sh <objdump> <cc> <build-dir> <pe-script>"
fi
OBJDUMP="$1"
CC="$2"
BUILD="${3%/}"
PESCRIPT="$4"

command -v "$OBJDUMP" >/dev/null 2>&1 || [ -x "$OBJDUMP" ] || fail "no objdump at $OBJDUMP"
command -v "$CC" >/dev/null 2>&1 || [ -x "$CC" ] || fail "no compiler at $CC"
[ -d "$BUILD" ] || fail "no build directory at $BUILD"
[ -r "$PESCRIPT" ] || fail "no PE section script at $PESCRIPT"

scratch_dir

# The instruction prefixes objdump prints as words of their own, dropped so that the word after
# them is read as the mnemonic. A prefix not listed here hides the mnemonic behind it.
PREFIXES='^(lock|rep|repz|repnz|repe|repne|bnd|notrack|data16|data32|addr32|rex([.][A-Z]+)?|cs|ds|es|fs|gs|ss)$'

# The vector and x87 state instructions that name no such register in their operands, in the
# spellings objdump prints, the 64-bit forms included. Every other x87 mnemonic begins with f,
# which the awk below matches as a prefix.
STATEOPS='^(emms|femms|ldmxcsr|stmxcsr|xsave|xsave64|xsaveopt|xsaveopt64|xsavec|xsavec64|xsaves|xsaves64|xrstor|xrstor64|xrstors|xrstors64|xsetbv|xgetbv|vzeroupper|vzeroall)$'

# The port's own state instructions: the function carrying each and how many it carries.
cat > "$TMP/sites" <<'SITES'
kickos_x86_64_switch_now xsave64 1
kickos_x86_64_switch_now xrstor64 1
kickos_x86_64_start xrstor64 1
kickos_x86_64_isr xsave64 1
kickos_x86_64_isr xrstor64 1
kickos_x86_64_fp_enable xsetbv 1
kickos_x86_64_fp_enable xgetbv 1
SITES

# The arms' own assembly, which the split control does not count.
ARMS='^(selftest|bench)_vec_'

# Disassemble one input into <hits> (one `<mnemonic> <text>` line per refused instruction, and
# `site <function> <mnemonic> <found> <owed>` for a site whose count is wrong) and append the
# number of instructions decoded to $TMP/insn, which is the corpus-wide control that the tool
# ran. The admitted sites the input carries go to $TMP/seen, and the count of hits in the arms'
# assembly, which is no site, to $TMP/armhits. Any further arguments name the only sections
# disassembled, as objdump's -j does.
census() { # <input> <hits> [section...]
    _in="$1"
    _out="$2"
    shift 2
    _only=""
    for _sec in "$@"; do
        _only="$_only -j $_sec"
    done
    # shellcheck disable=SC2086
    "$OBJDUMP" -d --no-show-raw-insn $_only "$_in" 2>/dev/null \
        | awk -v pfx="$PREFIXES" -v ops="$STATEOPS" -v sitesf="$TMP/sites" -v arms="$ARMS" '
            BEGIN {
                while ((getline line < sitesf) > 0) {
                    split(line, f, " ")
                    owed[f[1] " " f[2]] = f[3]
                    site[f[1]] = 1
                }
            }
            /^[0-9a-f]+ <.*>:$/ {
                fn = $2
                sub(/^</, "", fn)
                sub(/>:$/, "", fn)
                if (fn in site) { present[fn] = 1 }
                next
            }
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
                if (!hit) { next }
                if (fn ~ arms) { armhits++ }
                if ((fn " " mnem) in owed) { found[fn " " mnem]++; next }
                print "hit " mnem " " text
            }
            END {
                for (k in owed) {
                    split(k, f, " ")
                    carried[f[1]] += found[k]
                }
                for (k in owed) {
                    split(k, f, " ")
                    if (!(f[1] in present) || carried[f[1]] == 0) { continue }
                    if (found[k] + 0 != owed[k]) {
                        print "hit site " k " " found[k] + 0 " " owed[k]
                    }
                }
                for (k in present) { if (carried[k] > 0) { print "seen " k } }
                print "arms " armhits + 0
                print "insn " seen + 0
            }' > "$TMP/census.raw"
    sed -n 's/^hit //p' "$TMP/census.raw" > "$_out"
    sed -n 's/^seen //p' "$TMP/census.raw" >> "$TMP/seen"
    sed -n 's/^arms //p' "$TMP/census.raw" >> "$TMP/armhits"
    KOS_INSN_THIS="$(sed -n 's/^insn //p' "$TMP/census.raw")"
    require_number "$KOS_INSN_THIS" "the instruction count for $_in"
    printf '%s\n' "$KOS_INSN_THIS" >> "$TMP/insn"
}

# Executable sections of nonzero size in one input, from the section headers, by name one per
# line into <names>, and their count on stdout. The size test is textual because a hex field is
# what objdump prints and a zero-length .text is exactly the case being separated; awk's
# strtonum is a gawk extension and is not available here. A section named by <skip> is left
# out.
code_sections() { # <input> [names] [skip]
    "$OBJDUMP" -h "$1" > "$TMP/hdrs" 2>/dev/null \
        || fail "$OBJDUMP -h failed on $1, so whether that input carries text is UNKNOWN and
      an empty disassembly of it cannot be told from a clean one"
    awk -v skip="${3:-}" -v names="${2:-/dev/null}" '
        /^[ \t]*[0-9]+[ \t]+[^ \t]+[ \t]+[0-9a-f]+/ { nm = $2; sz = $3; hold = 1; next }
        hold {
            if (index($0, "CODE") > 0 && sz !~ /^0+$/ && nm != skip) {
                n++
                print nm > names
            }
            hold = 0
        }
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

# The site counter and the arms' admission, both ways round: the port's own sites at their
# owed counts and an arm pass; one more or one fewer, and a state instruction in a function of
# another name, refuse.
site_control() { # <name> <verdict: pass|refuse> <assembly>
    printf '%s\n' "$3" > "$TMP/$1.s"
    "$CC" -c -o "$TMP/$1.o" "$TMP/$1.s" || fail "$CC could not assemble $TMP/$1.s"
    : > "$TMP/seen"
    : > "$TMP/armhits"
    census "$TMP/$1.o" "$TMP/$1.hits"
    if [ "$2" = pass ] && [ -s "$TMP/$1.hits" ]; then
        fail "the site control '$1' reported [$(sed -n 1p "$TMP/$1.hits")] where the port's own
      sites and the arms are admitted, so every image carrying them would refuse"
    fi
    if [ "$2" = refuse ] && [ ! -s "$TMP/$1.hits" ]; then
        fail "the site control '$1' passed, so a state instruction the port does not own, or a
      site carrying the wrong count, would read as clean"
    fi
}
site_control site_ok pass '.text
.globl kickos_x86_64_switch_now
kickos_x86_64_switch_now:
    xsave64 64(%rdi)
    xrstor64 64(%rsi)
    ret
.globl kickos_x86_64_start
kickos_x86_64_start:
    xrstor64 64(%rdi)
    ret'
grep -qx 'kickos_x86_64_start' "$TMP/seen" \
    || fail "the site control's kickos_x86_64_start was not recorded as seen, so the corpus-wide
      check that every site was found reads nothing"
site_control site_decline pass '.text
.globl kickos_x86_64_isr
kickos_x86_64_isr:
    xorl %eax, %eax
    ret'
site_control site_extra refuse '.text
.globl kickos_x86_64_start
kickos_x86_64_start:
    xrstor64 64(%rdi)
    xrstor64 64(%rdi)
    ret'
site_control site_missing refuse '.text
.globl kickos_x86_64_isr
kickos_x86_64_isr:
    xrstor64 64(%rdi)
    ret'
site_control site_elsewhere refuse '.text
.globl kickos_x86_64_resume
kickos_x86_64_resume:
    xsetbv
    ret'
# The arms' assembly is outside the corpus, never admitted in it.
site_control site_arm refuse '.text
.globl selftest_vec_ctl
selftest_vec_ctl:
    vmovdqu %ymm0, (%rdi)
    ret'
[ "$(cat "$TMP/armhits")" = 1 ] \
    || fail "the arm control counted $(cat "$TMP/armhits") instruction(s) of its 1 as the arms',
      so the split control below cannot leave the arms' own assembly out"

echo "== control: $N_VEC vector and $N_X87 x87 hit(s) in a purpose-built object, 0 in an integer-only one, $N_HOSTHIT in $HOSTBIN =="
echo "== control: the port's sites at their counts and a declining body pass; an extra, a missing, a misplaced site and an arm refuse =="
echo "== control: $N_CS_DIRTY code section(s) in that object and $N_CS_DATA in a data-only one, which decodes to 0 instruction(s) =="

# --- the corpus ---------------------------------------------------------------
# The kernel half's targets, as the PE script claims their text: an archive by its file name, an
# object library by the directory CMake compiles it into.
grep -E '\(\.text \.text\.\*\)' "$PESCRIPT" \
    | sed -n 's|^[[:space:]]*\*lib\([a-z0-9_]*\)\.a:.*|\1|p; s|^[[:space:]]*\*\([a-z0-9_]*\)\.dir/.*|\1|p' \
    | sort -u > "$TMP/claims"
grep -qx kickos_kernel "$TMP/claims" \
    || fail "$PESCRIPT claims no kickos_kernel text, so the kernel half this gate derives from it
      is not the kernel's. The claim lines changed shape"
: > "$TMP/objects"
while IFS= read -r t; do
    find "$BUILD" -path "*/CMakeFiles/$t.dir/*" \( -name '*.o' -o -name '*.obj' \) -type f \
        | sort > "$TMP/claimed" || fail "the object walk of $BUILD failed"
    [ -s "$TMP/claimed" ] \
        || fail "the kernel-half target $t, which $PESCRIPT claims, compiled no object under
      $BUILD, so its claim names nothing this gate can read"
    cat "$TMP/claimed" >> "$TMP/objects"
done < "$TMP/claims"
find "$BUILD" -name '*.efi' -type f | sort > "$TMP/images" \
    || fail "the image walk of $BUILD failed"
: > "$TMP/appimages"
: > "$TMP/probeimages"
while IFS= read -r img; do
    if [ -f "$img.map" ]; then
        echo "$img" >> "$TMP/appimages"
    else
        echo "$img" >> "$TMP/probeimages"
    fi
done < "$TMP/images"

N_OBJ="$(lines "$TMP/objects")"
N_IMG="$(lines "$TMP/images")"
N_APPIMG="$(lines "$TMP/appimages")"
[ "$N_OBJ" -ge "$OBJ_FLOOR" ] \
    || fail "$N_OBJ kernel-half object(s) under $BUILD, beneath the floor of $OBJ_FLOOR: this tree
      is unbuilt, or the walk found the wrong directory. A corpus that size asserts nothing."
[ "$N_IMG" -ge "$IMG_FLOOR" ] \
    || fail "$N_IMG image(s) under $BUILD, beneath the floor of $IMG_FLOOR: the links did not
      run, so whatever the linker put in the image is unread."
[ "$N_APPIMG" -gt 0 ] \
    || fail "no image under $BUILD carries the map kickos_x86_64_link_image writes, so no
      application image's kernel half is in the corpus"

# --- the split -----------------------------------------------------------------
# The selftest image's app window, where the compiler's own vector code must be by now.
SPLIT_IMG="$(grep '/selftest\.efi$' "$TMP/appimages" | sed -n 1p)"
[ -n "$SPLIT_IMG" ] || fail "no selftest image under $BUILD, so the split control reads nothing"
: > "$TMP/insn"
: > "$TMP/armhits"
census "$SPLIT_IMG" "$TMP/split.hits" .apptext
N_SPLIT_ALL="$(lines "$TMP/split.hits")"
N_SPLIT_ARM="$(awk '{ n += $1 } END { print n + 0 }' "$TMP/armhits")"
N_SPLIT=$((N_SPLIT_ALL - N_SPLIT_ARM))
[ "$N_SPLIT" -gt 0 ] \
    || fail "the app window of $SPLIT_IMG decodes $N_SPLIT_ALL vector or x87 instruction(s), all
      of them the arms' own assembly, so the applications are not compiled with vectors: the
      split of cmake/toolchain-x86_64-uefi.cmake did not happen and this gate reads nothing new"
echo "== control: the app window of ${SPLIT_IMG#"$BUILD"/} decodes $N_SPLIT vector or x87 instruction(s) the compiler chose, beside $N_SPLIT_ARM in the arms =="

# --- the scan -----------------------------------------------------------------
# One line per input: the input, then `whole`, or `kernel` for an application image, whose app
# window is left out.
{
    sed 's/$/ whole/' "$TMP/objects" "$TMP/probeimages"
    sed 's/$/ kernel/' "$TMP/appimages"
} > "$TMP/corpus"
N_INPUT="$(lines "$TMP/corpus")"
: > "$TMP/insn"
: > "$TMP/findings"
: > "$TMP/seen"
: > "$TMP/armhits"
N_HIT=0
N_DIRTY=0
N_NOCODE=0
while read -r input scope; do
    # objdump names the architecture of a file it opened. One it could not read yields no
    # instruction at all, which an absence-assertion takes for clean, so this is per input
    # and never a total over the loop.
    "$OBJDUMP" -f "$input" 2>/dev/null | grep -q '^architecture: i386:x86-64' \
        || fail "$OBJDUMP reports no i386:x86-64 architecture for $input, so the scan of that
      input read a dead tool, or a file of another machine, as clean"
    if [ "$scope" = kernel ]; then
        N_CS="$(code_sections "$input" "$TMP/secs" .apptext)"
        require_number "$N_CS" "the code-section count for $input"
        # shellcheck disable=SC2046
        census "$input" "$TMP/hits" $(cat "$TMP/secs")
    else
        N_CS="$(code_sections "$input")"
        require_number "$N_CS" "the code-section count for $input"
        census "$input" "$TMP/hits"
    fi
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

echo "== $N_OBJ object(s) of $(lines "$TMP/claims") kernel-half target(s), $N_IMG image(s) of which $N_APPIMG read outside their app window, under $BUILD; $N_INSN instruction(s) decoded; $N_NOCODE input(s) carry no text and decoded none =="

# A site no input carries was never counted, which reads exactly like a site at its count, and a
# function that lost every one of its sites is counted nowhere.
for fn in $(awk '{ print $1 }' "$TMP/sites" | sort -u); do
    grep -qx "$fn" "$TMP/seen" \
        || fail "no input under $BUILD carries $fn with its vector-state instructions, so they
      were never counted. The function was renamed or moved, lost every one of them, or the
      corpus lost the objects holding it"
done
echo "== the port's $(lines "$TMP/sites") site(s) counted in every input carrying them =="

if [ -s "$TMP/findings" ]; then
    cat "$TMP/findings" >&2
    echo "" >&2
    fail "$N_HIT vector or x87 finding(s) in $N_DIRTY of $N_INPUT input(s). An instruction
      outside the port's sites is vector state the kernel half must never touch, and a site
      line names a function whose count of a state instruction moved. Give the target
      KICKOS_X86_64_KERNEL_FLAGS (cmake/x86_64_boot.cmake), take the float out of the source
      that made the compiler reach for one, or restate the site table above with the switch
      path."
fi

echo "PASS: no vector or x87 instruction in the x86_64 kernel half but the port's own sites"
