#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The SPDX rule of docs/reference/style.md: every tracked file that CAN carry a comment
# opens with an SPDX-License-Identifier line inside its first five lines, and the
# copyright line sits directly beneath it. docs/SPDX-header-template.txt spells the
# per-language forms.
#
# Run from the repo root, no arguments, no build directory:
#   tests/static/check_spdx.sh
#
#   corpus     every tracked file, from `git ls-files`, classified one file at a time by
#              the table in classify() below.
#
#   verdicts   `need`   the file's FORMAT has a comment production, so the header is
#                       representable and is required.
#              `none`   the format has no comment production at all, so a header would have
#                       to be data: JSON has none in its grammar, and CMake's preset parser
#                       rejects one.
#              `refuse` this gate has never been told what the type can carry, so its
#                       verdict is UNKNOWN and the run FAILS naming it. A new file type
#                       gets a line in classify() with the reason, or it stops the gate.
#
#   "beside"   the copyright line must be the line DIRECTLY AFTER the first SPDX line, not
#              merely somewhere near it. The loose reading has a live false green: a file
#              may hold the WORDS "SPDX-License-Identifier" and "copyright" in prose and
#              read as headered, which docs/SPDX-header-template.txt does.
#
# NOTHING outside classify() is exempt, and classify() exempts a format, never a file, with
# one named exception carrying its own reason.
#
# THEREFORE NOT CAUGHT. Know these before trusting a green run:
#   - the identifier VALUE. A file declaring some other licence passes; the rule
#     style.md states is about the header being present, not about which licence.
#   - the copyright HOLDER and YEAR: any line saying "copyright" satisfies the second
#     leg, so a stale year and a wrong name both pass.
#   - whether the header is actually inside a comment. A `need` file whose first five
#     lines quote the words in prose reads as headered; the adjacency rule above is what
#     makes that hard rather than impossible.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# NOT set -e: the point is to collect EVERY finding in one run, not to stop at the first.

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
        # JSON: no comment production in the grammar.
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

git ls-files > "$TMP/all" || fail "git ls-files failed"
require_nonempty "$TMP/all" "git ls-files matched nothing; every check below would pass vacuously"

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

    # -a: a file grep decides is "binary" yields one summary line and no match.
    SPDX="$(head -n 5 "$f" | grep -an 'SPDX-License-Identifier' | head -n1 | cut -d: -f1)"
    if [ -z "$SPDX" ]; then
        printf '%s: no SPDX-License-Identifier in the first five lines\n' "$f" >> "$TMP/findings"
        continue
    fi
    # The FIRST such line is the header; see "beside" above.
    NEXT="$(sed -n "$((SPDX + 1))p" "$f" | grep -ci 'copyright')"
    if [ "$NEXT" -eq 0 ]; then
        printf '%s: SPDX on line %s, but line %s is not the copyright line\n' \
            "$f" "$SPDX" "$((SPDX + 1))" >> "$TMP/findings"
    fi
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
    echo "      docs/SPDX-header-template.txt has the form for each language. The copyright" >&2
    echo "      line goes directly beneath the SPDX line." >&2
    exit 1
fi

echo "PASS: every tracked file that can carry a header carries one, with its copyright line beside it"
