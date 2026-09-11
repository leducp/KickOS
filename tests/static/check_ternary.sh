#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The no-ternary rule of docs/reference/style.md: no conditional operator in a tracked C or
# C++ source file. Write if/else, an early return, or a variable set in a branch; for plural
# selection set a char const* in an if.
#
# Run from the repo root, no arguments, no build directory:
#   tests/static/check_ternary.sh
#
#   corpus     tracked *.c, *.cc, *.cpp, *.h, *.hh, *.hpp, *.inc, *.h.in and *.S. Source
#              only: a scan of the markdown tree would report the prose stating the rule.
#
#   comments   tests/lib/strip_comments.awk blanks the comments and every string and
#              character literal BEFORE the scan, one output line per input line, so a
#              finding cites the real line. A file whose block comment or literal is still
#              open at EOF is REFUSED by name, not skipped: its verdict is UNKNOWN. So is a
#              file the strip could not be run over at all, which is what keeps a dead
#              reader from reading as a clean corpus.
#
#   the match  a `?` in the residue, and nothing else. After the strip, a `?` in a C or C++
#              translation unit is the conditional operator: the language spells no other
#              operator with one and no identifier may hold one. That is what makes `::`, a
#              `default:`, a `case X:`, a goto label, an access specifier `public:` and a
#              bitfield `uint32_t mode : 3` unreachable rather than merely unlisted, none of
#              them holding a `?`. And it is what catches a ternary SPLIT over lines, the
#              shape a single-line `? ... :` pattern misses and the one a writer dodging the
#              rule reaches for: wherever the `:` went, the `?` is still on a line of its
#              own. A line the strip SPLICED reports at the FIRST of the lines it came from,
#              which for a multi-line macro is the line the `#define` opens on.
#
#   directives a directive whose operand is TEXT and not an expression is dropped from the
#              hits, its path or its prose being free to end in a question mark: `#include`,
#              `#include_next`, `#import`, `#error` and `#warning`, recognised at the start
#              of the line only, through leading blanks and blanks after the `#`. A
#              `#define`, an `#if` and an `#elif` are NOT exempt: a macro body is where a
#              ternary hides. A directive that is not on that list and carries a bare `?`
#              reports, and the fix is one word in the list, never a rewrite.
#
# WHAT THIS DOES NOT COVER
#   - the non-C corpus. A conditional operator in an awk program, and a `$((c ? a : b))` in
#     a shell script, are outside it: the strip reads C comments, so over a file whose
#     comments open with `#` it would blank nothing and report the prose.
#   - a ternary the preprocessor ASSEMBLES, out of a macro argument or a token paste, is not
#     in the source text and cannot be seen here.
#   - an arch-specific assembler comment in a `.S`, `@` on arm32 and `;` elsewhere, is not a
#     C comment, so a `?` inside one is a FINDING. Every `.S` here comments in the C forms,
#     and the fix if one lands is to teach the strip, not to widen this gate.
#   - a trigraph reports. C++17 removed them and no file in this tree spells one.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# NOT set -e: every finding must be collected in one run.

require_repo_root

scratch_dir

STRIP="$(dirname "$0")/../lib/strip_comments.awk"
[ -r "$STRIP" ] || fail "tests/lib/strip_comments.awk is unreadable; nothing below can tell code from prose"

# The mark, as a bracket expression and never as `\?`: a backslash before an ERE special is
# undefined in POSIX and only conventionally a literal.
MARK='[?]'

# The textual directives. Anchored on the `<line>:` prefix that `grep -n` puts in front of
# every hit, which is the form this is applied to, so the whole shape reads from `^`.
TEXTUAL='^[0-9][0-9]*:[[:blank:]]*#[[:blank:]]*(include|include_next|import|error|warning)([^A-Za-z0-9_]|$)'

# --- the scanner, as one function, so the self-test runs the SAME program as the tree -----
#
# detect <file-list> <workdir> <strip-program> <mark-ere> <exempt-ere>, leaving in <workdir>:
# `findings` (file:line:text), `refused` (why a file's residue cannot be trusted) and
# `badfiles` (one name per refusal, so the reason text cannot be miscounted as one), `read`
# (one line per file actually scanned) and `code` (a per-file count of non-blank residue
# lines, the positive control that the scan read code at all).
#
# EVERY PART THE SELF-TEST MUTATES IS AN ARGUMENT, so the mutation arms below drive this
# program rather than a copy of it that could drift from it.
detect() {
    _list="$1"
    _w="$2"
    _strip="$3"
    _mark="$4"
    _exempt="$5"
    mkdir -p "$_w" || fail "mkdir failed under $_w"
    : > "$_w/findings"
    : > "$_w/refused"
    : > "$_w/badfiles"
    : > "$_w/read"
    : > "$_w/code"
    while IFS= read -r f; do
        [ -f "$f" ] || fail "file in the corpus is missing from the worktree: $f"
        # ANY nonzero status is a refusal, not just the strip's own exit 2: an awk that dies
        # on its own program exits 1 and prints nothing, and treating that as an empty
        # residue is how a gate goes green over a corpus it never read.
        if awk -f "$_strip" "$f" > "$_w/stripped" 2> "$_w/striperr"; then
            :
        else
            _rc=$?
            printf '%s: the strip exited %s, so the verdict is UNKNOWN\n' "$f" "$_rc" >> "$_w/refused"
            sed 's/^/        /' "$_w/striperr" >> "$_w/refused"
            printf '%s\n' "$f" >> "$_w/badfiles"
            continue
        fi
        _k="$(grep -c '[^[:blank:]]' "$_w/stripped")"
        _rc=$?
        if [ "$_rc" -gt 1 ]; then
            printf '%s: the code-line count exited %s\n' "$f" "$_rc" >> "$_w/refused"
            printf '%s\n' "$f" >> "$_w/badfiles"
            continue
        fi
        printf '%s\n' "$_k" >> "$_w/code"
        printf '%s\n' "$f" >> "$_w/read"
        grep -nE "$_mark" "$_w/stripped" > "$_w/hits"
        _rc=$?
        if [ "$_rc" -gt 1 ]; then
            printf '%s: the scan exited %s over the residue\n' "$f" "$_rc" >> "$_w/refused"
            printf '%s\n' "$f" >> "$_w/badfiles"
            continue
        fi
        [ -s "$_w/hits" ] || continue
        grep -vE "$_exempt" "$_w/hits" > "$_w/real"
        _rc=$?
        if [ "$_rc" -gt 1 ]; then
            printf '%s: the directive filter exited %s\n' "$f" "$_rc" >> "$_w/refused"
            printf '%s\n' "$f" >> "$_w/badfiles"
            continue
        fi
        sed "s|^|$f:|" "$_w/real" >> "$_w/findings"
    done < "$_list"
}

# --- self-test: prove the scanner both ways before reading the tree -----------------------
# One control per clause, each a MINIMAL PAIR, and every expected count is EXACT: a positive
# a neighbouring clause would also catch says nothing about the clause it is named for.
mkdir -p "$TMP/st"

# Nine ternaries, at lines 4, 5, 6, 8, 10, 14, 17, 18 and 20. Line 14 is the near miss for
# the directive anchor, a code line holding one of the exempted words. Line 19 must NOT
# report: the strip splices it onto 18, which is where a reader looks for the macro.
cat > "$TMP/st/pos.cc" <<'EOF'
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

# The near misses, one per clause and one clause per line, so the loop below can read each
# on its own. Lines 1 to 5 are the strip's; 6 to 11 hold a `:` that is not a ternary's and no
# `?` at all; 12 to 15 are the directives.
cat > "$TMP/st/neg.cc" <<'EOF'
char const* q = "is it a ternary? c : a";
char const* z = "?";
char ch = '?';
// why not write c ? a : b here
/* and why not c ? a : b in a block */
int n = kickos::sched::count();
default:
case KOS_SYS_YIELD:
retry:
public:
    uint32_t mode : 3;
#include <kickos/what?.h>
    #  include <kickos/other?.h>
#error the board names no clock, so now what?
#warning is the doorbell really wired?
EOF

# The block comment the per-line loop cannot carry: its state spans lines, and a line of it
# read alone would be refused rather than clean.
cat > "$TMP/st/block.cc" <<'EOF'
/* A block comment spanning lines that asks
   why not c ? a : b, and again c ? a : b,
   and keeps asking? */
int clean(int c)
{
    if (c != 0)
    {
        return 1;
    }
    return 2;
}
EOF

for f in pos neg block; do
    printf '%s\n' "$TMP/st/$f.cc" > "$TMP/st/$f.list"
done

detect "$TMP/st/pos.list" "$TMP/st/pw" "$STRIP" "$MARK" "$TEXTUAL"
if [ -s "$TMP/st/pw/refused" ]; then
    sed 's/^/      /' "$TMP/st/pw/refused" >&2
    fail "the scanner refused its own positive corpus, so it proved nothing about the tree"
fi
POS="$(wc -l < "$TMP/st/pw/findings" | tr -d ' ')"
if [ "$POS" -ne 9 ]; then
    sed 's/^/      /' "$TMP/st/pw/findings" >&2
    fail "the scanner found $POS of 9 planted ternaries; it would miss real ones"
fi
# The LINES, not just the count: this is what pins the two split shapes and the splice, a
# count alone being met by nine hits on the wrong lines.
POSAT="$(cut -d: -f2 "$TMP/st/pw/findings" | tr '\n' ' ' | sed 's/ $//')"
if [ "$POSAT" != "4 5 6 8 10 14 17 18 20" ]; then
    fail "the planted ternaries reported at lines '$POSAT', not '4 5 6 8 10 14 17 18 20';
      a split ternary or the spliced macro body is being read at the wrong line"
fi

detect "$TMP/st/neg.list" "$TMP/st/nw" "$STRIP" "$MARK" "$TEXTUAL"
if [ -s "$TMP/st/nw/refused" ]; then
    sed 's/^/      /' "$TMP/st/nw/refused" >&2
    fail "the scanner refused a file of legal spellings; every such file would read UNKNOWN"
fi
NEG="$(wc -l < "$TMP/st/nw/findings" | tr -d ' ')"
if [ "$NEG" -ne 0 ]; then
    sed 's/^/      /' "$TMP/st/nw/findings" >&2
    fail "the scanner reported $NEG hit(s) on a comment, a literal, a label, a bitfield or a
      directive; the gate would cry wolf and be switched off"
fi

# EACH negative on its own, so a control that is quiet for the WRONG reason is visible. A
# whole-file zero cannot tell "every clause holds" from "one clause swallowed the file".
i=0
while IFS= read -r line; do
    i=$((i + 1))
    printf '%s\n' "$line" > "$TMP/st/one.cc"
    printf '%s\n' "$TMP/st/one.cc" > "$TMP/st/one.list"
    detect "$TMP/st/one.list" "$TMP/st/ow" "$STRIP" "$MARK" "$TEXTUAL"
    if [ -s "$TMP/st/ow/refused" ]; then
        sed 's/^/      /' "$TMP/st/ow/refused" >&2
        fail "negative control $i cannot be read on its own: $line"
    fi
    n="$(wc -l < "$TMP/st/ow/findings" | tr -d ' ')"
    [ "$n" -eq 0 ] || fail "negative control $i reports: $line"
done < "$TMP/st/neg.cc"
[ "$i" -eq 15 ] || fail "$i negative control(s) ran, expected 15"

detect "$TMP/st/block.list" "$TMP/st/bw" "$STRIP" "$MARK" "$TEXTUAL"
if [ -s "$TMP/st/bw/refused" ]; then
    sed 's/^/      /' "$TMP/st/bw/refused" >&2
    fail "the scanner refused a file whose block comment is closed; the residue is unusable"
fi
NEGB="$(wc -l < "$TMP/st/bw/findings" | tr -d ' ')"
if [ "$NEGB" -ne 0 ]; then
    sed 's/^/      /' "$TMP/st/bw/findings" >&2
    fail "the scanner read a multi-line block comment as code; a doc comment naming the rule
      would be a finding in every file that carries one"
fi
BCODE="$(awk '{ s += $1 } END { print s + 0 }' "$TMP/st/bw/code")"
[ "$BCODE" -gt 0 ] || fail "the code-line count read nothing outside the block comment; the
      positive control that this gate reads code at all is dead"

# The mutations the controls exist to survive: disable one clause and the count over the
# control corpus must MOVE by an EXACT amount, so each control is a near miss and not a line
# the gate would have been quiet about anyway.
mutate() { # <clause> <corpus-list> <strip> <mark-ere> <exempt-ere> <expected count>
    detect "$2" "$TMP/st/mw" "$3" "$4" "$5"
    if [ -s "$TMP/st/mw/refused" ]; then
        sed 's/^/      /' "$TMP/st/mw/refused" >&2
        fail "the $1 mutation refused its corpus, so its count says nothing"
    fi
    _m="$(wc -l < "$TMP/st/mw/findings" | tr -d ' ')"
    if [ "$_m" -ne "$6" ]; then
        sed 's/^/      /' "$TMP/st/mw/findings" >&2
        fail "with the $1 clause disabled the scan reported $_m line(s) of $2, expected $6;
      the controls for that clause are not near misses and prove nothing"
    fi
}

# A strip that blanks nothing, which is what the comment and literal clause is. This is a
# stronger mutation than an ERE that cannot match: it proves the residue is what the scan
# actually reads.
printf '%s\n' '{ print }' > "$TMP/st/nostrip.awk"
NEVER='KICKOS_THIS_ERE_MATCHES_NOTHING'
# The exemption narrowed to a `#` in column zero: only the indented `#  include` needs the
# blanks either side of the introducer to be tolerated.
TIGHT='^[0-9][0-9]*:#(include|include_next|import|error|warning)([^A-Za-z0-9_]|$)'
# The exemption with its anchor removed, so the directive words become prose anywhere on the
# line. Only pos.cc line 14 can newly go quiet under it.
LOOSE='(include|include_next|import|error|warning)[^A-Za-z0-9_]'

mutate "comment and literal strip" "$TMP/st/neg.list"   "$TMP/st/nostrip.awk" "$MARK" "$TEXTUAL" 5
mutate "block comment strip"       "$TMP/st/block.list" "$TMP/st/nostrip.awk" "$MARK" "$TEXTUAL" 2
mutate "directive exemption"       "$TMP/st/neg.list"   "$STRIP"              "$MARK" "$NEVER"   4
mutate "directive introducer"      "$TMP/st/neg.list"   "$STRIP"              "$MARK" "$TIGHT"   1
mutate "directive anchor"          "$TMP/st/pos.list"   "$STRIP"              "$MARK" "$LOOSE"   8

# A DEAD READER IS ITS OWN ARM, separate from the planted violation. The program below is a
# syntax error (`and` where awk needs `&&`), so awk reads not one record and exits before it.
# Nine planted ternaries then go unseen, and only the refusal keeps that from reading as a
# clean corpus.
cat > "$TMP/st/dead.awk" <<'EOF'
{ if (1 and 1) { print } }
EOF
detect "$TMP/st/pos.list" "$TMP/st/dw" "$TMP/st/dead.awk" "$MARK" "$TEXTUAL"
if [ ! -s "$TMP/st/dw/refused" ]; then
    fail "a strip program that cannot run was not refused, so a corpus nothing read reports clean"
fi
DEAD="$(wc -l < "$TMP/st/dw/findings" | tr -d ' ')"
[ "$DEAD" -eq 0 ] || fail "the dead-reader arm found $DEAD finding(s); it is not testing a dead reader"
if [ -s "$TMP/st/dw/read" ]; then
    fail "a file the strip could not be run over was counted as read; the corpus tally below
      would not notice a reader that died on every file"
fi

# The other way the residue goes untrustworthy: a legal reader over input it refuses. Every
# line below an unclosed block comment went unread, so the verdict is UNKNOWN and not clean.
cat > "$TMP/st/open.cc" <<'EOF'
/* a block comment that never closes
int a = (c != 0) ? 1 : 2;
EOF
printf '%s\n' "$TMP/st/open.cc" > "$TMP/st/open.list"
detect "$TMP/st/open.list" "$TMP/st/uw" "$STRIP" "$MARK" "$TEXTUAL"
if [ ! -s "$TMP/st/uw/refused" ]; then
    fail "a file whose block comment never closes was not refused; its tail reads clean"
fi

# --- the corpus ---------------------------------------------------------------------------
corpus_sources "$TMP/all"
N="$(wc -l < "$TMP/all" | tr -d ' ')"

detect "$TMP/all" "$TMP/w" "$STRIP" "$MARK" "$TEXTUAL"

echo "== checked $N tracked C/C++ file(s) for a conditional operator, comments and literals stripped =="

if [ -s "$TMP/w/refused" ]; then
    echo "FAIL: the scan could not trust the residue of $(wc -l < "$TMP/w/badfiles" | tr -d ' ') file(s), so their verdict is UNKNOWN, not clean:" >&2
    sed 's/^/      /' "$TMP/w/refused" >&2
    exit 1
fi

READ="$(wc -l < "$TMP/w/read" | tr -d ' ')"
[ "$READ" -eq "$N" ] || fail "the scan read $READ of the $N file(s) in the corpus; a walk that
      ends early passes every absence test below it vacuously"
CODE="$(awk '{ s += $1 } END { print s + 0 }' "$TMP/w/code")"
[ "$CODE" -gt 0 ] || fail "not one line of code survived the strip across $N file(s); the scan
      read no code, so its silence means nothing"

if [ -s "$TMP/w/findings" ]; then
    cat "$TMP/w/findings" >&2
    echo "" >&2
    echo "per-file finding count:" >&2
    cut -d: -f1 "$TMP/w/findings" | sort | uniq -c | sort -rn >&2
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/w/findings" | tr -d ' ') line(s) hold a conditional operator." >&2
    echo "      docs/reference/style.md refuses it: write if/else, an early return, or a" >&2
    echo "      variable set in a branch, and for plural selection a char const* set in an" >&2
    echo "      if. A finding on a line with no conditional is a directive this gate does" >&2
    echo "      not know carries prose; see TEXTUAL at the top of this script." >&2
    exit 1
fi

echo "PASS: no conditional operator across $N tracked C/C++ file(s), $CODE line(s) of code read"
