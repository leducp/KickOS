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

require_repo_root

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
        # A planted console capture, written by hand: its judge reads from the boot's banner on,
        # so a `#` header above the banner is never judged.
        *.capture)
            printf 'need\n' ;;
        # A planted capture's arrival stamps: its judge reads only the rows that carry a line.
        *.capture.times)
            printf 'need\n' ;;
        # objcopy --redefine-syms input: `#` starts a comment there too, which a reader who
        # takes the file for a bare two-column table would not expect.
        *.syms)
            printf 'need\n' ;;
        # VCS metadata, not authored content.
        .gitignore|*/.gitignore|.gitattributes|*/.gitattributes)
            printf 'none\n' ;;
        # uv writes the lock whole on every `uv lock`, so a header line would not survive one.
        uv.lock|*/uv.lock)
            printf 'none\n' ;;
        # `#`.
        *.toml)
            printf 'need\n' ;;
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
        # A unified diff: `git apply` reads every line before the first file header as
        # commentary.
        *.patch)
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

# --- controls: one per clause of the header rule, planted under scratch_dir -----------------
# classify() is not planted: the tree runs every arm of it, and a type it does not know fails.

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

i=0
while IFS="$TAB" read -r name want; do
    i=$((i + 1))
    header_finding "$TMP/hdr_$name" "$SPDX_WINDOW" "$SPDX_ERE" "$COPYRIGHT_ERE" "$COPYRIGHT_SPAN"
    [ "$HF" = "$want" ] || fail "header control $name: the scan says '$HF', expected '$want'"
done < "$TMP/header_controls"
[ "$i" -eq 10 ] || fail "$i header control(s) ran, expected 10"

# --- the corpus ---------------------------------------------------------------
corpus_all "$TMP/all"
corpus_floor "$TMP/all" 600 "tracked file(s)"

: > "$TMP/findings"
: > "$TMP/refused"
N_NEED=0
N_NONE=0
while IFS= read -r f; do
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
