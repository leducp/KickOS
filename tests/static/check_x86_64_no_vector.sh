#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Refuse any vector or x87 instruction in the x86_64 kernel half but the ones the port owns,
# admitted by site.
#
#   tests/static/check_x86_64_no_vector.sh <objdump> <cc> <build-dir>
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
# The corpus is the LINKED IMAGES, because what -mgeneral-regs-only cannot reach is there and
# nowhere else: assembly, inline assembly, toolchain archive members and the text the linker
# synthesises. Every application image is read outside its app window, .apptext, and every
# probe image whole, those having no app half of their own. An application image is one whose
# link wrote its map beside it (tools/x86_64-link.sh); a probe image carries none.
#
# The split is a control of its own: the app half of the selftest image must decode at least
# one vector instruction outside the arms' own assembly, selftest_vec_* and bench_vec_*, or the
# applications are not compiled with vectors and this gate reads nothing it did not read before.
#
# An image carrying an executable section of nonzero size must decode at least one
# instruction, or objdump could not read it and its silence is not clean.

set -u
. "$(dirname "$0")/../lib/gate.sh"

# A tree whose links did not run leaves the text the linker contributes unread.
IMG_FLOOR=8

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
                print "hit " mnem " <" fn "> " text
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

# The planted control, built at the compiler's own defaults: a struct copy and a `double` for
# the vector leg, a `long double` for the x87 one.
cat > "$TMP/dirty.c" <<'EOF'
struct kos_ctl_blob { char b[64]; };
void kos_ctl_copy(struct kos_ctl_blob* d, const struct kos_ctl_blob* s) { *d = *s; }
double kos_ctl_sse(double a, double b) { return a * b + a / b; }
long double kos_ctl_x87(long double a, long double b) { return a * b + a / b; }
EOF
"$CC" -O2 -ffreestanding -c -o "$TMP/dirty.o" "$TMP/dirty.c" \
    || fail "$CC could not compile $TMP/dirty.c"
: > "$TMP/insn"
census "$TMP/dirty.o" "$TMP/dirty.hits"
grep -qE '%(x|y|z)mm[0-9]' "$TMP/dirty.hits" && grep -qE '^f' "$TMP/dirty.hits" \
    || fail "the planted object's vector or x87 instructions did not reach this detector, so it
      would report an absence it cannot measure"

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
N_IMG="$(lines "$TMP/images")"
[ "$N_IMG" -ge "$IMG_FLOOR" ] \
    || fail "$N_IMG image(s) under $BUILD, beneath the floor of $IMG_FLOOR: the links did not
      run, so whatever the linker put in the image is unread."
[ -s "$TMP/appimages" ] \
    || fail "no image under $BUILD carries the map tools/x86_64-link.sh writes, so no
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
    sed 's/$/ whole/' "$TMP/probeimages"
    sed 's/$/ kernel/' "$TMP/appimages"
} > "$TMP/corpus"
N_INPUT="$(lines "$TMP/corpus")"
: > "$TMP/insn"
: > "$TMP/findings"
: > "$TMP/seen"
: > "$TMP/armhits"
N_HIT=0
N_DIRTY=0
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
echo "== $N_IMG image(s) under $BUILD, $(lines "$TMP/appimages") read outside their app window; $N_INSN instruction(s) decoded =="

# A site no input carries was never counted, which reads exactly like a site at its count, and a
# function that lost every one of its sites is counted nowhere.
for fn in $(awk '{ print $1 }' "$TMP/sites" | sort -u); do
    grep -qx "$fn" "$TMP/seen" \
        || fail "no input under $BUILD carries $fn with its vector-state instructions, so they
      were never counted. The function was renamed or moved, lost every one of them, or the
      corpus lost the images holding it"
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
