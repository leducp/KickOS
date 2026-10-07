#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The two spellings docs/reference/style.md bans in the source TEXT of a tracked C or C++
# file, both read off the same comment-stripped residue in one walk:
#
#   the conditional operator. Write if/else, an early return, or a variable set in a branch;
#   for plural selection set a char const* in an if.
#
#   for (;;). Write while (true), so every unbounded loop in the tree is greppable in one
#   pass.
#
# Run from the repo root, no arguments, no build directory:
#   tests/static/check_ternary.sh
#
#   corpus     corpus_sources (tests/lib/gate.sh): tracked C, C++ and assembler source.
#
#   comments   tests/lib/strip_comments.awk blanks comments and literals, one output line per
#              input line, reading each file on stdin. A file it refuses (a block comment or
#              literal open at EOF, a raw string) or cannot run over is REFUSED by name: its
#              verdict is UNKNOWN, not clean.
#
#   the match  a `?` in the residue. After the strip a `?` in C or C++ is the conditional
#              operator, so `::`, labels, `case X:` and bitfields cannot match, and a ternary
#              split over lines still has its `?` on a line of its own. A spliced line
#              reports at the first line it came from.
#
#   directives `#include`, `#include_next`, `#import`, `#error` and `#warning` carry text,
#              not an expression, and are dropped from the `?` hits. `#define` and `#if` are
#              not: a macro body is where a ternary hides.
#
#   witness    `while (true)` must survive the strip somewhere, and some code must: the
#              positive control that the scan read code and not only blanks.
#
# Not covered: awk and shell (their comments are `#`), a ternary the preprocessor assembles,
# and the other unbounded shapes (`while (1)`, a `goto` back-edge).

set -u
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root
scratch_dir

STRIP="$(dirname "$0")/../lib/strip_comments.awk"
[ -r "$STRIP" ] || fail "tests/lib/strip_comments.awk is unreadable; nothing below can tell code from prose"

CORPUS_FLOOR=350
LOOP='for[[:space:]]*[(][[:space:]]*;[[:space:]]*;[[:space:]]*[)]'
WITNESS='while[[:space:]]*[(][[:space:]]*true[[:space:]]*[)]'
TEXTUAL='^[0-9][0-9]*:[[:blank:]]*#[[:blank:]]*(include|include_next|import|error|warning)([^A-Za-z0-9_]|$)'

# detect <file-list> <workdir>: `findings` and `loops` as file:line:text, `refused` with the
# reason per file, and `read`, `code` and `witness` counts per file read.
detect() {
    mkdir -p "$2" || fail "mkdir failed under $2"
    for _o in findings loops refused read code witness; do : > "$2/$_o"; done
    while IFS= read -r f; do
        [ -f "$f" ] || fail "file in the corpus is missing from the worktree: $f"
        if ! awk -f "$STRIP" < "$f" > "$2/stripped" 2> "$2/err"; then
            printf '%s: the strip refused it: %s\n' "$f" "$(head -n1 "$2/err")" >> "$2/refused"
            continue
        fi
        printf '%s\n' "$f" >> "$2/read"
        grep -c '[^[:blank:]]' "$2/stripped" >> "$2/code"
        grep -cE "$WITNESS" "$2/stripped" >> "$2/witness"
        grep -nE "$LOOP" "$2/stripped" | KOS_NAME="$f" awk '{ print ENVIRON["KOS_NAME"] ":" $0 }' >> "$2/loops"
        grep -n '[?]' "$2/stripped" | grep -vE "$TEXTUAL" \
            | KOS_NAME="$f" awk '{ print ENVIRON["KOS_NAME"] ":" $0 }' >> "$2/findings"
    done < "$1"
}

count() { wc -l < "$1" | tr -d ' '; }
lines() { cut -d: -f2 "$1" | tr '\n' ' ' | sed 's/ $//'; }
sum() { awk '{ s += $1 } END { print s + 0 }' "$1"; }

# --- controls: each corpus is scanned by detect() exactly as the tree is -------------------
mkdir -p "$TMP/st"
plant() { cat > "$TMP/st/$1.cc"; printf '%s\n' "$TMP/st/$1.cc" > "$TMP/st/$1.list"; detect "$TMP/st/$1.list" "$TMP/st/$1"; }

# Nine ternaries at lines 4, 5, 6, 8, 10, 14, 17, 18 and 20: split ones, a code line holding an
# exempted word (14), a macro whose continuation (19) reports at the line it opens on, an #if.
plant pos <<'EOF'
int plain(int c)
{
    int a = 0;
    a = (c != 0) ? 1 : 2;
    a = c?1:2;
    a = c ?: 3;
    a = (c != 0)
        ? 4
        : 5;
    a = (c != 0) ?
        6 :
        7;
    int error = c;
    a = error != 0 ? 8 : 9;
    return a;
}
#define KOS_PICK(c, a, b) ((c) ? (a) : (b))
#define KOS_MIN(a, b) \
    (((a) < (b)) ? (a) : (b))
#if defined(KOS_X) ? 1 : 0
#endif
EOF
[ "$(lines "$TMP/st/pos/findings")" = "4 5 6 8 10 14 17 18 20" ] \
    || fail "the planted ternaries reported at lines '$(lines "$TMP/st/pos/findings")', not '4 5 6 8 10 14 17 18 20'"

# A `?` or `:` in a literal, a comment, a label, a bitfield and the textual directives.
plant neg <<'EOF'
char const* q = "is it a ternary? c : a";
char ch = '?';
// why not write c ? a : b here
/* and why not c ? a : b
   in a block that spans lines? */
int n = kickos::sched::count();
default:
case KOS_SYS_YIELD:
public:
    uint32_t mode : 3;
#include <kickos/what?.h>
    #  include <kickos/other?.h>
#error the board names no clock, so now what?
EOF
[ -s "$TMP/st/neg/refused" ] && fail "the scanner refused a file of legal spellings"
[ "$(count "$TMP/st/neg/findings")" -eq 0 ] || fail "the scanner reported a comment, literal, label, bitfield or directive: $(cat "$TMP/st/neg/findings")"

# Four spacings of the loop at lines 3 to 6, then its spellings in comments and literals.
plant loop <<'EOF'
void spin(void)
{
    for(;;) { a(); }
    for (;;) { b(); }
    for( ;; ) { c(); }
    for ( ; ; ) { d(); }
}
// style: for (;;) is banned here
char const* s = "for (;;)";
char c = ';';
while (true) { e(); }
EOF
[ "$(lines "$TMP/st/loop/loops")" = "3 4 5 6" ] || fail "the planted loops reported at lines '$(lines "$TMP/st/loop/loops")', not '3 4 5 6'"
[ "$(sum "$TMP/st/loop/witness")" -eq 1 ] || fail "the witness did not count the one while (true)"

# The strip's two refusals: a block comment that never closes, and a raw string whose own
# content would otherwise decide where the strip stops. Neither may report its planted ternary.
plant open <<'EOF'
/* a block comment that never closes
int a = (c != 0) ? 1 : 2;
EOF
plant raw <<'EOF'
char const* s = R"(" /* )";
int a(int c) { return c ? 1 : 2; }
char const* t = R"(*/")";
EOF
for f in open raw; do
    [ -s "$TMP/st/$f/refused" ] || fail "the $f control was not refused, so an unstrippable file reads clean"
    [ "$(count "$TMP/st/$f/findings")" -eq 0 ] || fail "the $f control reported a residue it should have refused"
done
echo "== control: nine planted ternaries and four loops at their lines, none in prose or literals, and both strip refusals =="

# --- the corpus ---------------------------------------------------------------------------
corpus_sources "$TMP/all"
N="$(count "$TMP/all")"
[ "$N" -ge "$CORPUS_FLOOR" ] || fail "$N source file(s), beneath the floor of $CORPUS_FLOOR: this is not the tree"
detect "$TMP/all" "$TMP/w"

if [ -s "$TMP/w/refused" ]; then
    sed 's/^/      /' "$TMP/w/refused" >&2
    fail "the strip refused $(count "$TMP/w/refused") file(s), so their verdict is UNKNOWN"
fi
[ "$(count "$TMP/w/read")" -eq "$N" ] || fail "the scan read $(count "$TMP/w/read") of $N file(s)"
CODE="$(sum "$TMP/w/code")"
KEPT="$(sum "$TMP/w/witness")"
[ "$CODE" -gt 0 ] && [ "$KEPT" -gt 0 ] || fail "the strip left no code ($CODE line(s)) or no while (true) ($KEPT): the scan read nothing"

rc=0
if [ -s "$TMP/w/findings" ]; then
    cat "$TMP/w/findings" >&2
    echo "FAIL: $(count "$TMP/w/findings") line(s) hold a conditional operator; docs/reference/style.md refuses it." >&2
    echo "      Write if/else, an early return, or a variable set in a branch." >&2
    rc=1
fi
if [ -s "$TMP/w/loops" ]; then
    cat "$TMP/w/loops" >&2
    echo "FAIL: $(count "$TMP/w/loops") for(;;) loop(s). Write while (true)." >&2
    rc=1
fi
[ "$rc" -eq 0 ] || exit 1
echo "PASS: no conditional operator and no for(;;) across $N tracked C/C++ file(s), $CODE line(s) of code read, $KEPT while (true) loop(s)"
