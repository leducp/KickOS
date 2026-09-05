#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# No gawk-only extension in an awk program this tree runs. `awk` is gawk on a Debian
# developer box and is mawk or busybox awk on a minimal image, and the two disagree by
# REFUSING: a call to an extension is a program mawk exits 2 on and busybox awk exits 1 on,
# before a single record. A gate script then reads an empty result, and what it reports is
# whatever absence it was testing for rather than the awk.
#
# Run from the repo root, no arguments, no build directory:
#   tests/static/check_awk_portable.sh
#
#   corpus     tracked *.sh and *.awk under tools/ and tests/, which is every awk program this
#              tree runs. A file outside those two roots runs no awk.
#
#   named      the spellings listed in EXT below, one by one. NOT a wildcard over gawk's
#              function list: `and`, `or`, `xor` and `compl` are gawk extensions whose names
#              are ordinary English, so a pattern for them would fire on prose.
#
#   comments   NOT stripped, deliberately. A shell script's `#` is also `${var#prefix}` and
#              sits inside single quotes, so an accurate stripper here would be a shell parser
#              and an inaccurate one goes VACUOUS rather than loud. A COMMENT naming one of
#              these spellings is therefore a finding too: reword it.
set -u
. "$(dirname "$0")/../lib/gate.sh"

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

scratch_dir

# Every spelling, as one ERE, matched as a CALL: the name followed by `(`, with a
# non-identifier character or a line start before it, so `mkbool` does not hit `kos_mkbool`.
EXT='(^|[^A-Za-z0-9_])(strtonum|gensub|patsplit|asorti|asort|systime|mktime|strftime|typeof|mkbool)[[:space:]]*\('

# --- the instrument, before the corpus: both directions, or an absence proves nothing -------
#
# THE PLANTED NAMES ARE SPELLED IN HALVES: this file is in its own corpus like any other, so a
# control holding the literal call would be a finding in itself. Concatenation puts the whole
# name in the generated awk program and never in this source.
_e1="strto""num"
_e2="gen""sub"
printf 'a { print %s($1) }\n' "$_e1" > "$TMP/pos"
printf 'a { print %s(/x/,"y",1) }\n' "$_e2" >> "$TMP/pos"
# The negative control pins the plain spellings OUT of the pattern; its first field is the
# banned name as a bare IDENTIFIER, since what is refused is a CALL and not a name.
printf 'a { n = %sber; print index($1, "x"), length($1), substr($1, 2) }\n' "$_e1" > "$TMP/neg"
printf 'a { split($0, f, ":"); printf "%%s", tolower(f[1]) }\n' >> "$TMP/neg"

_pos="$(grep -cE "$EXT" "$TMP/pos")" || _pos=0
[ "$_pos" -eq 2 ] \
    || fail "the pattern found $_pos of 2 planted extension calls, so it cannot find one in
      the corpus either"
_neg="$(grep -cE "$EXT" "$TMP/neg")" || _neg=0
[ "$_neg" -eq 0 ] \
    || fail "the pattern reports $_neg finding(s) in a control holding POSIX awk only, so
      every count below it is unattributable"

# --- the corpus -----------------------------------------------------------------------------
git ls-files -- 'tools/*.sh' 'tools/*.awk' 'tests/*.sh' 'tests/*.awk' > "$TMP/list" \
    || fail "git ls-files failed; the corpus is UNKNOWN and not empty"
require_nonempty "$TMP/list" \
    "the corpus is empty: no tracked *.sh or *.awk under tools/ or tests/, so this gate would
      pass on any tree at all. An untracked file is invisible here (git add first)."
_n="$(wc -l < "$TMP/list")"
echo "check_awk_portable: $_n file(s) in the corpus"

: > "$TMP/findings"
while IFS= read -r f; do
    [ -f "$f" ] || fail "file in the corpus is missing from the worktree: $f"
    grep -nE "$EXT" "$f" | sed -e "s|^|$f:|" >> "$TMP/findings"
done < "$TMP/list"

if [ -s "$TMP/findings" ]; then
    cat "$TMP/findings" >&2
    fail "$(wc -l < "$TMP/findings") gawk-only extension call(s) above. A non-gawk awk refuses
      the whole program, so the script reports the absence it was testing for instead of the
      awk. Rewrite in POSIX awk; a hex field can be compared as a zero-padded string
      (tools/amp/merge-partition.sh), and a size is nonzero iff its digits are not all zero.
      A COMMENT naming one of these spellings is a finding too: this gate strips none, on
      purpose (see the header), so reword it."
fi

echo "check_awk_portable: OK, no gawk-only extension in $_n file(s)"
