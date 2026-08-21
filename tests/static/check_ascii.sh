#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The ASCII rule of docs/reference/style.md: no tracked file holds a byte above 0x7F.
#
# Run from the repo root, no arguments, no build directory:
#   tests/static/check_ascii.sh
#
#   corpus   EVERY tracked file, from `git ls-files`, with no extension filter of any
#            kind, so the next .svg, .rst or .csv is covered on the day it lands. exempt()
#            below carries the two exempt patterns and the reason each holds; every file
#            they match is printed by name on every run.
#
#   binary   a tracked file holding a NUL byte is not text, and "ASCII only" states nothing
#            about it, so this gate REFUSES it by name rather than skipping it. Refused AND
#            STILL SCANNED: `grep -a` reads it either way, so no byte report is suppressed.
#
# The rule is about the BYTE, so the verdict covers bytes above 0x7F and NUL, and nothing
# else: an ASCII spelling that is merely WRONG passes (an HTML entity for a dash, `(c)`
# where a real copyright sign was meant), and a control character below 0x20 is ASCII,
# belonging to style.md's separate no-CRLF and no-trailing-whitespace rules.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# NOT set -e: the point is to collect EVERY finding in one run, not to stop at the first.

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

# The two exempt patterns, each with the reason the bytes are not ours to change.
#
#   LICENSE
#       verbatim upstream CeCILL-C text, which carries a-grave (C3 A0) twice, in
#       "Commissariat a l'Energie Atomique" and in "vis-a-vis": it cannot be ASCII and stay
#       verbatim. Restore it from cecill.info, never by hand-transliterating.
#
#   docs/archive/*_meas.md
#       an archived RAW INSTRUMENT CAPTURE. Per docs/README.md a measurement is not
#       regenerable, so rewriting a byte inside one falsifies it, NULs off the wire included.
#       Scoped to the `_meas.md` captures, not to docs/archive/ wholesale: the directory
#       also holds ordinary prose (docs/archive/M1_state.md), scanned like any other file.
#
exempt() {
    case "$1" in
        LICENSE)                 return 0 ;;
        docs/archive/*_meas.md)  return 0 ;;
    esac
    return 1
}

scratch_dir

git ls-files > "$TMP/all" || fail "git ls-files failed"
require_nonempty "$TMP/all" "git ls-files matched nothing; every check below would pass vacuously"

# Built with printf: a literal 0x80..0xFF range cannot be typed into this file, which is
# itself part of the corpus above. Proven both ways every run, because a shell that left
# the escapes unexpanded and a locale other than C each break the range silently.
HIGH="$(printf '[\200-\377]')"
printf 'caf\351\n' | LC_ALL=C grep -q "$HIGH" \
    || fail "the high-byte class matches no high byte; the scan below would report clean on anything"
if printf 'a plain ASCII line\n' | LC_ALL=C grep -q "$HIGH"; then
    fail "the high-byte class matches pure ASCII; every file in the tree would report"
fi

: > "$TMP/findings"
: > "$TMP/binary"
: > "$TMP/exempt"
N=0
while IFS= read -r f; do
    [ -f "$f" ] || fail "tracked file is missing from the worktree: $f"
    [ -r "$f" ] || fail "tracked file is unreadable, so its verdict is UNKNOWN, not clean: $f"
    if exempt "$f"; then
        printf '%s\n' "$f" >> "$TMP/exempt"
        continue
    fi
    N=$((N + 1))
    # grep cannot carry a NUL in its pattern, so an unequal `tr -d` compare is the test.
    if ! LC_ALL=C tr -d '\000' < "$f" | cmp -s - "$f"; then
        printf '%s\n' "$f" >> "$TMP/binary"
    fi
    # -a, because a file grep decides is "binary" gets ONE summary line and no matches.
    # The name is prefixed by hand, so a one-file invocation reports in the same shape as
    # a many-file one.
    LC_ALL=C grep -an "$HIGH" "$f" | awk -v F="$f" '{ print F ":" $0 }' >> "$TMP/findings"
done < "$TMP/all"

echo "== checked $N tracked file(s), every byte of each =="

if [ -s "$TMP/exempt" ]; then
    echo "== not scanned, exempt by name (see exempt() for the reason each carries) =="
    sed 's/^/   /' "$TMP/exempt"
fi

RC=0

if [ -s "$TMP/findings" ]; then
    # cat -v, or the report re-emits the bytes it complains about and the terminal renders
    # them as whatever looked fine to whoever committed them.
    cat -v "$TMP/findings" >&2
    echo "" >&2
    echo "per-file finding count:" >&2
    cut -d: -f1 "$TMP/findings" | sort | uniq -c | sort -rn >&2
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/findings" | tr -d ' ') line(s) hold a byte above 0x7F." >&2
    echo "      Spell it in ASCII: a comma or a single - for an em dash, -> for an arrow," >&2
    echo "      straight quotes, \"section\" for a section sign. Never a double hyphen:" >&2
    echo "      check_dash_punct.sh refuses that. The M-x pairs above are how cat -v shows the byte." >&2
    RC=1
fi

if [ -s "$TMP/binary" ]; then
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/binary" | tr -d ' ') tracked file(s) hold a NUL byte, so they are not text" >&2
    echo "      and this rule says nothing about them. Their verdict is UNKNOWN, not clean:" >&2
    sed 's/^/      /' "$TMP/binary" >&2
    echo "      Drop the file, strip the NULs, or classify it in this script with the reason" >&2
    echo "      the tree tracks a non-text file." >&2
    RC=1
fi

[ "$RC" -eq 0 ] || exit 1

echo "PASS: no tracked file holds a byte above 0x7F, and none holds a NUL"
