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
#   corpus     corpus_shell and corpus_floor (tests/lib/gate.sh).
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

set -u
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root

scratch_dir

CORPUS_FLOOR=120

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
    sed "$STRIP" "$1" | grep -nE "$ASSIGN_ERE|$FOR_ERE|$READ_ERE" | sed "s|^|$1:|"
}

# One write per form, then reads, ordinary variables, longer names and a quoted violation in a
# comment, none of which may report. The writes are assembled by printf because this file is in
# its own corpus.
printf '%s="a table"\nexport %s=1\nfor %s in a b; do :; done\nprintf x | while IFS= read -r %s; do :; done\n' \
    GROUPS BASH_MINE UID PWD > "$TMP/pos.sh"
printf '%s\n' 'while IFS= read -r line; do :; done' 'OPTIND=1; OPTARG=x; REPLY=y' \
    'echo "$PWD $UID ${PIPESTATUS[0]} $RANDOM"' 'NOTGROUPS=1; MY_UID=2' \
    'x=1  # GROUPS=the table was the bug' > "$TMP/neg.sh"
[ "$(scan "$TMP/pos.sh" | wc -l | tr -d ' ')" -eq 4 ] \
    || fail "the scan missed a planted write: $(scan "$TMP/pos.sh")"
[ -z "$(scan "$TMP/neg.sh")" ] \
    || fail "the scan reported a read, an ordinary name or a comment: $(scan "$TMP/neg.sh")"

corpus_shell "$TMP/corpus"
corpus_floor "$TMP/corpus" "$CORPUS_FLOOR" "shell script(s)"
: > "$TMP/findings"
while IFS= read -r f; do
    scan "$f" >> "$TMP/findings"
done < "$TMP/corpus"

if [ -s "$TMP/findings" ]; then
    sed 's/^/      /' "$TMP/findings" >&2
    fail "$(wc -l < "$TMP/findings" | tr -d ' ') line(s) write an identifier a shell already owns.
      Under bash the write is refused or overwritten and the data is silently gone. Rename it
      into the project's own namespace (KOS_...)."
fi
echo "PASS: no tracked shell script writes a shell-owned identifier across $N script(s)"
