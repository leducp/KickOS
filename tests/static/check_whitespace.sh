#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The whitespace rules of docs/reference/style.md, over every tracked file: no trailing
# whitespace, no CRLF, a final newline. Plus the two `git diff --check` classes that rule
# does not name and an audit found in a linker script, a space immediately before a tab in
# a line's indent and a blank line at end of file.
#
# Run from the repo root, no arguments, no build directory:
#   tests/static/check_whitespace.sh
#
#   corpus   EVERY tracked file, from `git ls-files`, with no extension filter, so the next
#            .ld, .S or .json is covered on the day it lands. NO FILE IS SKIPPED WHOLE:
#            allowed_records() below names the individual records a file may carry, and
#            every other class in that same file still reports.
#
#   floor    CORPUS_FLOOR, at about half what the tree tracks, applied to the tracked total
#            AND to the number of files actually read, because a gate handed nothing must go
#            red rather than clean.
#
#   tally    read + empty + NUL-holding must equal the tracked total, so a walk that ended
#            early is a red refusal and not a smaller headline figure.
#
#   control  five planted files, one per class, plus a clean one, all scanned by the same
#            detector before the corpus is read.
#
#   binary   a tracked file holding a NUL byte is SKIPPED by name and printed.
#            check_ascii.sh is what refuses it.
#
# NO FILE NAME REACHES A TOOL AS AN OPERAND. Every scan takes its file on stdin. `awk` reads
# an operand of the form `name=value` as a VARIABLE ASSIGNMENT and then falls through to
# stdin, which here is the file list the corpus loop is reading, so one tracked file named
# like an assignment consumed the rest of the list and every file below it went unread while
# the run printed PASS.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# NOT set -e: the point is to collect EVERY finding in one run, not to stop at the first.

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

# Sized at about HALF what the tree tracks, so an ordinary deletion still passes while an
# empty walk, a run outside a checkout and a corpus narrowed by an edit here all refuse. The
# tracked total is printed on every run and is not pinned here.
CORPUS_FLOOR=512

# The records a tracked file is allowed to carry, one per line, in the shape scan_file emits.
# Every listed record must still be PRESENT: a file that stopped carrying one fails here, so
# the set cannot go stale into a blanket skip. Every record a file carries that is not listed
# reports as an ordinary finding.
#
#   LICENSE
#       verbatim upstream CeCILL-C text. Lines 133 and 279 carry a trailing space; rewriting
#       them makes the file no longer the licence as published. Restore it from cecill.info,
#       never by hand.
allowed_records() { # <file>
    case "$1" in
        LICENSE)
            printf '%s\n' '133: trailing whitespace' '279: trailing whitespace'
            ;;
    esac
}

scratch_dir

# One record per finding, `<line>: <class>`, for every class a line can carry. The caller
# prefixes the file name, so a one-file invocation reports in the same shape as the sweep.
#
# A record is judged on the line with any trailing carriage return REMOVED, so a CRLF file
# reports the CR once and does not also report every line as trailing whitespace.
scan_lines() { # <file>
    awk '
        BEGIN { CR = sprintf("%c", 13) }
        {
            line = $0
            if (substr(line, length(line), 1) == CR) {
                print NR ": carriage return at end of line"
                line = substr(line, 1, length(line) - 1)
            }
            if (line ~ /[ \t]$/) { print NR ": trailing whitespace" }
            indent = line
            sub(/[^ \t].*$/, "", indent)
            if (indent ~ / \t/) { print NR ": space before tab in indent" }
            empty = (line == "")
            last = NR
        }
        END {
            if (last > 0 && empty) { print last ": blank line at end of file" }
        }' < "$1"
}

# The newline awk cannot see: it splits on records, so a file whose last byte is not a
# newline yields the same records as one whose last byte is.
scan_eof() { # <file>
    if [ "$(tail -c 1 < "$1" | wc -l | tr -d ' ')" -eq 0 ]; then
        printf '%s\n' '$: no newline at end of file'
    fi
}

scan_file() { # <file>
    scan_lines "$1"
    scan_eof "$1"
}

# Drops the records allowed_records() names for <file> from the record file <records>, and
# refuses when one of them is no longer there: an entry that stopped matching would hide
# whatever took its place.
apply_allowed() { # <file> <records>
    allowed_records "$1" > "$TMP/allow"
    while IFS= read -r _a; do
        grep -Fxq -e "$_a" "$2" \
            || fail "$1 no longer carries the record [$_a] that allowed_records() names, so
      that entry hides whatever replaced it. Re-measure the file and update the list."
    done < "$TMP/allow"
    # grep exits 1 when nothing survives the filter, which is the ordinary case here; only
    # an exit above that is an error, and taking it for "nothing left" would empty the record
    # set and report the file clean.
    grep -Fxv -f "$TMP/allow" "$2" > "$TMP/kept"
    _rc=$?
    [ "$_rc" -le 1 ] || fail "exit $_rc from grep while filtering the records of $1: its
      verdict is UNKNOWN, not clean"
    mv "$TMP/kept" "$2"
}

# --- the detector, before it is asked to report an absence --------------------
# One planted file per class and one clean file, each scanned by scan_file above. A class
# whose control does not fire is a class this run cannot report on.
mkdir "$TMP/ctl" || fail "cannot create $TMP/ctl"

printf 'a line ending in a space \n'                > "$TMP/ctl/trail"
printf ' %sindented after a space\n' "$TAB"      > "$TMP/ctl/sbt"
printf 'a line ending in a carriage return\r\n'     > "$TMP/ctl/crlf"
printf 'a line\n\n'                                 > "$TMP/ctl/blank"
printf 'a line with no newline after it'            > "$TMP/ctl/nonl"
printf 'a clean line\n%sa clean indent\n' "$TAB"   > "$TMP/ctl/clean"

control() { # <name> <expected-record>
    _got="$(scan_file "$TMP/ctl/$1")"
    if [ "$_got" != "$2" ]; then
        fail "the control for '$2' reported [$_got] instead: this detector cannot see that
      class, so the sweep below would report the tree clean of it"
    fi
}

control trail "1: trailing whitespace"
control sbt   "1: space before tab in indent"
control crlf  "1: carriage return at end of line"
control blank "2: blank line at end of file"
control nonl  "\$: no newline at end of file"

CLEAN_HITS="$(scan_file "$TMP/ctl/clean")"
[ -z "$CLEAN_HITS" ] \
    || fail "the detector reported [$CLEAN_HITS] on a file that carries none of these
      classes, so every file in the tree would report"

echo "== control: all five classes fire on a planted file, none on a clean one =="

# --- the filter, before it is asked to hide anything --------------------------
# The suppression is a detector too, so it gets the same treatment: it must pass an unlisted
# record through, and it must refuse a listed one that is no longer in the file.
allowed_records LICENSE > "$TMP/ctl/allow"
require_nonempty "$TMP/ctl/allow" "allowed_records() names nothing for LICENSE, so the two
      controls below would assert nothing about the filter"

{ cat "$TMP/ctl/allow"; printf '%s\n' '7: carriage return at end of line'; } > "$TMP/ctl/rec"
apply_allowed LICENSE "$TMP/ctl/rec"
[ "$(cat "$TMP/ctl/rec")" = "7: carriage return at end of line" ] \
    || fail "the filter reported [$(cat "$TMP/ctl/rec")] where only the unlisted record was
      expected, so it drops records it was never given"

sed -n '2,$p' "$TMP/ctl/allow" > "$TMP/ctl/rec"
if ( apply_allowed LICENSE "$TMP/ctl/rec" ) >/dev/null 2>&1; then
    fail "the filter accepted a file missing a record allowed_records() names, so a stale
      entry would silently hide the record that replaced it"
fi

echo "== control: the filter passes an unlisted record and refuses a listed one gone missing =="

# --- the corpus ---------------------------------------------------------------
git ls-files > "$TMP/all" || fail "git ls-files failed"
require_nonempty "$TMP/all" "git ls-files matched nothing; every check below would pass vacuously"

N_TRACKED="$(wc -l < "$TMP/all" | tr -d ' ')"
[ "$N_TRACKED" -ge "$CORPUS_FLOOR" ] \
    || fail "$N_TRACKED tracked file(s), beneath the floor of $CORPUS_FLOOR: this is not the
      KickOS worktree, or the corpus was narrowed. A corpus that size asserts nothing."

: > "$TMP/findings"
: > "$TMP/binary"
: > "$TMP/empty"
: > "$TMP/allowed"
N=0
while IFS= read -r f; do
    [ -f "$f" ] || fail "tracked file is missing from the worktree: $f"
    [ -r "$f" ] || fail "tracked file is unreadable, so its verdict is UNKNOWN, not clean: $f"
    # An empty file carries no line and no missing newline.
    if [ ! -s "$f" ]; then
        printf '%s\n' "$f" >> "$TMP/empty"
        continue
    fi
    # grep cannot carry a NUL in its pattern, so an unequal byte count is the test.
    _bytes="$(wc -c < "$f" | tr -d ' ')"
    _text="$(LC_ALL=C tr -d '\000' < "$f" | wc -c | tr -d ' ')"
    if [ "$_bytes" -ne "$_text" ]; then
        printf '%s\n' "$f" >> "$TMP/binary"
        continue
    fi
    N=$((N + 1))
    scan_file "$f" > "$TMP/rec"
    if [ -n "$(allowed_records "$f")" ]; then
        printf '%s\n' "$f" >> "$TMP/allowed"
        apply_allowed "$f" "$TMP/rec"
    fi
    while IFS= read -r r; do
        printf '%s:%s\n' "$f" "$r"
    done < "$TMP/rec" >> "$TMP/findings"
done < "$TMP/all"

N_EMPTY="$(wc -l < "$TMP/empty" | tr -d ' ')"
N_BINARY="$(wc -l < "$TMP/binary" | tr -d ' ')"
N_SEEN=$((N + N_EMPTY + N_BINARY))

echo "== checked $N of $N_TRACKED tracked file(s), every line of each =="

[ "$N" -ge "$CORPUS_FLOOR" ] \
    || fail "$N file(s) read, beneath the floor of $CORPUS_FLOOR: the walk did not reach the
      tree, and a class this run never looked for is unread rather than absent."

[ "$N_SEEN" -eq "$N_TRACKED" ] \
    || fail "$N_SEEN of $N_TRACKED tracked file(s) reached a verdict ($N read, $N_EMPTY empty,
      $N_BINARY holding a NUL): the walk ended before the list did, so the remainder is
      unread and this run asserts nothing about it."

if [ -s "$TMP/allowed" ]; then
    echo "== scanned, with the records allowed_records() names dropped and asserted present =="
    sed 's/^/   /' "$TMP/allowed"
fi
if [ -s "$TMP/empty" ]; then
    echo "== not scanned, an empty file carries no line and no missing final newline =="
    sed 's/^/   /' "$TMP/empty"
fi
if [ -s "$TMP/binary" ]; then
    echo "== not scanned, a NUL byte makes it not text (check_ascii.sh refuses these) =="
    sed 's/^/   /' "$TMP/binary"
fi

if [ -s "$TMP/findings" ]; then
    cat "$TMP/findings" >&2
    echo "" >&2
    echo "per-class finding count:" >&2
    sed 's/^.*: //' "$TMP/findings" | sort | uniq -c | sort -rn >&2
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/findings" | tr -d ' ') whitespace finding(s)." >&2
    echo "      docs/reference/style.md: no trailing whitespace, no CRLF, a final newline." >&2
    echo "      A space before a tab renders at the reader's tab stop and not at yours, and" >&2
    echo "      a blank line before end of file is what \`git diff --check\` calls one." >&2
    echo "      A \$ in place of a line number is the end of the file rather than a line." >&2
    exit 1
fi

echo "PASS: no tracked file carries trailing whitespace, a CRLF, a space before a tab in its"
echo "      indent, a blank line at end of file, or a missing final newline"
