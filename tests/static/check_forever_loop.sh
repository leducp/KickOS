#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The unbounded-loop rule: `while (true)`, never `for (;;)`.
#
# Run from the repo root, no arguments: tests/static/check_forever_loop.sh
#
# SCOPE. It reads the two spellings the rule names out of the source text of tracked C/C++
# files, with comments and literals blanked first. Outside it: `while (1)`, `do {} while
# (true)`, a `goto` back-edge, a recursive tail call, and a `for (;;)` that exists only after
# preprocessing or macro substitution.
#
# A file whose comments and literals cannot be classified exits the scanner 2 and is reported
# UNKNOWN by name, not clean.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# Findings accumulate over the whole corpus, so set -e must stay off.

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

scratch_dir

STRIP="$(dirname "$0")/../lib/strip_comments.awk"
[ -r "$STRIP" ] || fail "tests/lib/strip_comments.awk is unreadable; nothing below can tell code from prose"

FOR_ERE='for[[:space:]]*\([[:space:]]*;[[:space:]]*;[[:space:]]*\)'

# --- self-test: prove the scanner both ways before reading the tree -----------
# A stripper that ate everything and an ERE that matched nothing each report the whole tree
# clean, so both are proven on planted input first.
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
# Exit status read separately: a pipeline hides it, and a refusal leaks the literal's text
# into the residue as code.
if ! LC_ALL=C awk -f "$STRIP" "$TMP/neg.c" > "$TMP/negout" 2> "$TMP/negerr"; then
    sed 's/^/      /' "$TMP/negerr" >&2
    fail "the scanner refused legal C, so every file carrying a continued literal reads UNKNOWN"
fi
NEG="$(grep -cE "$FOR_ERE" "$TMP/negout")"
[ "$NEG" -eq 0 ] || fail "the scanner reported $NEG hit(s) in comments and literals; every finding would be noise"
# check_public_headers.sh pairs the residue with the raw source by line number, so a spliced
# continuation must not shorten it.
NL_IN="$(wc -l < "$TMP/neg.c" | tr -d ' ')"
NL_OUT="$(wc -l < "$TMP/negout" | tr -d ' ')"
[ "$NL_IN" = "$NL_OUT" ] || fail "the scanner emitted $NL_OUT line(s) for $NL_IN input line(s); every finding would cite the wrong line"

# An unclosed literal with no continuation is not classifiable, and the refusal is what keeps
# such a file from reading clean.
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
    grep -cE 'while[[:space:]]*\([[:space:]]*true[[:space:]]*\)' "$TMP/stripped" >> "$TMP/residue"
done < "$TMP/all"

echo "== checked $N tracked C/C++ file(s) for for(;;), comments and literals stripped =="

if [ -s "$TMP/refused" ]; then
    echo "FAIL: the scan could not strip $(wc -l < "$TMP/refused" | tr -d ' ') file(s), so their verdict is UNKNOWN, not clean:" >&2
    sed 's/^/      /' "$TMP/refused" >&2
    exit 1
fi

# Proves the scanner read THIS corpus and not just the planted input: `while (true)` is the
# mandated spelling, so a residue holding none means the strip ate the tree's code.
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
