#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The byte rules and the whitespace rules of docs/reference/style.md, over every tracked file:
#
#   the byte    no byte above 0x7F, and no NUL.
#   the line    no trailing whitespace, no CRLF, a final newline, no space before a tab in a
#               line's indent, and no blank line at end of file.
#
# Run from the repo root, no arguments, no build directory:
#   tests/static/check_ascii.sh
#
# The corpus is the worktree's tracked files but the vendored ones (KOS_VENDORED_DIRS, gate.sh),
# read as one `git diff` against the empty tree, so every file is read with no extension filter;
# a NUL is searched for over each whole file beside it.
#
# One exemption, LICENSE: verbatim upstream CeCILL-C text, which carries a-grave (C3 A0) twice
# and a trailing space on lines 133 and 279. Restore it from cecill.info, never by hand.

set -u
. "$(dirname "$0")/../lib/gate.sh"

CORPUS_FLOOR=512
scratch_dir

# scan <repo dir>: one finding per line, `<file>:<line>: <class>`, then `files <n>` last.
scan() {
    _repo="$1"
    _empty="$(git -C "$_repo" hash-object -t tree /dev/null)" || fail "git cannot name the empty tree in $_repo"
    set --
    for _d in $KOS_VENDORED_DIRS; do
        set -- "$@" ":(exclude)$_d"
    done
    # Over the whole file: git's own binary test reads the first 8000 bytes alone.
    ( cd "$_repo" && git ls-files -z -- . "$@" \
        | LC_ALL=C xargs -0 grep -l -a -P '\x00' /dev/null 2> "$TMP/nulerr" ) \
        | sed 's/$/:0: holds a NUL byte, so it is not text/'
    [ -s "$TMP/nulerr" ] && fail "the NUL search failed, so no file's verdict is known: $(head -n1 "$TMP/nulerr")"
    git -C "$_repo" -c core.quotepath=off diff --no-color --no-ext-diff --no-textconv --no-renames \
        -U0 "$_empty" . "$@" | LC_ALL=C awk '
        function close_file() {
            if (file != "" && last_blank && !nonl) { print file ":$: blank line at end of file" }
        }
        /^diff --git / { close_file(); file = ""; files++; next }
        /^\+\+\+ b\// { file = substr($0, 7); last_blank = 0; nonl = 0; next }
        /^@@ / { split($3, a, /[+,]/); n = a[2] - 1; next }
        /^\\ No newline at end of file/ { if (file != "") { print file ":" n ": no final newline" } nonl = 1; next }
        /^\+/ {
            n++
            line = substr($0, 2)
            lic = (file == "LICENSE")
            if (!lic && line ~ /[\200-\377]/) { print file ":" n ": byte above 0x7F" }
            if (line ~ /\r$/) { print file ":" n ": carriage return at end of line"; sub(/\r$/, "", line) }
            else if (line ~ /[ \t]$/ && !(lic && (n == 133 || n == 279))) { print file ":" n ": trailing whitespace" }
            if (match(line, /^[ \t]*/) && substr(line, 1, RLENGTH) ~ / \t/) { print file ":" n ": space before a tab in the indent" }
            last_blank = (line == "")
        }
        END { close_file(); print "files " files + 0 }'
}

# --- controls: one planted file per class in a scratch repository, scanned by scan() ----------
R="$TMP/ctl"
mkdir -p "$R" && git -C "$R" init -q || fail "cannot create the control repository"
printf 'clean\n'                         > "$R/clean.txt"
printf 'caf\351\n'                       > "$R/high.txt"
printf 'a\000b\n'                        > "$R/nul.bin"
{ head -c 9000 /dev/zero | tr '\000' a; printf '\000\n'; } > "$R/late.txt"
printf 'a \n'                            > "$R/trail.txt"
printf 'a\r\n'                           > "$R/crlf.txt"
printf 'a'                               > "$R/nonl.txt"
printf ' \tx\nx \tok\n'                  > "$R/indent.txt"
printf 'a\n\n'                           > "$R/blank.txt"
git -C "$R" add -A || fail "cannot stage the control files"
scan "$R" > "$TMP/ctl.out" || fail "the scan failed over the control repository"
cat > "$TMP/ctl.want" <<'EOF'
blank.txt:$: blank line at end of file
crlf.txt:1: carriage return at end of line
high.txt:1: byte above 0x7F
indent.txt:1: space before a tab in the indent
late.txt:0: holds a NUL byte, so it is not text
nonl.txt:1: no final newline
nul.bin:0: holds a NUL byte, so it is not text
trail.txt:1: trailing whitespace
files 9
EOF
sort "$TMP/ctl.out" > "$TMP/ctl.got"
sort "$TMP/ctl.want" > "$TMP/ctl.sorted"
cmp -s "$TMP/ctl.got" "$TMP/ctl.sorted" || {
    diff "$TMP/ctl.sorted" "$TMP/ctl.got" >&2
    fail "the scan did not report exactly one finding per planted class and none in the clean file"
}
echo "== control: one finding per planted class, at its line, and none in the clean file =="

# --- the corpus -------------------------------------------------------------------------------
require_repo_root
scan . > "$TMP/out" || fail "the scan failed over the tree"
N="$(sed -n 's/^files //p' "$TMP/out")"
[ "${N:-0}" -ge "$CORPUS_FLOOR" ] || fail "${N:-0} tracked file(s) read, beneath the floor of $CORPUS_FLOOR"
grep -v '^files ' "$TMP/out" > "$TMP/findings"
if [ -s "$TMP/findings" ]; then
    cat -v "$TMP/findings" >&2
    echo "FAIL: $(wc -l < "$TMP/findings" | tr -d ' ') finding(s). Spell it in ASCII (a comma or a single -" >&2
    echo "      for an em dash, -> for an arrow, straight quotes), drop the trailing whitespace and" >&2
    echo "      the CR, and end the file with exactly one newline." >&2
    exit 1
fi
echo "PASS: $N tracked file(s): no byte above 0x7F, no NUL, no trailing whitespace, CRLF, space"
echo "      before a tab in the indent, blank line at end of file or missing final newline"
