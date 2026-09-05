#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# No tracked shell script writes an identifier a shell already owns. `/bin/sh` is dash on the
# CI images and bash on plenty of developer boxes, and a name bash maintains itself does not
# hold what a script puts in it. `GROUPS="<a data table>"` is the shape: under bash the
# assignment does not take, the table expands to the caller's group ids, every per-row floor
# derived from it goes empty, and the script still exits with the code it exits with under
# dash. `dash -n` passes it, `sh -n` passes it, and only a run under BOTH shells shows it.
#
# Run from the repo root, no arguments, no build directory:
#   tests/static/check_shell_special_names.sh
#
#   corpus     tracked *.sh, plus any tracked file whose first line is a sh or bash shebang,
#              out of `git ls-files` so an untracked scratch script is neither read nor
#              counted. A sourced fragment with no shebang is in the corpus when it is named
#              *.sh, which is how tests/lib/gate.sh is covered.
#
#   floor      CORPUS_FLOOR, at about half what the tree tracks, on the corpus AND on the
#              number of files the scan actually read: a `git ls-files` that matched almost
#              nothing, a walk that stopped early and a corpus narrowed by an edit here would
#              all read as clean. A tracked file MISSING from the worktree is refused rather
#              than skipped, a silent skip being how a corpus shrinks without a count moving.
#
#   the names  NAMES below. Two kinds, and the fix is the same for both, so one list: the ones
#              bash refuses to let a script write (GROUPS, BASH_*, EUID, UID, PPID, SHELLOPTS,
#              BASHOPTS, FUNCNAME, DIRSTACK, PIPESTATUS, SRANDOM, EPOCHSECONDS,
#              EPOCHREALTIME), and the ones it lets a script write and then overwrites on its
#              own (RANDOM, SECONDS, LINENO, HISTCMD, PWD, OLDPWD). A deliberate reseed of
#              RANDOM or SECONDS wants an exemption; nothing in this tree does it.
#
#   NOT here   IFS, OPTARG, OPTIND and REPLY. Those are ordinary shell variables and writing
#              them is the normal idiom (`while IFS= read -r`, `getopts`).
#
#   the forms  a write, never a read: `NAME=`, `NAME+=`, the same behind export / local /
#              readonly / declare / typeset, `for NAME in`, and `read [-opts] [names] NAME`.
#              `"$PWD"` and `${PIPESTATUS[0]}` are reads and pass.
#
#   comments   a line whose first non-blank character is `#` is blanked, and a ` #` opens a
#              comment that is erased to end of line. That buys one false NEGATIVE, an
#              assignment sitting after a `#` inside a string on the same line, and it is
#              what lets this gate's own header name GROUPS without reporting itself.
#
# The self-test below plants one violation per NAME per form: a name in the list that no ERE
# reaches would otherwise be listed and unenforced, which reads exactly like a clean corpus.

set -u
. "$(dirname "$0")/../lib/gate.sh"

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

scratch_dir

# Sized at about HALF what the corpus holds, so an ordinary deletion still passes while an
# empty walk, a run outside a checkout and a corpus narrowed by an edit here all refuse. The
# corpus count is printed on every run and is not pinned here.
CORPUS_FLOOR=48

# One name per line. BASH_ and COMP_ are prefixes, matched below as such.
NAMES="GROUPS
EUID
UID
PPID
SHELLOPTS
BASHOPTS
FUNCNAME
DIRSTACK
PIPESTATUS
SRANDOM
EPOCHSECONDS
EPOCHREALTIME
RANDOM
SECONDS
LINENO
HISTCMD
PWD
OLDPWD"

# The alternation, built from the list so the list is the only place a name is written.
ALT="$(printf '%s\n' "$NAMES" | tr '\n' '|' | sed 's/|$//')"
ALT="($ALT|BASH[A-Z_]*|COMP_[A-Z_]+)"

DECL='(export|local|readonly|declare|typeset)[[:blank:]]+'
POS='(^|[[:blank:]]|[;&|({])[[:blank:]]*'
ASSIGN_ERE="$POS($DECL)*$ALT\\+?="
# A name is followed by anything that is not an identifier character, or by end of line: a
# `read -r GROUPS; do` ends the name with a semicolon, not a blank.
END='([^A-Za-z0-9_]|$)'
FOR_ERE="${POS}for[[:blank:]]+$ALT$END"
READ_ERE="${POS}read([[:blank:]]+-[A-Za-z]+)*([[:blank:]]+[A-Za-z_][A-Za-z0-9_]*)*[[:blank:]]+$ALT$END"

# A comment line is BLANKED and not deleted, so grep -n still reports the file's own line
# number: a deleted line shifts every number below it and the finding then names innocent code.
STRIP='s/^[[:blank:]]*#.*$//; s/[[:blank:]]#.*$//'

scan() { # <file>
    LC_ALL=C sed "$STRIP" "$1" \
        | LC_ALL=C grep -nE "$ASSIGN_ERE|$FOR_ERE|$READ_ERE" \
        | sed "s|^|$1:|"
}

# --- self-test: every listed name, every form ---------------------------------
planted=0
for n in $(printf '%s\n' "$NAMES") BASH_MINE COMP_MINE; do
    for form in 1 2 3 4; do
        case "$form" in
            1) printf '%s=value\n' "$n" > "$TMP/one.sh" ;;
            2) printf 'export %s=value\n' "$n" > "$TMP/one.sh" ;;
            3) printf 'for %s in a b; do :; done\n' "$n" > "$TMP/one.sh" ;;
            4) printf 'printf x | while IFS= read -r %s; do :; done\n' "$n" > "$TMP/one.sh" ;;
        esac
        got="$(scan "$TMP/one.sh" | wc -l | tr -d ' ')"
        if [ "$got" -ne 1 ]; then
            fail "the scan found $got finding(s) in a planted write to $n (form $form); that
      name is listed and unenforced, which reads exactly like a clean corpus"
        fi
        planted=$((planted + 1))
    done
done
[ "$planted" -eq 80 ] || fail "$planted positive control(s) ran, expected 80"

# The negatives, one per line and each scanned ALONE: a whole-file zero cannot tell "every
# clause is right" from "one clause swallowed the file".
{
    printf 'while IFS= read -r line; do :; done\n'
    printf 'IFS="$TAB"\n'
    printf 'OPTIND=1; OPTARG=x; REPLY=y\n'
    printf 'echo "$PWD $UID ${PIPESTATUS[0]} $RANDOM"\n'
    printf 'RC=${PIPESTATUS[0]}\n'
    printf 'NOTGROUPS=1\n'
    printf 'MY_UID=2\n'
    printf 'KOS_GROUP_TABLE="a table"\n'
    printf 'KOS_GROUP_ROWS=5\n'
    printf 'for f in a b; do :; done\n'
} > "$TMP/neg.sh"
i=0
while IFS= read -r line; do
    i=$((i + 1))
    printf '%s\n' "$line" > "$TMP/one.sh"
    got="$(scan "$TMP/one.sh" | wc -l | tr -d ' ')"
    [ "$got" -eq 0 ] || fail "negative control $i reports, so this gate would cry wolf: $line"
done < "$TMP/neg.sh"
[ "$i" -eq 10 ] || fail "$i negative control(s) ran, expected 10"

# The comment erase is load-bearing and has to be a near miss: with it disabled, a line that
# NAMES a violation in prose must newly report. A comment carrying no violation must stay
# silent under either, which is what separates the erase from a blanket skip.
printf 'x=1  # GROUPS=the table was the bug\n' > "$TMP/cmt.sh"
[ "$(scan "$TMP/cmt.sh" | wc -l | tr -d ' ')" -eq 0 ] \
    || fail "a violation quoted inside a comment reports; this gate's own header would fail it"
raw="$(LC_ALL=C grep -cE "$ASSIGN_ERE" "$TMP/cmt.sh" || true)"
[ "$raw" -eq 1 ] \
    || fail "with the comment erase disabled the quoted violation still does not report, so
      the erase is not a near miss and proves nothing about what it hides"

# --- the corpus ---------------------------------------------------------------
git ls-files > "$TMP/tracked" || fail "git ls-files failed"
require_nonempty "$TMP/tracked" "git ls-files matched nothing; the scan would pass vacuously"

: > "$TMP/corpus"
while IFS= read -r f; do
    [ -f "$f" ] || fail "tracked file is missing from the worktree: $f. A skipped file leaves
      the corpus one shorter with nothing saying so."
    [ -r "$f" ] || fail "tracked file is unreadable, so its verdict is UNKNOWN, not clean: $f"
    case "$f" in
        *.sh)
            printf '%s\n' "$f" >> "$TMP/corpus"
            continue
            ;;
    esac
    if head -n1 "$f" | LC_ALL=C grep -qE '^#!.*(/bin/sh|env[[:blank:]]+sh|/bin/bash|env[[:blank:]]+bash)'; then
        printf '%s\n' "$f" >> "$TMP/corpus"
    fi
done < "$TMP/tracked"
require_nonempty "$TMP/corpus" "no tracked shell script matched; the scan would pass vacuously"
N="$(wc -l < "$TMP/corpus" | tr -d ' ')"
[ "$N" -ge "$CORPUS_FLOOR" ] \
    || fail "$N shell script(s) in the corpus, beneath the floor of $CORPUS_FLOOR: this is not
      the tree, so a clean result below would be a corpus that shrank and not a tree that is
      clean."

# This script is itself a corpus member, so its presence is asserted: a corpus built from the
# wrong path would otherwise read one file fewer and still pass.
grep -Fxq "tests/static/check_shell_special_names.sh" "$TMP/corpus" \
    || fail "the corpus does not hold tests/static/check_shell_special_names.sh, so it was built
      from the wrong path and every finding below would be missing rather than absent"

: > "$TMP/findings"
N_READ=0
while IFS= read -r f; do
    scan "$f" >> "$TMP/findings"
    N_READ=$((N_READ + 1))
done < "$TMP/corpus"

# The corpus is what the walk BUILT; this is what the scan READ. A loop that ended early leaves
# the two apart, and only the first of them reaches the headline.
[ "$N_READ" -eq "$N" ] \
    || fail "the scan read $N_READ of $N script(s) in the corpus, so it stopped early and the
      rest are unreported rather than clean"
[ "$N_READ" -ge "$CORPUS_FLOOR" ] \
    || fail "$N_READ script(s) read, beneath the floor of $CORPUS_FLOOR"

echo "== checked $N_READ tracked shell script(s) for a write to a shell-owned identifier =="

if [ -s "$TMP/findings" ]; then
    sed 's/^/      /' "$TMP/findings" >&2
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/findings" | tr -d ' ') line(s) write an identifier a shell" >&2
    echo "      already owns. Under bash the write is refused or overwritten, the data is" >&2
    echo "      silently gone, and the script keeps its exit code. Rename it into the" >&2
    echo "      project's own namespace (KOS_...) and assert whatever count depends on it." >&2
    exit 1
fi

echo "PASS: no tracked shell script writes a shell-owned identifier across $N_READ script(s)"
