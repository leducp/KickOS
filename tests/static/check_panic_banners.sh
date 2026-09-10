#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Every fault banner a reporter can put on the wire is matched by tests/lib/panic.ere.
# DERIVED from the emit sites, never from a second list: the ERE is what makes
# assert_no_panic and the sim FAIL_REGULAR_EXPRESSION see a fault at all, so a banner it
# misses turns a board that died into a board that passed.
#
# Run from the repo root, no arguments: tests/static/check_panic_banners.sh
#
# THE MATCH: a single-line string literal that BOTH opens with `\n=== ` and carries a
# closing ` ===` after that, which separates an emit site from prose ABOUT one with no
# by-name exemption list to maintain. A comment that spells a WHOLE banner including its
# `\n` escape DOES report, and the fix is to stop spelling an escape sequence in prose.
#
# THE NAME: where the banner name is itself the conversion, `\n=== %s ===`, the labels come
# from the argument NAME on the emit line and from every `<name> = "..."` assignment in the
# same file. Every other conversion is substituted with a placeholder, the ERE keying on the
# fixed prefix.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# Findings accumulate over the whole corpus, so set -e must stay off.

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

# THE EXCLUSIONS, and they are one ruling: these banners say a thread died and the SYSTEM DID
# NOT, so a gate reads them while asserting no panic occurred and the ERE must not match them.
#   - the thread-fault report, read by check_rootfault.sh, check_mpu_fault.sh,
#     check_faultsurvive.sh and check_qemu_ringppb.sh;
#   - every CONTAINED refusal, which the trap entry resumes from: the arch reporters spell the
#     noun rather than substitute it precisely so this scan can see which outcome each is.
EXCLUDED_RE='=== THREAD FAULT ===| CONTAINED '

# Files that MUST each yield a banner, one per fault reporter in the tree, so a shape that
# stops matching cannot read as "no reporter emits a banner any more".
REPORTERS='arch/arm/armv6m/arch_armv6m.cc
arch/arm/armv6m/isa/arm_isa.h
arch/arm/armv7m/arch_armv7m.cc
arch/arm/armv7m/isa/arm_isa.h
arch/arm64/armv8a/arch_armv8a.cc
arch/riscv/rv32imac/arch_rv32imac.cc
arch/riscv/rv64imac/arch_rv64imac.cc
arch/rx/rxv3/arch_rxv3.cc
arch/x86/x86_64/fault_x86_64.cc
arch/xtensa/lx6/arch_xtensa.cc
arch/sim/sim.cc'

# The armv7m reporter picks between HARD, MPU and BUS. Fewer resolved labels means the
# label scan went vacuous, which would otherwise read as a clean tree.
MIN_LABELS=3

scratch_dir

# THE CORPUS IS A TOTAL CLASSIFICATION OF THE TREE AND NOT A LIST OF ROOTS, because every
# other leg here is satisfied by arch/ alone: with kernel/ and system/ absent the REPORTERS
# leg, the MIN_LABELS leg and the ERE leg all still pass, so a root that leaves the corpus
# has to REFUSE rather than shrink it in silence. Every top-level directory tracking source
# is `read`, or `skip` with the reason it holds no fault reporter, and one this gate has
# never been told about stops the run.
corpus_class() { # <tracked path> -> read | skip | refuse
    case "${1%%/*}" in
        arch|kernel|include|lib|system)
            printf 'read\n' ;;
        # user/ and examples/ are application code and boards/ is board glue: an application
        # printing its own message is not a fault reporter. tests/ and tools/ are host-side,
        # where a banner-shaped literal is a gate's own planted control.
        user|examples|boards|tests|tools)
            printf 'skip\n' ;;
        *)
            printf 'refuse\n' ;;
    esac
}

# The six needles the extractor keys on, spelled once and handed to it. The opening literal
# is ESC then MARK behind a quote, and the name slot is MARK then CONV: one spelling each, so
# a clause cannot be disabled in one of the two places that read it.
B_ESC='\\n'
B_MARK='=== '
B_CLOSE=' ==='
B_TRUNC='\\n'
B_CONV='%s'
B_ASSIGN='[ \t]*=[ \t]*"[^"]*"'

# Emits one tab-separated record per resolved banner:
#   <file> <line> <slot> <banner text>
# slot is `fixed` for a literal name and `label` for one substituted into a `%s` name slot.
extract() { # <file> <esc> <mark> <close> <trunc> <conv> <assign>
    awk -v FNAME="$1" -v ESC="$2" -v MARK="$3" -v CLOSE="$4" -v TRUNC="$5" \
        -v CONV="$6" -v ASSIGN="$7" '
        # Placeholder for every conversion that is not the name slot. The ERE keys on the
        # fixed prefix, so the value only has to be free of regex-significant bytes.
        function subst_convs(s,   out, i, c) {
            out = ""
            i = 1
            while (i <= length(s)) {
                c = substr(s, i, 1)
                if (c == "%" && i < length(s)) {
                    # %% is a literal percent; anything else is one conversion character
                    # after optional flag/width bytes, and this tree uses none.
                    if (substr(s, i + 1, 1) == "%") {
                        out = out "%"
                    } else {
                        out = out "X"
                    }
                    i = i + 2
                    continue
                }
                out = out c
                i = i + 1
            }
            return out
        }
        # The wire text of a banner literal: from the `===` up to the first `\n` escape
        # after it, so a multi-line format contributes only its banner line.
        function banner_line(body,   rest, p) {
            rest = substr(body, DROP)       # drop the leading escape
            p = index(rest, TRUNC)
            if (p > 0) {
                rest = substr(rest, 1, p - 1)
            }
            return rest
        }
        BEGIN {
            OPEN = "\"" ESC MARK
            DROP = length(ESC) + 1
            BPOS = length(ESC) + length(MARK)
            NAME = MARK CONV " "
            NLEN = length(NAME)
        }
        { line[NR] = $0 }
        END {
            for (n = 1; n <= NR; n++) {
                s = line[n]
                pos = 1
                while (1) {
                    # index() over the literal bytes, so no regex escaping is in play.
                    p = index(substr(s, pos), OPEN)
                    if (p == 0) { break }
                    start = pos + p - 1                  # the opening quote
                    rest = substr(s, start + 1)
                    q = index(rest, "\"")
                    if (q == 0) { pos = start + 1; continue }
                    body = substr(rest, 1, q - 1)
                    pos = start + q + 1
                    if (index(substr(body, BPOS), CLOSE) == 0) { continue }
                    text = banner_line(body)
                    if (substr(text, 1, NLEN) == NAME) {
                        # The name IS the conversion: take it from the argument name on this
                        # line and from every assignment to that name in this file.
                        tail = substr(s, start + q + 1)
                        sub(/^[ \t]*,[ \t]*/, "", tail)
                        if (match(tail, /^[A-Za-z_][A-Za-z_0-9]*/) == 0) { continue }
                        nm = substr(tail, 1, RLENGTH)
                        for (m = 1; m <= NR; m++) {
                            t = line[m]
                            while (match(t, nm ASSIGN)) {
                                a = substr(t, RSTART, RLENGTH)
                                t = substr(t, RSTART + RLENGTH)
                                if (match(a, /"[^"]*"/) == 0) { continue }
                                v = substr(a, RSTART + 1, RLENGTH - 2)
                                if (v == "") { continue }
                                out = text
                                sub(CONV, v, out)
                                printf "%s\t%d\tlabel\t%s\n", FNAME, n, subst_convs(out)
                            }
                        }
                        continue
                    }
                    printf "%s\t%d\tfixed\t%s\n", FNAME, n, subst_convs(text)
                }
            }
        }
    ' "$1"
}

scan() { # <file>, with the rule as it stands
    extract "$1" "$B_ESC" "$B_MARK" "$B_CLOSE" "$B_TRUNC" "$B_CONV" "$B_ASSIGN"
}

# One line per banner the ERE pair judges wrongly, as `<kind> <file> <line> <slot> <text>`.
# EXCLUDED is a survivable outcome the ERE matches, UNMATCHED a fault it does not. Both EREs
# are arguments so the self-test can disable one without touching tests/lib/panic.ere.
match_findings() { # <banners file> <excluded-ere> <panic-ere>
    while IFS="$TAB" read -r _f _n _slot _text; do
        if printf '%s\n' "$_text" | grep -qE "$2"; then
            if printf '%s\n' "$_text" | grep -qE "$3"; then
                printf 'EXCLUDED\t%s\t%s\t%s\t%s\n' "$_f" "$_n" "$_slot" "$_text"
            fi
            continue
        fi
        if ! printf '%s\n' "$_text" | grep -qE "$3"; then
            printf 'UNMATCHED\t%s\t%s\t%s\t%s\n' "$_f" "$_n" "$_slot" "$_text"
        fi
    done < "$1"
}

# --- self-test: prove every clause of the rule, one control per clause ---------
# The REPORTERS and MIN_LABELS legs below assert an ABSENCE over today's tree: they prove a
# file yielded something, never that the extractor reads the shape right. Each control here is
# a MINIMAL PAIR against the negative beside it, with an exact expected count.
cat > "$TMP/st_pos.cc" <<'EOF'
    kprintf("\n=== FROB FAULT ===\n");
    kprintf("\n=== FROB EXCEPTION (%s) ===\n  PC=0x%x\n", what, pc);
    kprintf("\n=== 100%% FROB FAULT ===\n");
    a("\n=== FROB A ===\n"); b("\n=== FROB B ===\n");
    // the reporter puts "\n=== FROB COMMENT ===" on the wire
    kprintf("\n=== %s ===\n", tag);
    tag = "FROB ONE";
    tag = "FROB TWO";
EOF
cat > "$TMP/st_neg.cc" <<'EOF'
    kprintf("\n=== FROB FAULT\n");
    kprintf("=== FROB FAULT ===\n");
    // see "\n=== FROB FAULT === with no closing quote
    kprintf("\n=== %s ===\n", 42);
    kprintf("\n=== %s ===\n", tag);
    kprintf("hello\n");
EOF

POS="$(scan "$TMP/st_pos.cc" | wc -l | tr -d ' ')"
[ "$POS" -eq 8 ] || fail "the extractor found $POS of 8 planted banners; it would miss real ones"

# The banner TEXT, not just the count: a multi-line format must contribute its banner line
# alone, and a doubled percent must survive as one.
scan "$TMP/st_pos.cc" | cut -f4 > "$TMP/st_pos_text"
for want in '=== FROB FAULT ===' '=== FROB EXCEPTION (X) ===' '=== 100% FROB FAULT ===' \
            '=== FROB A ===' '=== FROB B ===' '=== FROB COMMENT ===' \
            '=== FROB ONE ===' '=== FROB TWO ==='; do
    grep -qxF "$want" "$TMP/st_pos_text" \
        || fail "the extractor resolved no banner reading exactly [$want], so the text it
      hands tests/lib/panic.ere is not the text a reporter puts on the wire"
done

scan "$TMP/st_neg.cc" > "$TMP/st_negout"
if [ -s "$TMP/st_negout" ]; then
    sed 's/^/      /' "$TMP/st_negout" >&2
    fail "the extractor read a banner out of prose, a flagless literal or an unterminated
      one; every such line would demand an entry in tests/lib/panic.ere"
fi

# EACH negative on its own, so a control that is silent for the WRONG reason is visible. A
# whole-file zero cannot tell "six clauses work" from "one clause swallowed the file".
i=0
while IFS= read -r nline; do
    i=$((i + 1))
    printf '%s\n' "$nline" > "$TMP/st_one.cc"
    n="$(scan "$TMP/st_one.cc" | wc -l | tr -d ' ')"
    [ "$n" -eq 0 ] || fail "negative control $i reports: $nline"
done < "$TMP/st_neg.cc"
[ "$i" -eq 6 ] || fail "$i negative control(s) ran, expected 6"

# The label leg needs two lines to state, so its four controls are whole files. Each is read
# ALONE: the emit line is the same in all four and only the assignment beside it differs, so
# a count that does not move names the clause that failed.
printf '%s\n' 'tag = "FROB ONE";' 'tag = "FROB TWO";' '    kprintf("\n=== %s ===\n", tag);' \
    > "$TMP/st_lab_two.cc"
printf '%s\n' 'tag = "FROB ONE";' '    kprintf("\n=== %s ===\n", 42);' \
    > "$TMP/st_lab_ident.cc"
printf '%s\n' 'other = "FROB ONE";' '    kprintf("\n=== %s ===\n", tag);' \
    > "$TMP/st_lab_other.cc"
printf '%s\n' 'tag = "";' '    kprintf("\n=== %s ===\n", tag);' \
    > "$TMP/st_lab_empty.cc"
label_case() { # <file> <expected> <what the pair pins>
    _lc="$(scan "$1" | wc -l | tr -d ' ')"
    [ "$_lc" -eq "$2" ] || fail "the label control $1 resolved $_lc banner(s), expected $2:
      $3"
}
label_case "$TMP/st_lab_two.cc"   2 "two assignments to the name must each resolve, which is
      how a reporter picking between three labels is covered at all"
label_case "$TMP/st_lab_ident.cc" 0 "an argument that is not an identifier names no label"
label_case "$TMP/st_lab_other.cc" 0 "an assignment to another name resolves no label"
label_case "$TMP/st_lab_empty.cc" 0 "an empty assignment resolves no label"

# The mutation the controls exist to survive: respell one needle and the count over the
# control corpus must move to an EXACT number. Every arm but the last two hands a needle that
# cannot match; the two CLOSE and ESC arms hand a PLAUSIBLE WRONG SPELLING instead, since a
# clause that only ever reports LESS cannot show that a silent negative was near.
NEVER='KICKOS_THIS_NEEDLE_MATCHES_NOTHING'
mutate() { # <what> <file> <esc> <mark> <close> <trunc> <conv> <assign> <expect>
    _m="$(extract "$2" "$3" "$4" "$5" "$6" "$7" "$8" | wc -l | tr -d ' ')"
    [ "$_m" -eq "$9" ] || fail "with the $1 needle respelled the extractor read $_m banner(s)
      of $2, expected $9; the controls for it are not near misses and prove nothing"
}
# CONV: the `=== %s ===` emit falls back to one fixed banner, so pos loses a label and neg
# gains the two emits whose label never resolved.
mutate "name-slot" "$TMP/st_pos.cc" "$B_ESC" "$B_MARK" "$B_CLOSE" "$B_TRUNC" "$NEVER" "$B_ASSIGN" 7
mutate "name-slot" "$TMP/st_neg.cc" "$B_ESC" "$B_MARK" "$B_CLOSE" "$B_TRUNC" "$NEVER" "$B_ASSIGN" 2
# ASSIGN: the emit still takes the name slot and finds no value for it, so only the two
# labels go.
mutate "assignment" "$TMP/st_pos.cc" "$B_ESC" "$B_MARK" "$B_CLOSE" "$B_TRUNC" "$B_CONV" "$NEVER" 6
# CLOSE spelled as the escape, the shape a reader gets by taking the trailing `\n` for the
# terminator: the one negative that opens a banner and never closes it now reports.
mutate "closing" "$TMP/st_neg.cc" "$B_ESC" "$B_MARK" '\\n' "$B_TRUNC" "$B_CONV" "$B_ASSIGN" 1
# ESC dropped from the opening, the shape a reader gets by forgetting the escape is part of
# the literal: the one negative that spells the marker with no escape now reports.
mutate "escape" "$TMP/st_neg.cc" '' "$B_MARK" "$B_CLOSE" "$B_TRUNC" "$B_CONV" "$B_ASSIGN" 1

# TRUNC does not change WHICH literals are banners, only how much of one reaches the ERE, so
# its arm counts the records that still carry an escape rather than the records themselves.
# Seven of the eight, since every planted literal but the one spelled in a comment ends with
# the escape a reporter writes after its banner.
T_ON="$(scan "$TMP/st_pos.cc" | grep -cF '\n')" || T_ON=0
[ "$T_ON" -eq 0 ] || fail "$T_ON planted banner(s) reach the ERE with an escape still in the
      text, so a multi-line format is checked against a string no reporter ever prints"
extract "$TMP/st_pos.cc" "$B_ESC" "$B_MARK" "$B_CLOSE" "$NEVER" "$B_CONV" "$B_ASSIGN" \
    > "$TMP/st_trunc"
T_OFF="$(grep -cF '\n' "$TMP/st_trunc")" || T_OFF=0
[ "$T_OFF" -eq 7 ] || fail "with the truncation needle respelled $T_OFF planted banner(s)
      carry an escape, expected 7; the controls for it are not near misses"
T_MULTI="$(cut -f4 "$TMP/st_trunc" | grep -cxF '=== FROB EXCEPTION (X) ===')" || T_MULTI=0
[ "$T_MULTI" -eq 0 ] || fail "the multi-line control still reads as its banner line with the
      truncation needle respelled, so nothing here pins where a banner ends"

# The verdict leg, over PLANTED records: what it asserts is the ERE pair, and driving it off
# today's tree would assert the tree instead. Record 4 is the shape the exclusion exists for,
# an arch reporter that spelled the contained outcome by ADDING the noun to a banner
# panic.ere already matches.
cat > "$TMP/st_verdict" <<EOF
a.cc${TAB}1${TAB}fixed${TAB}=== HARD FAULT ===
a.cc${TAB}2${TAB}fixed${TAB}=== NEON FAULT (dead) ===
a.cc${TAB}3${TAB}fixed${TAB}=== ARMV7M CONTAINED (wild PSP) ===
a.cc${TAB}4${TAB}fixed${TAB}=== RX EXCEPTION CONTAINED (wild stack) ===
a.cc${TAB}5${TAB}label${TAB}=== THREAD FAULT === thread 'X' killed
EOF
match_findings "$TMP/st_verdict" "$EXCLUDED_RE" "$KOS_PANIC_RE" > "$TMP/st_vout"
V_UN="$(grep -c '^UNMATCHED' "$TMP/st_vout")" || V_UN=0
V_EX="$(grep -c '^EXCLUDED' "$TMP/st_vout")" || V_EX=0
[ "$V_UN" -eq 1 ] || fail "the verdict leg found $V_UN of 1 planted banner that panic.ere
      does not match; a reporter's new banner would read as accounted for"
[ "$V_EX" -eq 1 ] || fail "the verdict leg found $V_EX of 1 planted banner that panic.ere
      matches although it says the thread died and the system did not"

# EACH record on its own, so a record judged wrongly for the wrong reason is visible.
V_WANT='0 1 0 1 0'
i=0
for want in $V_WANT; do
    i=$((i + 1))
    sed -n "${i}p" "$TMP/st_verdict" > "$TMP/st_vone"
    n="$(match_findings "$TMP/st_vone" "$EXCLUDED_RE" "$KOS_PANIC_RE" | wc -l | tr -d ' ')"
    [ "$n" -eq "$want" ] || fail "verdict control $i answers $n finding(s), expected $want:
      $(cut -f4 "$TMP/st_vone")"
done
[ "$i" -eq 5 ] || fail "$i verdict control(s) ran, expected 5"

V_NEVER='KICKOS_THIS_ERE_MATCHES_NOTHING'
match_findings "$TMP/st_verdict" "$V_NEVER" "$KOS_PANIC_RE" > "$TMP/st_vmut"
M_UN="$(grep -c '^UNMATCHED' "$TMP/st_vmut")" || M_UN=0
M_EX="$(grep -c '^EXCLUDED' "$TMP/st_vmut")" || M_EX=0
[ "$M_UN" -eq 3 ] && [ "$M_EX" -eq 0 ] \
    || fail "with the exclusion disabled the verdict leg answers $M_UN unmatched and $M_EX
      excluded, expected 3 and 0; the three survivable-outcome controls are not near misses"
match_findings "$TMP/st_verdict" "$EXCLUDED_RE" "$V_NEVER" > "$TMP/st_vmut"
P_UN="$(grep -c '^UNMATCHED' "$TMP/st_vmut")" || P_UN=0
P_EX="$(grep -c '^EXCLUDED' "$TMP/st_vmut")" || P_EX=0
[ "$P_UN" -eq 2 ] && [ "$P_EX" -eq 0 ] \
    || fail "with panic.ere disabled the verdict leg answers $P_UN unmatched and $P_EX
      excluded, expected 2 and 0; it is not reading the ERE it reports against"

# corpus_class, one control per arm plus the near miss that discriminates it. Only the
# leading directory is read, so no control opens a file: the three refusals name paths that
# do not exist on purpose, and tools/ tracks no source today.
: > "$TMP/st_class"
cls() { printf '%s\t%s\n' "$1" "$2" >> "$TMP/st_class"; }
cls read   arch/arm/armv7m/arch_armv7m.cc
cls read   kernel/amp/ampmap.cc
cls read   include/kickos/diag.h
cls read   lib/libc/string.cc
cls read   system/init/common/services_none.cc
cls skip   user/apps/common/selftest/main.cc
cls skip   examples/oot-app/main.cc
cls skip   boards/blackpill/include/kickos/board_wiring.h
cls skip   tests/unit/uartclass/uart_mock.cc
cls skip   tools/host/probe.c
cls refuse archive/old/arch_armv7m.cc
cls refuse kernelish/ampmap.cc
cls refuse ampmap.cc
C_READ=0
C_SKIP=0
C_REFUSE=0
i=0
while IFS="$TAB" read -r want path; do
    i=$((i + 1))
    got="$(corpus_class "$path")"
    [ "$got" = "$want" ] || fail "corpus_class($path) says '$got', expected '$want'"
    case "$got" in
        read)   C_READ=$((C_READ + 1)) ;;
        skip)   C_SKIP=$((C_SKIP + 1)) ;;
        refuse) C_REFUSE=$((C_REFUSE + 1)) ;;
    esac
done < "$TMP/st_class"
[ "$i" -eq 13 ] || fail "$i corpus_class control(s) ran, expected 13"
# All three verdicts, or a classification collapsed onto one of them would satisfy every
# equality above and still read the whole tree wrong.
[ "$C_READ" -eq 5 ] || fail "corpus_class answered read for $C_READ of 5 controls"
[ "$C_SKIP" -eq 5 ] || fail "corpus_class answered skip for $C_SKIP of 5 controls"
[ "$C_REFUSE" -eq 3 ] || fail "corpus_class answered refuse for $C_REFUSE of 3 controls"

# --- the corpus ---------------------------------------------------------------
git ls-files > "$TMP/tracked" || fail "git ls-files failed, so the tree is UNKNOWN, not empty"
require_nonempty "$TMP/tracked" "git ls-files matched nothing"
# The extensions are the tree's own, `.cc` and `.h` per docs/reference/style.md. The one
# tracked `.inc` is an assembler vector-table fragment that calls nothing, so widening this
# adds arms nothing reaches. rc 1 is an empty filter result and the floor below catches it;
# past 1 is grep itself failing and must not read as a tree with no source in it.
grep -E '\.(c|cc|h|S)$' "$TMP/tracked" > "$TMP/allsrc"
_rc=$?
[ "$_rc" -le 1 ] || fail "grep exited $_rc filtering the tracked files by source extension"

: > "$TMP/corpus"
: > "$TMP/unclassified"
while IFS= read -r f; do
    case "$(corpus_class "$f")" in
        read) printf '%s\n' "$f" >> "$TMP/corpus" ;;
        skip) ;;
        *)    printf '%s\n' "$f" >> "$TMP/unclassified" ;;
    esac
done < "$TMP/allsrc"
if [ -s "$TMP/unclassified" ]; then
    sed 's/^/      /' "$TMP/unclassified" >&2
    fail "the tracked source above is under no directory corpus_class() knows, so whether a
      fault reporter lives in it is UNKNOWN rather than answered. Give the directory a line
      in corpus_class() with the reason it can or cannot hold one."
fi
require_nonempty "$TMP/corpus" \
    "the corpus is empty: not one tracked source file is under a directory this gate reads"
N="$(wc -l < "$TMP/corpus" | tr -d ' ')"
# Sized at about HALF what those directories track, so an ordinary deletion still passes.
# corpus_class() is what refuses a root taken out of the corpus; this is what refuses a
# listing that came back short with every root still named.
CORPUS_FLOOR=250
[ "$N" -ge "$CORPUS_FLOOR" ] || fail "$N source file(s) in the corpus, under the floor of
      $CORPUS_FLOOR; the tree this gate read is not this tree"

: > "$TMP/banners"
while IFS= read -r f; do
    # REFUSED, not skipped: a silent skip is how a corpus shrinks with no count moving, and
    # the headline below counts the files that YIELDED a banner rather than the corpus.
    [ -f "$f" ] || fail "tracked file is missing from the worktree: $f"
    if ! scan "$f" >> "$TMP/banners"; then
        fail "the extractor exited nonzero on $f, so that file's banners went unread and its
      verdict is UNKNOWN, not clean"
    fi
done < "$TMP/corpus"

require_nonempty "$TMP/banners" \
    "no banner was extracted from any file: the literal shape no longer matches an emit site"

# --- positive controls -------------------------------------------------------
rc=0
printf '%s\n' "$REPORTERS" | while IFS= read -r r; do
    [ -n "$r" ] || continue
    if ! cut -f1 "$TMP/banners" | grep -qxF "$r"; then
        echo "FAIL: $r yielded no banner: it is a fault reporter, so either it stopped" >&2
        echo "      emitting one or this gate stopped seeing it" >&2
        echo x >> "$TMP/rc"
    fi
done
labels=$(awk -F"$TAB" '$3 == "label"' "$TMP/banners" | wc -l)
if [ "$labels" -lt "$MIN_LABELS" ]; then
    echo "FAIL: the name-slot leg resolved $labels label(s), expected at least $MIN_LABELS." >&2
    echo "      A '=== %s ===' banner whose labels are not found is checked against nothing." >&2
    echo x >> "$TMP/rc"
fi

# --- every banner must be matched -------------------------------------------
checked=$(wc -l < "$TMP/banners" | tr -d ' ')
match_findings "$TMP/banners" "$EXCLUDED_RE" "$KOS_PANIC_RE" > "$TMP/mismatch"
while IFS="$TAB" read -r kind f n slot text; do
    if [ "$kind" = EXCLUDED ]; then
        echo "FAIL: $f:$n banner '$text' IS matched by tests/lib/panic.ere, but it says" >&2
        echo "      the thread died and the system did not. Every assert_no_panic on a" >&2
        echo "      board that survives this outcome would now fail." >&2
    else
        echo "FAIL: $f:$n banner '$text' ($slot) is NOT matched by tests/lib/panic.ere." >&2
        echo "      A gate asserting no panic passes on a board that printed it, and a gate" >&2
        echo "      asserting a panic cannot key on it. Add it to the ERE." >&2
    fi
    echo x >> "$TMP/rc"
done < "$TMP/mismatch"

if [ -s "$TMP/rc" ]; then
    rc=1
fi
if [ "$rc" -ne 0 ]; then
    fail "$(wc -l < "$TMP/rc") banner finding(s) above"
fi

echo "PASS: $checked banner(s) from $(cut -f1 "$TMP/banners" | sort -u | wc -l) of $N" \
     "tracked file(s), $labels resolved from a name slot, all accounted for by" \
     "tests/lib/panic.ere"
exit 0
