#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The unbounded-loop rule of docs/reference/style.md: `while (true)`, never `for (;;)`.
#
# Run from the repo root, no arguments, no build directory:
#   tests/static/check_forever_loop.sh
#
#   corpus     tracked *.c, *.cc, *.cpp, *.h, *.hh, *.hpp, *.inc, *.h.in and *.S. Source
#              only; the docs quote the banned spelling on purpose.
#
#   spacing    every `for` / `(` / `;` / `;` / `)` arrangement, whitespace-insensitive:
#              for(;;), for (;;), for( ;; ), for ( ; ; ) all hit.
#
#   comments   `//`, `/* */`, string literals and character literals are blanked BEFORE the
#              match, line numbering preserved so a finding cites the real line.
#
#   refusal    the scanner exits 2 and the run FAILS naming the file, verdict UNKNOWN, when
#              a block comment is open at EOF or a literal is unclosed at the end of a line
#              once backslash continuations are spliced.
#
# THEREFORE NOT CAUGHT. Know these before trusting a green run:
#   - `while (1)`, `do {} while (true)`, `goto` back-edges and a recursive tail call: the
#     rule style.md states names two spellings and this gate checks those two.
#   - a `for (;;)` assembled by the preprocessor out of pieces, and one inside a macro
#     argument that only becomes a loop after substitution.
#   - an untracked file, and any language outside the corpus above.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# NOT set -e: the point is to collect EVERY finding in one run, not to stop at the first.

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

scratch_dir

# One copy of the scanner, shared with check_public_headers.sh: a second copy here is how a
# scanner bug gets fixed in one reader and left standing in the other.
STRIP="$(dirname "$0")/../lib/strip_comments.awk"
[ -r "$STRIP" ] || fail "tests/lib/strip_comments.awk is unreadable; nothing below can tell code from prose"

# The one match, as one ERE, so the self-test and the corpus scan cannot disagree.
FOR_ERE='for[[:space:]]*\([[:space:]]*;[[:space:]]*;[[:space:]]*\)'

# --- self-test: prove the scanner both ways before reading the tree -----------
# A stripper that ate everything, or an ERE that matched nothing, would each report the
# whole tree clean, and a green run shows neither.
cat > "$TMP/pos.c" <<'EOF'
for(;;) { a(); }
for (;;) { b(); }
for( ;; ) { c(); }
for ( ; ; ) { d(); }
EOF
cat > "$TMP/neg.c" <<'EOF'
// style: for (;;) is banned here
/* a block comment naming for (;;) over
   two lines, still for(;;) */
char const* s = "for (;;)";
char c = ';';
char const* t = "for (;;) inside a \
continued literal, which is legal C";
while (true) { e(); }
EOF
POS="$(LC_ALL=C awk -f "$STRIP" "$TMP/pos.c" | grep -cE "$FOR_ERE")"
[ "$POS" -eq 4 ] || fail "the scanner found $POS of 4 planted for(;;) spellings; it would miss real ones"
# Exit status read separately: the pipeline below hides it, and a refusal leaks the literal's
# text into the residue as code.
if ! LC_ALL=C awk -f "$STRIP" "$TMP/neg.c" > "$TMP/negout" 2> "$TMP/negerr"; then
    sed 's/^/      /' "$TMP/negerr" >&2
    fail "the scanner refused legal C, so every file carrying a continued literal reads UNKNOWN"
fi
NEG="$(grep -cE "$FOR_ERE" "$TMP/negout")"
[ "$NEG" -eq 0 ] || fail "the scanner reported $NEG hit(s) in comments and literals; every finding would be noise"
# The residue is compared line for line against the source by check_public_headers.sh, and a
# joined continuation must not shorten it.
NL_IN="$(wc -l < "$TMP/neg.c" | tr -d ' ')"
NL_OUT="$(wc -l < "$TMP/negout" | tr -d ' ')"
[ "$NL_IN" = "$NL_OUT" ] || fail "the scanner emitted $NL_OUT line(s) for $NL_IN input line(s); every finding would cite the wrong line"

# A literal that is unclosed with no continuation is NOT classifiable, and the refusal is the
# only thing that keeps such a file from reading clean.
printf 'char const* u = "never closed;\nwhile (true) { f(); }\n' > "$TMP/unterm.c"
if LC_ALL=C awk -f "$STRIP" "$TMP/unterm.c" > /dev/null 2>&1; then
    fail "the scanner accepted an unclosed literal, so an unstrippable file would report clean"
fi

# --- the corpus ---------------------------------------------------------------
git ls-files -- '*.c' '*.cc' '*.cpp' '*.h' '*.hh' '*.hpp' '*.inc' '*.h.in' '*.S' \
    > "$TMP/all" || fail "git ls-files failed"
require_nonempty "$TMP/all" "git ls-files matched no C/C++ file; every check below would pass vacuously"
N="$(wc -l < "$TMP/all" | tr -d ' ')"

: > "$TMP/findings"
: > "$TMP/refused"
: > "$TMP/residue"
while IFS= read -r f; do
    [ -f "$f" ] || fail "tracked file is missing from the worktree: $f"
    if LC_ALL=C awk -f "$STRIP" "$f" > "$TMP/stripped" 2>> "$TMP/refused"; then
        :
    else
        rc=$?
        [ "$rc" -eq 2 ] || fail "awk exited $rc scanning $f"
        continue
    fi
    grep -nE "$FOR_ERE" "$TMP/stripped" | awk -v F="$f" '{ print F ":" $0 }' >> "$TMP/findings"
    # The positive control on the corpus pass, accumulated as it goes: see below.
    grep -cE 'while[[:space:]]*\([[:space:]]*true[[:space:]]*\)' "$TMP/stripped" >> "$TMP/residue"
done < "$TMP/all"

echo "== checked $N tracked C/C++ file(s) for for(;;), comments and literals stripped =="

if [ -s "$TMP/refused" ]; then
    echo "FAIL: the scan could not strip $(wc -l < "$TMP/refused" | tr -d ' ') file(s), so their verdict is UNKNOWN, not clean:" >&2
    sed 's/^/      /' "$TMP/refused" >&2
    exit 1
fi

# Proves the scanner ran over THIS corpus, not just the planted input: `while (true)` is the
# spelling the rule mandates, so a residue holding none means the strip ate the tree's code.
KEPT="$(awk '{ s += $1 } END { print s + 0 }' "$TMP/residue")"
[ "$KEPT" -gt 0 ] || fail "not one while (true) survived the strip across $N file(s); the scan read no code"

if [ -s "$TMP/findings" ]; then
    cat "$TMP/findings" >&2
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/findings" | tr -d ' ') for(;;) loop(s)." >&2
    echo "      Write while (true). One spelling is what makes every unbounded loop in this" >&2
    echo "      tree greppable in one pass." >&2
    exit 1
fi

echo "PASS: $KEPT while (true) loop(s) across $N tracked C/C++ file(s), and no for(;;)"
