#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The SPDX rule: every tracked file that CAN carry a comment opens with an
# SPDX-License-Identifier line inside its first five lines, and the copyright line sits
# directly beneath it, written with the file format's own comment marker.
#
# Run from the repo root, no arguments: tests/static/check_spdx.sh
#
# Every tracked file from `git ls-files` is classified one at a time by classify() below:
# `need` when the FORMAT has a comment production, `none` when it has none and a header
# would have to be data, and `refuse` for a type this gate has never been told about, whose
# verdict is UNKNOWN and whose run FAILS naming it. A new file type gets a line in
# classify() with its reason, or it stops the gate. NOTHING outside classify() is exempt,
# and classify() exempts a format, never a file, with one named exception carrying its own
# reason.
#
# "BESIDE" means the copyright line is the line DIRECTLY AFTER the first SPDX line. The
# loose reading has a live false green, since a file may hold the WORDS
# "SPDX-License-Identifier" and "copyright" in prose and read as headered, which
# docs/SPDX-header-template.txt does.
#
# SCOPE. What is read is the presence and adjacency of the two lines within the first five.
# The identifier's VALUE, the copyright HOLDER and YEAR, and whether either line sits inside
# a comment rather than in prose are all outside it; the adjacency rule is what makes prose
# hard to pass off as a header.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# Findings accumulate over the whole corpus, so set -e must stay off.

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

# One tracked path -> need | none | refuse.
#
# The single by-NAME exemption: tests/lib/panic.ere is read as an ERE by two consumers
# (tests/lib/gate.sh cats it into KOS_PANIC_RE; the root CMakeLists takes its first line as
# a FAIL_REGULAR_EXPRESSION), so a header line on top breaks both.
classify() {
    case "$1" in
        tests/lib/panic.ere)
            printf 'none\n'; return ;;
    esac
    case "$1" in
        # JSON: no comment production in the grammar, and CMake's preset parser rejects one.
        *.json)
            printf 'none\n' ;;
        # `//` or `/* */`.
        *.c|*.cc|*.cpp|*.h|*.hh|*.hpp|*.S|*.inc|*.ld|*.lds)
            printf 'need\n' ;;
        # `<!-- -->`.
        *.md)
            printf 'need\n' ;;
        # `#`.
        *.sh|*.py|*.cmake|*.awk|*.txt|*.yml|*.yaml|*.conf|*.example)
            printf 'need\n' ;;
        # objcopy --redefine-syms input: `#` starts a comment there too, which a reader who
        # takes the file for a bare two-column table would not expect.
        *.syms)
            printf 'need\n' ;;
        # VCS metadata, not authored content.
        .gitignore|*/.gitignore|.gitattributes|*/.gitattributes)
            printf 'none\n' ;;
        Kconfig|*/Kconfig|defconfig|*/defconfig)
            printf 'need\n' ;;
        # A configure_file/`.in` template: the substituted copy is a source file, so the
        # template carries the header the copy will.
        *.in)
            printf 'need\n' ;;
        # Plain text with no comment syntax, but a licence-scanning tool expects the tag on
        # line 1.
        LICENSE|*/LICENSE)
            printf 'need\n' ;;
        *)
            printf 'refuse\n' ;;
    esac
}

scratch_dir

# --- the rule, handed to the scanner, so the self-test and the corpus cannot disagree ---
SPDX_WINDOW=5
SPDX_ERE='SPDX-License-Identifier'
COPYRIGHT_ERE='copyright'
# 1 reads the line after the SPDX line, and nothing else; the loose reading is the false green
# the header names.
COPYRIGHT_SPAN=1

# Sets HF to nothing, `nospdx`, or `adjacency <line the SPDX tag sits on>`.
#
# THE VERDICT COMES BACK IN A GLOBAL AND NOT ON STDOUT, so that every reader below runs in
# THIS shell. Read through `$(...)` this whole function is a SUBSHELL, where tool_out's exit
# and fail() kill only that subshell and the caller reads the empty string, which is the
# CLEAN verdict.
#
# grep's own STATUS carries the copyright answer, never a count: an empty count makes
# `[ "$c" -eq 0 ]` exit 2, and an `if` reads an error as false, so the finding is dropped and
# the file reports clean.
header_finding() { # <file> <window> <spdx-ere> <copyright-ere> <copyright span 1|0>
    HF=""
    tool_out "$TMP/hf_head" '' head -n "$2" "$1"
    # -a: a file grep decides is "binary" yields one summary line and no match. rc 1 is a
    # file carrying no tag, so tool_out cannot judge this one; past 1 is grep dying.
    grep -anE "$3" "$TMP/hf_head" > "$TMP/hf_hit"
    _hf_rc=$?
    [ "$_hf_rc" -le 1 ] || fail "grep exited $_hf_rc looking for the SPDX tag in $1"
    if [ ! -s "$TMP/hf_hit" ]; then
        HF="nospdx"
        return
    fi
    _hf_line="$(sed -n '1s/:.*//p' "$TMP/hf_hit")"
    [ -n "$_hf_line" ] \
        || fail "no line number came out of the SPDX match in $1, so its verdict is UNKNOWN"
    # The FIRST such line is the header, so the copyright line is the one after it.
    _hf_src="$1"
    if [ "$5" -eq 1 ]; then
        tool_out "$TMP/hf_next" '' sed -n "$((_hf_line + 1))p" "$1"
        _hf_src="$TMP/hf_next"
    fi
    grep -qiE "$4" "$_hf_src"
    _hf_rc=$?
    [ "$_hf_rc" -le 1 ] || fail "grep exited $_hf_rc looking for the copyright line of $1"
    if [ "$_hf_rc" -eq 1 ]; then
        HF="adjacency $_hf_line"
    fi
}

# --- self-test: prove every clause of the rule, one control per clause ---------
# Each control is a MINIMAL PAIR against its opposite number, differing in one property only,
# so one an unrelated clause catches shows up as the wrong count rather than as a pass.
#
# The corpus below is every tracked file, so the controls are planted under scratch_dir's
# mktemp directory and are invisible to `git ls-files`. A control naming the SPDX tag inside
# this file is harmless: only the first five lines of a file are ever read.

# classify(), one path per arm plus the near miss that discriminates it from its neighbour.
: > "$TMP/classify_controls"
arm() { # <verdict> <path>
    printf '%s\t%s\n' "$1" "$2" >> "$TMP/classify_controls"
}
arm none   tests/lib/panic.ere
arm refuse tests/lib/panic.ere.bak
arm refuse kernel/lib/panic.ere
arm none   CMakePresets.json
arm refuse boards/x/presets.jsonc
arm need   kernel/sched.cc
arm need   arch/arm/armv7m/vectors.S
arm need   docs/reference/style.md
arm need   tools/sweep_host_gates.sh
arm need   tests/static/fn_body.awk
arm need   cmake/kernel_runtime.syms
arm none   .gitignore
arm none   boards/x/.gitignore
arm none   .gitattributes
arm refuse boards/x/gitignore
arm need   Kconfig
arm need   boards/x/Kconfig
arm refuse boards/x/Kconfiguration
arm need   defconfig
arm need   boards/x/defconfig
arm refuse boards/x/rx72m_defconfig
arm need   kernel/include/kickos/config/cap_width.h.in
arm refuse tools/telemetry.ini
arm need   LICENSE
arm need   docs/LICENSE
arm refuse LICENCE
arm refuse README

C_NEED=0
C_NONE=0
C_REFUSE=0
i=0
while IFS="$TAB" read -r want path; do
    i=$((i + 1))
    got="$(classify "$path")"
    [ "$got" = "$want" ] || fail "classify($path) says '$got', expected '$want'"
    case "$got" in
        need)   C_NEED=$((C_NEED + 1)) ;;
        none)   C_NONE=$((C_NONE + 1)) ;;
        refuse) C_REFUSE=$((C_REFUSE + 1)) ;;
    esac
done < "$TMP/classify_controls"
[ "$i" -eq 27 ] || fail "$i classify() control(s) ran, expected 27"
# All three verdicts, or a classify() collapsed onto one of them would satisfy every equality
# above and still classify the whole tree wrong.
[ "$C_NEED" -eq 13 ] || fail "classify() answered need for $C_NEED of 13 controls"
[ "$C_NONE" -eq 5 ] || fail "classify() answered none for $C_NONE of 5 controls"
[ "$C_REFUSE" -eq 9 ] || fail "classify() answered refuse for $C_REFUSE of 9 controls"

# The header check. Each positive is one clause: no tag at all, a tag one line past the
# window, a copyright line not beside the tag, the two words in PROSE, and a copyright line
# that sits beside the SECOND tag rather than the first.
: > "$TMP/header_controls"
hdr() { # <name> <expected finding>; the body arrives on stdin
    cat > "$TMP/hdr_$1"
    printf '%s\t%s\n' "$1" "$2" >> "$TMP/header_controls"
}
hdr p_absent nospdx <<'EOF'
# Copyright (c) 2026 Philippe Leduc
int kos_x;
EOF
hdr p_past_window nospdx <<'EOF'
#
#
#
#
#
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
EOF
hdr p_not_beside "adjacency 1" <<'EOF'
# SPDX-License-Identifier: CECILL-C
#
# Copyright (c) 2026 Philippe Leduc
EOF
hdr p_prose "adjacency 2" <<'EOF'
# A template for the header every source file opens with.
# Put the SPDX-License-Identifier tag on the first line that can hold a comment.
# It names the licence and carries nothing else.
# The copyright line goes directly beneath it.
EOF
hdr p_second_tag "adjacency 1" <<'EOF'
# SPDX-License-Identifier: CECILL-C
# a note wedged in between the two tags
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
EOF
hdr n_line_one '' <<'EOF'
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
EOF
hdr n_after_shebang '' <<'EOF'
#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
EOF
hdr n_window_edge '' <<'EOF'
#
#
#
#
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
EOF
hdr n_lowercase '' <<'EOF'
// SPDX-License-Identifier: CECILL-C
// copyright (c) 2026 Philippe Leduc
EOF
hdr n_markdown '' <<'EOF'
<!-- SPDX-License-Identifier: CECILL-C -->
<!-- Copyright (c) 2026 Philippe Leduc -->
EOF

H_NOSPDX=0
H_ADJ=0
H_CLEAN=0
i=0
while IFS="$TAB" read -r name want; do
    i=$((i + 1))
    header_finding "$TMP/hdr_$name" "$SPDX_WINDOW" "$SPDX_ERE" "$COPYRIGHT_ERE" "$COPYRIGHT_SPAN"
    got="$HF"
    [ "$got" = "$want" ] || fail "header control $name: the scan says '$got', expected '$want'"
    case "$got" in
        nospdx) H_NOSPDX=$((H_NOSPDX + 1)) ;;
        '')     H_CLEAN=$((H_CLEAN + 1)) ;;
        *)      H_ADJ=$((H_ADJ + 1)) ;;
    esac
done < "$TMP/header_controls"
[ "$i" -eq 10 ] || fail "$i header control(s) ran, expected 10"
[ "$H_NOSPDX" -eq 2 ] || fail "the scan missed a tag in $H_NOSPDX of 2 controls that carry none in the window"
[ "$H_ADJ" -eq 3 ] || fail "the scan reported $H_ADJ of 3 controls whose copyright line is not beside the tag"
[ "$H_CLEAN" -eq 5 ] || fail "the scan read $H_CLEAN of 5 conforming controls clean; the gate would cry wolf"

# The mutation the controls exist to survive: relax one clause and the counts over the control
# corpus must move to EXACT numbers that differ per clause, so the control is a near miss and
# not slack.
NEVER='KICKOS_THIS_ERE_MATCHES_NOTHING'
header_mutation() { # <clause> <window> <spdx-ere> <copyright-ere> <span> <nospdx> <adjacency>
    _hm_n=0
    _hm_a=0
    while IFS="$TAB" read -r name want; do
        header_finding "$TMP/hdr_$name" "$2" "$3" "$4" "$5"
        case "$HF" in
            '') ;;
            nospdx) _hm_n=$((_hm_n + 1)) ;;
            *)      _hm_a=$((_hm_a + 1)) ;;
        esac
    done < "$TMP/header_controls"
    [ "$_hm_n" -eq "$6" ] && [ "$_hm_a" -eq "$7" ] || fail "with the $1 clause relaxed the scan reported $_hm_n missing tag(s) and
      $_hm_a misplaced copyright line(s), expected $6 and $7; the controls for it are not near
      misses and prove nothing"
}
# A window wide enough to reach line 6 exempts the one control that sits there, and nothing
# else moves.
header_mutation five-line-window 99 "$SPDX_ERE" "$COPYRIGHT_ERE" "$COPYRIGHT_SPAN" 1 3
# The loose reading: a copyright line ANYWHERE in the file. All three adjacency controls carry
# one, the prose control included, so all three go quiet.
header_mutation beside 5 "$SPDX_ERE" "$COPYRIGHT_ERE" 0 2 0
# Neither leg can run at all without its own matcher, and each collapses the corpus onto ONE
# verdict, so a control quiet because the tag was found is not confused with one quiet because
# the copyright line was.
header_mutation spdx-tag 5 "$NEVER" "$COPYRIGHT_ERE" "$COPYRIGHT_SPAN" 10 0
header_mutation copyright-word 5 "$SPDX_ERE" "$NEVER" "$COPYRIGHT_SPAN" 2 8

# --- the corpus ---------------------------------------------------------------
git ls-files > "$TMP/all" || fail "git ls-files failed"
require_nonempty "$TMP/all" "git ls-files matched nothing; every check below would pass vacuously"
# Sized at about HALF what the tree tracks, so an ordinary deletion still passes while a
# truncated listing refuses. Nonempty is not a floor: this gate asserts an ABSENCE over
# every tracked file, and a handful of them satisfies it as readily as all of them.
CORPUS_FLOOR=600
N_ALL="$(wc -l < "$TMP/all" | tr -d ' ')"
[ "$N_ALL" -ge "$CORPUS_FLOOR" ] || fail "git ls-files listed $N_ALL tracked file(s), under the
      floor of $CORPUS_FLOOR. The corpus this gate read is not this tree, so its silence is
      about nothing."

: > "$TMP/findings"
: > "$TMP/refused"
N=0
N_NEED=0
N_NONE=0
while IFS= read -r f; do
    [ -f "$f" ] || fail "tracked file is missing from the worktree: $f"
    [ -r "$f" ] || fail "tracked file is unreadable, so its verdict is UNKNOWN, not clean: $f"
    N=$((N + 1))
    case "$(classify "$f")" in
        none)
            N_NONE=$((N_NONE + 1))
            continue ;;
        refuse)
            printf '%s\n' "$f" >> "$TMP/refused"
            continue ;;
    esac
    N_NEED=$((N_NEED + 1))

    header_finding "$f" "$SPDX_WINDOW" "$SPDX_ERE" "$COPYRIGHT_ERE" "$COPYRIGHT_SPAN"
    case "$HF" in
        '') ;;
        nospdx)
            printf '%s: no SPDX-License-Identifier in the first five lines\n' "$f" >> "$TMP/findings" ;;
        *)
            SPDX="${HF#adjacency }"
            printf '%s: SPDX on line %s, but line %s is not the copyright line\n' \
                "$f" "$SPDX" "$((SPDX + 1))" >> "$TMP/findings" ;;
    esac
done < "$TMP/all"

echo "== checked $N tracked file(s): $N_NEED can carry a header, $N_NONE cannot =="

if [ -s "$TMP/refused" ]; then
    echo "FAIL: this gate has no classification for $(wc -l < "$TMP/refused" | tr -d ' ') tracked file(s)," >&2
    echo "      so their verdict is UNKNOWN, not clean:" >&2
    sed 's/^/      /' "$TMP/refused" >&2
    echo "      Add the type to classify() in this script, with the reason it can or" >&2
    echo "      cannot hold a comment. Do not widen the fallthrough." >&2
    exit 1
fi

# A `none` verdict for every file would satisfy every check above. Pin the other side.
[ "$N_NEED" -gt 0 ] || fail "not one tracked file was required to carry a header; classify() is broken"

if [ -s "$TMP/findings" ]; then
    cat "$TMP/findings" >&2
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/findings" | tr -d ' ') file(s) without a conforming header." >&2
    echo "      Open the file with its own comment marker plus SPDX-License-Identifier," >&2
    echo "      inside the first five lines, and put the copyright line directly beneath." >&2
    exit 1
fi

echo "PASS: every tracked file that can carry a header carries one, with its copyright line beside it"
