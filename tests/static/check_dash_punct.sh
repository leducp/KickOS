#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The dash rule of docs/reference/style.md: ` -- ` is essay punctuation and does not belong
# in software. No comment and no string literal in a tracked source file spells a break in a
# sentence as a double hyphen. Write a comma, a semicolon, a single ` - `, or two sentences.
#
# Run from the repo root, no arguments, no build directory:
#   tests/static/check_dash_punct.sh
#
#   corpus     tracked source, script and build files: the C family, *.ld, *.py, *.sh,
#              *.cmake, *.awk, *.yml, CMakeLists.txt and Kconfig. Comments and literals
#              alike, because the rule is about text a human reads, not about where the
#              compiler puts it.
#
#   the match  EXACTLY TWO hyphens with a blank before, and after them a blank, the end
#              of the line, or the character that closes the text they sit in: a quote or
#              the backslash of an escape, so `"... exhausted --"` reports too. The count
#              and the LEADING blank are what make the three commonest legal spellings
#              unreachable rather than merely unlisted:
#                --flag      no blank after the hyphens
#                i--, --i    no blank before, or none after
#                ---- ----   three or more hyphens is a run, not a pair
#
#   separator  a `--` that ends a command's options, `git ls-files -- '*.c'` and
#              `grep -qF -- "$x"`, is erased before the match. Recognised structurally: a
#              command word at a command position, then option words only, then `--`. A
#              command position also tolerates leading `NAME=value` assignments
#              (`CDPATH= cd -- "$dir"`) and one leading `"${NAME[@]}"` array invocation
#              (`"${SSH[@]}" bash -s -- "$@"`, the array holding the real command). The
#              command list is SEP below, kept generous on purpose so a fresh exemption is
#              not needed every month; a tool that takes `--` and is still not on it
#              reports, and the fix is one word in that list, never a rewrite of working
#              shell.
#
#   quoted     a `--` inside a matched pair of backticks is the token being NAMED, not
#              punctuation: a comment documenting `++ -- +=` is describing the decrement
#              operator. The span between the two backticks is erased before the match.
#
#   banner     a `--` closing a section banner, `# --- Selection --`, is erased at end of
#              line when the line also holds a run of three or more hyphens. Prose earlier
#              on the same line still reports.
#
#   heredoc    a heredoc body in a shell script is skipped whole. It is data, and a gate's
#              self-test corpus has to be free to plant the spellings the gate refuses.
#              The scan REFUSES a file whose heredoc is never terminated: everything below
#              it went unread, so its verdict is UNKNOWN and not clean.
#
# THEREFORE NOT CAUGHT. Know these before trusting a green run:
#   - *.md. Documentation is prose and its 4000-odd dashes are a separate decision, not
#     this rule; commit messages are not in the tree at all and no static gate can see one.
#   - a line that carries BOTH a real separator and prose, or both a banner and prose after
#     the banner's own trailing pair. The erase is per occurrence, but a prose pair sitting
#     inside the command-word span an erase covers goes with it.
#   - an em dash or an en dash spelled as such: that is check_ascii.sh's rule, and the byte
#     is what it reads.
#   - a `#`-to-end-of-line assembler comment in a *.S file is scanned like any other text,
#     but a `--` there has never appeared; the file is read as bytes, not parsed.
#   - an untracked file, and any extension outside the corpus above.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# NOT set -e: the point is to collect EVERY finding in one run, not to stop at the first.

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

scratch_dir

SCAN="$(dirname "$0")/dash_punct.awk"
[ -r "$SCAN" ] || fail "tests/static/dash_punct.awk is unreadable; nothing below can scan a line"

# The four EREs, defined once and handed to the scanner, so the self-test below and the
# corpus scan cannot disagree about what the rule is.
DASH_ERE='(^|[^-])[[:blank:]]--([[:blank:]"'\''\\\\]|$)'
RUN_ERE='---'
TICK_ERE='`[^`]*`'
SEP_ERE='(^|[;&|(]|[$][(])[[:blank:]]*("[$][{][A-Za-z_][A-Za-z0-9_]*[[]@[]][}]"[[:blank:]]+)?([A-Za-z_][A-Za-z0-9_]*=[^[:blank:]]*[[:blank:]]+)*(command[[:blank:]]+)?(git[[:blank:]]+[a-z][a-z-]*|git|grep|egrep|fgrep|xargs|find|rm|mv|cp|ls|printf|echo|sed|awk|install|chmod|env|test|ctest|cmake|nm|objcopy|objdump|readelf|size|python3|diff|sort|head|tail|cut|tr|kill|bash|sh|cd|dirname)([[:blank:]]+-[A-Za-z0-9][^[:blank:]]*)*[[:blank:]]+--([[:blank:]]|$)'

scan() { # <file> <heredoc 0|1>
    LC_ALL=C awk -v DASH="$DASH_ERE" -v RUN="$RUN_ERE" -v SEP="$SEP_ERE" -v TICK="$TICK_ERE" \
        -v HEREDOC="$2" -f "$SCAN" "$1"
}

# --- self-test: prove every clause of the rule, one control per clause ---------
# A gate whose positive control could be caught by an unrelated clause proves nothing about
# the clause it claims to test, so each control below is a MINIMAL PAIR: the positive and
# the negative differ in one property only, and the expected count is exact.
cat > "$TMP/pos.sh" <<'EOF'
echo "the arena is full -- the spawn returns ENOMEM"
# a comment that breaks -- right here
echo "FAIL: the arena is exhausted --" >&2
# you kill -- the runaway process
# the cd -- command changes directory
frobnicate -- "-$PID"
EOF
cat > "$TMP/neg.sh" <<'EOF'
prog --help
git ls-files -- '*.c' '*.h'
printf '%s\n' "$OUT" | grep -qF -- "$expect"
# --- Selection --
# ---- box rule ----
i--; --j
# the `++ -- +=` family applied to an atomic
  kill -- "-$READER" 2>/dev/null
"${SSH[@]}" bash -s -- "$BOARD" "$APP"
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
sh -- "$@"
EOF
cat > "$TMP/here.sh" <<'EOF'
cat > "$TMP/corpus" <<'INNER'
a planted violation -- inside a heredoc body
INNER
echo "and this one -- is real"
EOF

POS="$(scan "$TMP/pos.sh" 0 | wc -l | tr -d ' ')"
[ "$POS" -eq 6 ] || fail "the scanner found $POS of 6 planted pairs; it would miss real ones"

if ! scan "$TMP/neg.sh" 0 > "$TMP/negout" 2> "$TMP/negerr"; then
    sed 's/^/      /' "$TMP/negerr" >&2
    fail "the scanner refused a file of legal spellings; every such file would read UNKNOWN"
fi
if [ -s "$TMP/negout" ]; then
    sed 's/^/      /' "$TMP/negout" >&2
    fail "the scanner reported a flag, a pathspec separator, a banner or a decrement; the
      gate would cry wolf and be switched off"
fi

# EACH negative on its own, so a control that is silent for the WRONG reason is visible. A
# whole-file zero cannot tell "six clauses work" from "one clause swallowed the file".
i=0
while IFS= read -r line; do
    i=$((i + 1))
    printf '%s\n' "$line" > "$TMP/one.sh"
    n="$(scan "$TMP/one.sh" 0 | wc -l | tr -d ' ')"
    [ "$n" -eq 0 ] || fail "negative control $i reports: $line"
done < "$TMP/neg.sh"
[ "$i" -eq 12 ] || fail "$i negative control(s) ran, expected 12"

# The mutation the negatives exist to survive: turn each exemption OFF and the control must
# then report, which is what proves the control was ever a near miss. Done by feeding the
# scanner a rule with that one clause emptied.
mutate() { # <what> <run-ere> <sep-ere> <tick-ere> <expect-count>
    m="$(LC_ALL=C awk -v DASH="$DASH_ERE" -v RUN="$2" -v SEP="$3" -v TICK="$4" -v HEREDOC=0 \
            -f "$SCAN" "$TMP/neg.sh" | wc -l | tr -d ' ')"
    [ "$m" -eq "$5" ] || fail "with the $1 clause disabled the scanner reported $m line(s), expected $5;
      the negative controls for it are not near misses and prove nothing"
}
# Disabled = an ERE that cannot match. The count is EXACT and differs per clause, so a
# control kept quiet by the wrong clause shows up as the wrong number rather than as a pass.
NEVER='KICKOS_THIS_ERE_MATCHES_NOTHING'
mutate "separator" "$RUN_ERE" "$NEVER"   "$TICK_ERE" 7
mutate "banner"    "$NEVER"   "$SEP_ERE" "$TICK_ERE" 1
mutate "backtick"  "$RUN_ERE" "$SEP_ERE" "$NEVER"    1

# SEP has two sub-clauses beyond the plain command word, each with its own near miss above,
# so each gets its own disabled-variant proof: a control quiet because the WORD is
# recognised must not be confused with one quiet because the PREFIX in front of it is
# tolerated. Self-test only; the corpus scan never sees these two.
SEP_ERE_NOPREFIX='(^|[;&|(]|[$][(])[[:blank:]]*(command[[:blank:]]+)?(git[[:blank:]]+[a-z][a-z-]*|git|grep|egrep|fgrep|xargs|find|rm|mv|cp|ls|printf|echo|sed|awk|install|chmod|env|test|ctest|cmake|nm|objcopy|objdump|readelf|size|python3|diff|sort|head|tail|cut|tr|kill|bash|sh|cd|dirname)([[:blank:]]+-[A-Za-z0-9][^[:blank:]]*)*[[:blank:]]+--([[:blank:]]|$)'
SEP_ERE_OLDWORDS='(^|[;&|(]|[$][(])[[:blank:]]*("[$][{][A-Za-z_][A-Za-z0-9_]*[[]@[]][}]"[[:blank:]]+)?([A-Za-z_][A-Za-z0-9_]*=[^[:blank:]]*[[:blank:]]+)*(command[[:blank:]]+)?(git[[:blank:]]+[a-z][a-z-]*|git|grep|egrep|fgrep|xargs|find|rm|mv|cp|ls|printf|echo|sed|awk|install|chmod|env|test|ctest|cmake|nm|objcopy|objdump|readelf|size|python3|diff|sort|head|tail|cut|tr)([[:blank:]]+-[A-Za-z0-9][^[:blank:]]*)*[[:blank:]]+--([[:blank:]]|$)'
# NOPREFIX keeps the new words but drops the VAR=/array tolerance: only the three lines that
# actually need a prefix (CDPATH= cd, CDPATH= cd via $(dirname, and the ssh-array bash) must
# newly report.
mutate "sep-prefix"    "$RUN_ERE" "$SEP_ERE_NOPREFIX" "$TICK_ERE" 3
# OLDWORDS keeps the prefix tolerance but drops kill/bash/sh/cd/dirname from the list: every
# line that exists to prove one of those five words must newly report.
mutate "sep-new-words" "$RUN_ERE" "$SEP_ERE_OLDWORDS" "$TICK_ERE" 5

# The heredoc body must be skipped and the line AFTER it must not be.
H="$(scan "$TMP/here.sh" 1)"
case "$H" in
    *"is real"*) ;;
    *) fail "the line after a heredoc went unscanned; a shell gate's whole tail would read clean" ;;
esac
case "$H" in
    *"inside a heredoc body"*) fail "a heredoc body was scanned; no gate could plant its own corpus" ;;
    *) ;;
esac
[ "$(printf '%s\n' "$H" | wc -l | tr -d ' ')" -eq 1 ] || fail "the heredoc control reported more than the one line it should"

# An unterminated heredoc hides every line below it, and only the refusal keeps that from
# reading clean.
cat > "$TMP/open.sh" <<'EOF'
cat <<INNER
a violation -- here
EOF
if scan "$TMP/open.sh" 1 > /dev/null 2>&1; then
    fail "the scanner accepted an unterminated heredoc, so the tail of such a file reads clean"
fi

# --- the corpus ---------------------------------------------------------------
git ls-files -- '*.c' '*.cc' '*.cpp' '*.h' '*.hh' '*.hpp' '*.inc' '*.h.in' '*.S' \
    '*.ld' '*.lds' '*.py' '*.sh' '*.cmake' '*.awk' '*.yml' \
    CMakeLists.txt '*/CMakeLists.txt' Kconfig '*/Kconfig' > "$TMP/all" \
    || fail "git ls-files failed"
require_nonempty "$TMP/all" "git ls-files matched no source file; every check below would pass vacuously"
N="$(wc -l < "$TMP/all" | tr -d ' ')"

: > "$TMP/findings"
: > "$TMP/refused"
while IFS= read -r f; do
    [ -f "$f" ] || fail "tracked file is missing from the worktree: $f"
    hd=0
    case "$f" in
        *.sh) hd=1 ;;
    esac
    if scan "$f" "$hd" >> "$TMP/findings" 2>> "$TMP/refused"; then
        :
    else
        rc=$?
        [ "$rc" -eq 2 ] || fail "awk exited $rc scanning $f"
    fi
done < "$TMP/all"

echo "== checked $N tracked source file(s) for a double hyphen used as punctuation =="

if [ -s "$TMP/refused" ]; then
    echo "FAIL: the scan could not read $(wc -l < "$TMP/refused" | tr -d ' ') file(s) to the end, so their verdict is UNKNOWN, not clean:" >&2
    sed 's/^/      /' "$TMP/refused" >&2
    exit 1
fi

if [ -s "$TMP/findings" ]; then
    cat "$TMP/findings" >&2
    echo "" >&2
    echo "per-file finding count:" >&2
    cut -d: -f1 "$TMP/findings" | sort | uniq -c | sort -rn >&2
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/findings" | tr -d ' ') line(s) spell a sentence break as a double hyphen." >&2
    echo "      Write a comma, a semicolon, a single ' - ', or two sentences. Do NOT reach" >&2
    echo "      for an em dash or an en dash: check_ascii.sh refuses the byte." >&2
    exit 1
fi

echo "PASS: no double hyphen used as punctuation across $N tracked source file(s)"
