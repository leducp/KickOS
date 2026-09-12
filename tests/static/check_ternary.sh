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
#   corpus     tracked *.c, *.cc, *.cpp, *.h, *.hh, *.hpp, *.inc, *.h.in and *.S. Source
#              only: a scan of the markdown tree would report the prose stating the rule.
#
#   comments   tests/lib/strip_comments.awk blanks the comments and every string and
#              character literal BEFORE the scan, one output line per input line, so a
#              finding cites the real line. A file whose block comment or literal is still
#              open at EOF, or which opens a raw string, is REFUSED by name, not skipped:
#              its verdict is UNKNOWN. So is a file the strip could not be run over at all,
#              which is what keeps a dead reader from reading as a clean corpus. The strip
#              reads each file on STDIN and never as an awk operand: `git ls-files` names a
#              file at the repo root with no directory in front of it, and awk reads an
#              operand shaped like `z=z.cc` as a variable assignment rather than a file.
#
#   the names  a file name reaches an output through the environment, never interpolated
#              into the text of a sed or awk program: a name holding the substitution
#              delimiter makes the tool fail and its findings vanish.
#
#   floor      CORPUS_FLOOR, at about half what the tree tracks, beside the exact per-run
#              tally: a `git ls-files` that matched almost nothing and a corpus narrowed by
#              an edit here both read as a clean tree otherwise.
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
#   the loop   `for` and a parenthesised pair of bare semicolons, blanks tolerated at every
#              join. The residue is what makes a `for (;;)` named in a comment or quoted in
#              a string a non-finding.
#
#   directives a directive whose operand is TEXT and not an expression is dropped from the
#              `?` hits, its path or its prose being free to end in a question mark:
#              `#include`, `#include_next`, `#import`, `#error` and `#warning`, recognised
#              at the start of the line only, through leading blanks and blanks after the
#              `#`. A `#define`, an `#if` and an `#elif` are NOT exempt: a macro body is
#              where a ternary hides. A directive that is not on that list and carries a
#              bare `?` reports, and the fix is one word in the list, never a rewrite. The
#              loop match takes no exemption: no directive spells `for (;;)` as prose.
#
#   the witness a per-file count of `while (true)` in the residue, summed over the corpus and
#              required to be positive. It is the positive control that the strip left the
#              tree's CODE behind and not only its blanks, in the one shape the loop rule
#              mandates.
#
# WHAT THIS DOES NOT COVER
#   - the non-C corpus. A conditional operator in an awk program, and a `$((c ? a : b))` in
#     a shell script, are outside it: the strip reads C comments, so over a file whose
#     comments open with `#` it would blank nothing and report the prose.
#   - a ternary or a loop the preprocessor ASSEMBLES, out of a macro argument or a token
#     paste, is not in the source text and cannot be seen here.
#   - the other unbounded shapes: `while (1)`, `do {} while (true)`, a `goto` back-edge and
#     a recursive tail call. The rule names one spelling to write and one to refuse.
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

# Sized at about HALF what the corpus holds, so an ordinary deletion still passes while an
# empty walk, a run outside a checkout and a corpus narrowed by an edit here all refuse. The
# corpus count is printed on every run and is not pinned here.
CORPUS_FLOOR=350

# The marks, as bracket expressions and never as `\?` or `\(`: a backslash before an ERE
# special is undefined in POSIX and only conventionally a literal.
MARK='[?]'
LOOP='for[[:space:]]*[(][[:space:]]*;[[:space:]]*;[[:space:]]*[)]'
WITNESS='while[[:space:]]*[(][[:space:]]*true[[:space:]]*[)]'

# The textual directives. Anchored on the `<line>:` prefix that `grep -n` puts in front of
# every hit, which is the form this is applied to, so the whole shape reads from `^`.
TEXTUAL='^[0-9][0-9]*:[[:blank:]]*#[[:blank:]]*(include|include_next|import|error|warning)([^A-Za-z0-9_]|$)'

# --- the scanner, as one function, so the self-test runs the SAME program as the tree -----
#
# detect <file-list> <workdir> <strip-program> <mark-ere> <exempt-ere> <loop-ere>
#        <witness-ere>, leaving in <workdir>:
# `findings` (file:line:text for the mark), `loops` (the same for the loop), `refused` (why a
# file's residue cannot be trusted) and `badfiles` (one name per refusal, so the reason text
# cannot be miscounted as one), `read` (one line per file actually scanned), `code` (a
# per-file count of non-blank residue lines) and `witness` (a per-file count of the witness
# spelling), the last two being the positive controls that the scan read code at all.
#
# EVERY PART THE SELF-TEST MUTATES IS AN ARGUMENT, so the mutation arms below drive this
# program rather than a copy of it that could drift from it.

# The name goes in through the environment because a name is data: interpolated into a sed
# replacement, a `|` in it closes the substitution and sed exits nonzero having printed
# nothing, which discards every finding of that file.
prefix_name() { # <name> <file>
    KOS_NAME="$1" awk '{ print ENVIRON["KOS_NAME"] ":" $0 }' < "$2"
}

detect() {
    _list="$1"
    _w="$2"
    _strip="$3"
    _mark="$4"
    _exempt="$5"
    _loop="$6"
    _wit="$7"
    mkdir -p "$_w" || fail "mkdir failed under $_w"
    : > "$_w/findings"
    : > "$_w/loops"
    : > "$_w/refused"
    : > "$_w/badfiles"
    : > "$_w/read"
    : > "$_w/code"
    : > "$_w/witness"
    while IFS= read -r f; do
        [ -f "$f" ] || fail "file in the corpus is missing from the worktree: $f"
        # ANY nonzero status is a refusal, not just the strip's own exit 2: an awk that dies
        # on its own program exits 1 and prints nothing, and treating that as an empty
        # residue is how a gate goes green over a corpus it never read.
        if awk -f "$_strip" < "$f" > "$_w/stripped" 2> "$_w/striperr"; then
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
        _t="$(grep -cE "$_wit" "$_w/stripped")"
        _rc=$?
        if [ "$_rc" -gt 1 ]; then
            printf '%s: the witness count exited %s\n' "$f" "$_rc" >> "$_w/refused"
            printf '%s\n' "$f" >> "$_w/badfiles"
            continue
        fi
        printf '%s\n' "$_k" >> "$_w/code"
        printf '%s\n' "$_t" >> "$_w/witness"
        printf '%s\n' "$f" >> "$_w/read"
        grep -nE "$_loop" "$_w/stripped" > "$_w/lhits"
        _rc=$?
        if [ "$_rc" -gt 1 ]; then
            printf '%s: the loop scan exited %s over the residue\n' "$f" "$_rc" >> "$_w/refused"
            printf '%s\n' "$f" >> "$_w/badfiles"
            continue
        fi
        if prefix_name "$f" "$_w/lhits" >> "$_w/loops"; then
            :
        else
            _rc=$?
            printf '%s: naming the loop hits exited %s\n' "$f" "$_rc" >> "$_w/refused"
            printf '%s\n' "$f" >> "$_w/badfiles"
            continue
        fi
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
        if prefix_name "$f" "$_w/real" >> "$_w/findings"; then
            :
        else
            _rc=$?
            printf '%s: naming the findings exited %s\n' "$f" "$_rc" >> "$_w/refused"
            printf '%s\n' "$f" >> "$_w/badfiles"
            continue
        fi
    done < "$_list"
}

# The count of a detect output, as one word, so an arm reads `4` and not a pipeline.
countof() { # <workdir> <file>
    wc -l < "$1/$2" | tr -d ' '
}

# The lines an output cites, space separated, so an arm can pin WHERE and not only how many.
linesof() { # <workdir> <file>
    cut -d: -f2 "$1/$2" | tr '\n' ' ' | sed 's/ $//'
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

# The four spacings of the banned loop, at lines 3, 4, 5 and 6. Only the first needs no
# blank tolerance, which is what the tightened mutation below turns on.
cat > "$TMP/st/loop.cc" <<'EOF'
void spin(void)
{
    for(;;) { a(); }
    for (;;) { b(); }
    for( ;; ) { c(); }
    for ( ; ; ) { d(); }
}
EOF

# The loop's near misses: the spelling inside a line comment, inside a block comment spanning
# lines, inside a string, and inside a backslash-continued string. Line 5 is the character
# literal that holds the semicolon the match reads. Line 8 is the witness.
cat > "$TMP/st/loopneg.cc" <<'EOF'
// style: for (;;) is banned here
/* a block comment naming for (;;) over
   two lines, still for(;;) */
char const* s = "for (;;)";
char c = ';';
char const* t = "for (;;) inside a \
continued literal, which is legal C";
while (true) { e(); }
EOF

# A continued literal SPLICES, and the residue of a splice prints on the first of the lines
# it came from with the rest blank. A strip that instead shortened its output would cite the
# loop below at line 2.
cat > "$TMP/st/cont.cc" <<'EOF'
char const* t = "a literal continued \
across two lines";
for (;;) { spin(); }
EOF

for f in pos neg block loop loopneg cont; do
    printf '%s\n' "$TMP/st/$f.cc" > "$TMP/st/$f.list"
done

detect "$TMP/st/pos.list" "$TMP/st/pw" "$STRIP" "$MARK" "$TEXTUAL" "$LOOP" "$WITNESS"
if [ -s "$TMP/st/pw/refused" ]; then
    sed 's/^/      /' "$TMP/st/pw/refused" >&2
    fail "the scanner refused its own positive corpus, so it proved nothing about the tree"
fi
POS="$(countof "$TMP/st/pw" findings)"
if [ "$POS" -ne 9 ]; then
    sed 's/^/      /' "$TMP/st/pw/findings" >&2
    fail "the scanner found $POS of 9 planted ternaries; it would miss real ones"
fi
# The LINES, not just the count: this is what pins the two split shapes and the splice, a
# count alone being met by nine hits on the wrong lines.
POSAT="$(linesof "$TMP/st/pw" findings)"
if [ "$POSAT" != "4 5 6 8 10 14 17 18 20" ]; then
    fail "the planted ternaries reported at lines '$POSAT', not '4 5 6 8 10 14 17 18 20';
      a split ternary or the spliced macro body is being read at the wrong line"
fi

detect "$TMP/st/neg.list" "$TMP/st/nw" "$STRIP" "$MARK" "$TEXTUAL" "$LOOP" "$WITNESS"
if [ -s "$TMP/st/nw/refused" ]; then
    sed 's/^/      /' "$TMP/st/nw/refused" >&2
    fail "the scanner refused a file of legal spellings; every such file would read UNKNOWN"
fi
NEG="$(countof "$TMP/st/nw" findings)"
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
    detect "$TMP/st/one.list" "$TMP/st/ow" "$STRIP" "$MARK" "$TEXTUAL" "$LOOP" "$WITNESS"
    if [ -s "$TMP/st/ow/refused" ]; then
        sed 's/^/      /' "$TMP/st/ow/refused" >&2
        fail "negative control $i cannot be read on its own: $line"
    fi
    n="$(countof "$TMP/st/ow" findings)"
    [ "$n" -eq 0 ] || fail "negative control $i reports: $line"
done < "$TMP/st/neg.cc"
[ "$i" -eq 15 ] || fail "$i negative control(s) ran, expected 15"

detect "$TMP/st/block.list" "$TMP/st/bw" "$STRIP" "$MARK" "$TEXTUAL" "$LOOP" "$WITNESS"
if [ -s "$TMP/st/bw/refused" ]; then
    sed 's/^/      /' "$TMP/st/bw/refused" >&2
    fail "the scanner refused a file whose block comment is closed; the residue is unusable"
fi
NEGB="$(countof "$TMP/st/bw" findings)"
if [ "$NEGB" -ne 0 ]; then
    sed 's/^/      /' "$TMP/st/bw/findings" >&2
    fail "the scanner read a multi-line block comment as code; a doc comment naming the rule
      would be a finding in every file that carries one"
fi
BCODE="$(awk '{ s += $1 } END { print s + 0 }' "$TMP/st/bw/code")"
[ "$BCODE" -gt 0 ] || fail "the code-line count read nothing outside the block comment; the
      positive control that this gate reads code at all is dead"

# The loop's positive: four spacings, and the lines, so a match that fires four times on one
# line cannot stand in for one that reads all four.
detect "$TMP/st/loop.list" "$TMP/st/lw" "$STRIP" "$MARK" "$TEXTUAL" "$LOOP" "$WITNESS"
if [ -s "$TMP/st/lw/refused" ]; then
    sed 's/^/      /' "$TMP/st/lw/refused" >&2
    fail "the scanner refused its own loop corpus, so it proved nothing about the tree"
fi
LPOS="$(countof "$TMP/st/lw" loops)"
if [ "$LPOS" -ne 4 ]; then
    sed 's/^/      /' "$TMP/st/lw/loops" >&2
    fail "the scanner found $LPOS of 4 planted for(;;) spellings; it would miss real ones"
fi
LPOSAT="$(linesof "$TMP/st/lw" loops)"
[ "$LPOSAT" = "3 4 5 6" ] || fail "the planted loops reported at lines '$LPOSAT', not '3 4 5 6'"
LXT="$(countof "$TMP/st/lw" findings)"
[ "$LXT" -eq 0 ] || fail "the loop corpus reported $LXT conditional operator(s); the two
      matches are reading each other's shapes"

# The loop's negatives, and the witness with them: line 8 is the only `while (true)` in that
# file, so a witness count of anything but 1 means the count is reading the wrong thing.
detect "$TMP/st/loopneg.list" "$TMP/st/lnw" "$STRIP" "$MARK" "$TEXTUAL" "$LOOP" "$WITNESS"
if [ -s "$TMP/st/lnw/refused" ]; then
    sed 's/^/      /' "$TMP/st/lnw/refused" >&2
    fail "the scanner refused a file carrying a backslash-continued literal, which is legal
      C; every such file in the tree would read UNKNOWN"
fi
LNEG="$(countof "$TMP/st/lnw" loops)"
if [ "$LNEG" -ne 0 ]; then
    sed 's/^/      /' "$TMP/st/lnw/loops" >&2
    fail "the scanner reported $LNEG for(;;) hit(s) in a comment or a literal; every finding
      would be noise"
fi
LWIT="$(awk '{ s += $1 } END { print s + 0 }' "$TMP/st/lnw/witness")"
[ "$LWIT" -eq 1 ] || fail "the witness counted $LWIT while (true) in a file holding exactly
      one; the corpus control below it would be measuring something else"

# The splice, pinned by WHERE and not by a line tally: a strip that emitted one line for the
# continuation would cite this loop at 2.
detect "$TMP/st/cont.list" "$TMP/st/cw" "$STRIP" "$MARK" "$TEXTUAL" "$LOOP" "$WITNESS"
if [ -s "$TMP/st/cw/refused" ]; then
    sed 's/^/      /' "$TMP/st/cw/refused" >&2
    fail "the scanner refused a continued literal; the residue is unusable"
fi
CAT="$(linesof "$TMP/st/cw" loops)"
[ "$CAT" = "3" ] || fail "the loop after a spliced literal reported at line '$CAT', not '3';
      every finding below a continued literal would cite the wrong line"

# The mutations the controls exist to survive: disable one clause and the count over the
# control corpus must MOVE by an EXACT amount, so each control is a near miss and not a line
# the gate would have been quiet about anyway.
mutate() { # <clause> <corpus-list> <strip> <mark-ere> <exempt-ere> <loop-ere> <out> <count>
    detect "$2" "$TMP/st/mw" "$3" "$4" "$5" "$6" "$WITNESS"
    if [ -s "$TMP/st/mw/refused" ]; then
        sed 's/^/      /' "$TMP/st/mw/refused" >&2
        fail "the $1 mutation refused its corpus, so its count says nothing"
    fi
    _m="$(countof "$TMP/st/mw" "$7")"
    if [ "$_m" -ne "$8" ]; then
        sed 's/^/      /' "$TMP/st/mw/$7" >&2
        fail "with the $1 clause disabled the scan reported $_m line(s) of $2, expected $8;
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
# The loop match with every blank tolerance removed. Only loop.cc line 3 spells it that way.
LTIGHT='for[(];;[)]'

mutate "comment and literal strip" "$TMP/st/neg.list"     "$TMP/st/nostrip.awk" "$MARK"  "$TEXTUAL" "$LOOP"   findings 5
mutate "block comment strip"       "$TMP/st/block.list"   "$TMP/st/nostrip.awk" "$MARK"  "$TEXTUAL" "$LOOP"   findings 2
mutate "directive exemption"       "$TMP/st/neg.list"     "$STRIP"              "$MARK"  "$NEVER"   "$LOOP"   findings 4
mutate "directive introducer"      "$TMP/st/neg.list"     "$STRIP"              "$MARK"  "$TIGHT"   "$LOOP"   findings 1
mutate "directive anchor"          "$TMP/st/pos.list"     "$STRIP"              "$MARK"  "$LOOSE"   "$LOOP"   findings 8
mutate "loop strip"                "$TMP/st/loopneg.list" "$TMP/st/nostrip.awk" "$MARK"  "$TEXTUAL" "$LOOP"   loops    5
mutate "loop blank tolerance"      "$TMP/st/loop.list"    "$STRIP"              "$MARK"  "$TEXTUAL" "$LTIGHT" loops    1

# A DEAD READER IS ITS OWN ARM, separate from the planted violation. The program below is a
# syntax error every awk refuses, so awk reads not one record and exits before it. Nine
# planted ternaries then go unseen, and only the refusal keeps that from reading as a clean
# corpus. NOT `1 and 1`: gawk refuses that spelling, mawk reads it as a concatenation of an
# unset variable and runs the program, so the arm below would fail on the awk of the
# commonest CI image and pass on a developer box.
cat > "$TMP/st/dead.awk" <<'EOF'
{ print ( }
EOF
detect "$TMP/st/pos.list" "$TMP/st/dw" "$TMP/st/dead.awk" "$MARK" "$TEXTUAL" "$LOOP" "$WITNESS"
if [ ! -s "$TMP/st/dw/refused" ]; then
    fail "a strip program that cannot run was not refused, so a corpus nothing read reports clean"
fi
DEAD="$(countof "$TMP/st/dw" findings)"
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
detect "$TMP/st/open.list" "$TMP/st/uw" "$STRIP" "$MARK" "$TEXTUAL" "$LOOP" "$WITNESS"
if [ ! -s "$TMP/st/uw/refused" ]; then
    fail "a file whose block comment never closes was not refused; its tail reads clean"
fi

# An unclosed LITERAL is the strip's other refusal, and a separate state from the block
# comment above. The planted loop below it must not surface as a finding: a file the strip
# could not classify has no verdict at all.
printf 'char const* u = "never closed;\nfor (;;) { g(); }\n' > "$TMP/st/unterm.cc"
printf '%s\n' "$TMP/st/unterm.cc" > "$TMP/st/unterm.list"
detect "$TMP/st/unterm.list" "$TMP/st/tw" "$STRIP" "$MARK" "$TEXTUAL" "$LOOP" "$WITNESS"
if [ ! -s "$TMP/st/tw/refused" ]; then
    fail "a file whose string literal never closes was not refused, so an unstrippable file
      reports clean"
fi
UNT="$(countof "$TMP/st/tw" loops)"
[ "$UNT" -eq 0 ] || fail "the unclosed-literal arm reported $UNT loop(s); it is reading a
      residue it has already declared UNKNOWN"

# The raw string, which is the third refusal and the one that exits 0 when it is guessed at.
# Both lines below are valid C++: the first holds a quote and a comment OPENER as content, the
# second a comment CLOSER, so a strip reading them as ordinary literals opens a block comment
# on the first and closes it on the second, blanks everything between and exits clean. The two
# planted violations sit in that span.
cat > "$TMP/st/raw.cc" <<'EOF'
char const* s = R"(" /* )";
int a(int c) { return c ? 1 : 2; }
void spin(void) { for (;;) { b(); } }
char const* t = R"(*/")";
EOF
printf '%s\n' "$TMP/st/raw.cc" > "$TMP/st/raw.list"
detect "$TMP/st/raw.list" "$TMP/st/rw" "$STRIP" "$MARK" "$TEXTUAL" "$LOOP" "$WITNESS"
if [ ! -s "$TMP/st/rw/refused" ]; then
    fail "a file opening a raw string was not refused; its own content decides where the strip
      stops, so the code after it reads clean"
fi
RAWX="$(countof "$TMP/st/rw" findings)"
RAWL="$(countof "$TMP/st/rw" loops)"
[ "$RAWX" -eq 0 ] && [ "$RAWL" -eq 0 ] || fail "the raw-string arm reported $RAWX finding(s)
      and $RAWL loop(s); it is reading a residue it has already declared UNKNOWN"

# An `R` that does not open a raw string must not refuse, or every file naming a register in a
# literal reads UNKNOWN: inside a literal, and at the tail of an identifier.
cat > "$TMP/st/rawneg.cc" <<'EOF'
char const* rw = "R";
char const* p = KOS_R"x";
while (true) { c(); }
EOF
printf '%s\n' "$TMP/st/rawneg.cc" > "$TMP/st/rawneg.list"
detect "$TMP/st/rawneg.list" "$TMP/st/rnw" "$STRIP" "$MARK" "$TEXTUAL" "$LOOP" "$WITNESS"
if [ -s "$TMP/st/rnw/refused" ]; then
    sed 's/^/      /' "$TMP/st/rnw/refused" >&2
    fail "the raw-string refusal fires on an R that opens no raw string; the files that name a
      register in a literal would all read UNKNOWN"
fi

# THE HOSTILE NAMES, AND THE RELATIVE PATH IS THE POINT: `git ls-files` names a file at the
# repo root with no directory in front of it, and only a bare name can be read as an awk
# assignment. The arm runs the scanner from the directory holding them, which is the only way
# to hand the list one. A subshell cannot fail() the run, so its status is read.
mkdir -p "$TMP/st/hostile"
printf 'int a(int c) { return c ? 1 : 2; }\n' > "$TMP/st/hostile/z=z.cc"
printf 'int b(int c) { return c ? 3 : 4; }\nvoid spin(void) { for (;;) { d(); } }\n' \
    > "$TMP/st/hostile/z|x.cc"
printf 'z=z.cc\nz|x.cc\n' > "$TMP/st/hostile/list"
ABS="$STRIP"
case "$ABS" in
    /*) ;;
    *) ABS="$PWD/$ABS" ;;
esac
( cd "$TMP/st/hostile" && detect list w "$ABS" "$MARK" "$TEXTUAL" "$LOOP" "$WITNESS" ) \
    || fail "the hostile-name arm could not be run at all"
if [ -s "$TMP/st/hostile/w/refused" ]; then
    sed 's/^/      /' "$TMP/st/hostile/w/refused" >&2
    fail "the scanner refused an ordinary file for its NAME alone"
fi
HN="$(countof "$TMP/st/hostile/w" findings)"
HL="$(countof "$TMP/st/hostile/w" loops)"
if [ "$HN" -ne 2 ] || [ "$HL" -ne 1 ]; then
    fail "the hostile-name arm found $HN of 2 ternaries and $HL of 1 loop: a name shaped like
      an awk assignment leaves the file unopened, and one holding the substitution delimiter
      makes the naming step fail and throw its findings away. Both read as a clean file."
fi

# --- the corpus ---------------------------------------------------------------------------
corpus_sources "$TMP/all"
N="$(wc -l < "$TMP/all" | tr -d ' ')"
[ "$N" -ge "$CORPUS_FLOOR" ] \
    || fail "$N source file(s) in the corpus, beneath the floor of $CORPUS_FLOOR: this is not
      the tree, so a clean result below would be a corpus that shrank and not a tree that is
      clean."

detect "$TMP/all" "$TMP/w" "$STRIP" "$MARK" "$TEXTUAL" "$LOOP" "$WITNESS"

echo "== checked $N tracked C/C++ file(s) for a conditional operator and for(;;), comments and literals stripped =="

if [ -s "$TMP/w/refused" ]; then
    echo "FAIL: the scan could not trust the residue of $(countof "$TMP/w" badfiles) file(s), so their verdict is UNKNOWN, not clean:" >&2
    sed 's/^/      /' "$TMP/w/refused" >&2
    exit 1
fi

READ="$(countof "$TMP/w" read)"
[ "$READ" -eq "$N" ] || fail "the scan read $READ of the $N file(s) in the corpus; a walk that
      ends early passes every absence test below it vacuously"
CODE="$(awk '{ s += $1 } END { print s + 0 }' "$TMP/w/code")"
[ "$CODE" -gt 0 ] || fail "not one line of code survived the strip across $N file(s); the scan
      read no code, so its silence means nothing"
KEPT="$(awk '{ s += $1 } END { print s + 0 }' "$TMP/w/witness")"
[ "$KEPT" -gt 0 ] || fail "not one while (true) survived the strip across $N file(s); the scan
      read no loop code, so the absence of for(;;) below means nothing"

rc=0

if [ -s "$TMP/w/findings" ]; then
    cat "$TMP/w/findings" >&2
    echo "" >&2
    echo "per-file finding count:" >&2
    cut -d: -f1 "$TMP/w/findings" | sort | uniq -c | sort -rn >&2
    echo "" >&2
    echo "FAIL: $(countof "$TMP/w" findings) line(s) hold a conditional operator." >&2
    echo "      docs/reference/style.md refuses it: write if/else, an early return, or a" >&2
    echo "      variable set in a branch, and for plural selection a char const* set in an" >&2
    echo "      if. A finding on a line with no conditional is a directive this gate does" >&2
    echo "      not know carries prose; see TEXTUAL at the top of this script." >&2
    rc=1
fi

if [ -s "$TMP/w/loops" ]; then
    cat "$TMP/w/loops" >&2
    echo "" >&2
    echo "FAIL: $(countof "$TMP/w" loops) for(;;) loop(s)." >&2
    echo "      Write while (true). One spelling is what makes every unbounded loop in this" >&2
    echo "      tree greppable in one pass." >&2
    rc=1
fi

[ "$rc" -eq 0 ] || exit 1

echo "PASS: no conditional operator and no for(;;) across $N tracked C/C++ file(s), $CODE line(s) of code read, $KEPT while (true) loop(s)"
