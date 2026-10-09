#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Require chaincheck's verdict and every arm's line by name, for the half the image carries.
# Under QEMU or the sim it boots the image; with KOS_CAPTURE it reads a silicon capture:
#
#   check_chaincheck.sh <chaincheck image> <half>
#   KOS_CAPTURE=<log> check_chaincheck.sh <board-build> <kickos-source> <cmake> <half>
#
# <half> is `int`, `float` or `full`, one image each. The arm list is read off the app's
# source (user/apps/common/chaincheck/main.c): an arm is a ck_* call opening its line with its
# name literal, and the half it belongs to is the `#if CHAINCHECK_INT`, `_FLOAT` or `_FULL` it
# sits under (`#if CHAINCHECK_FP`, which the two float halves share, names none). A
# PASS is printed by whatever arms ran, so an arm the build dropped leaves the verdict green;
# naming each one here is what keeps the coverage the claim makes.
set -u
. "$(dirname "$0")/../lib/gate.sh"

usage="usage: check_chaincheck.sh <image> <half>"
if judging_capture; then
    src="${2:?$usage}"
    shift 3
else
    elf="${1:?$usage}"
    src="$(dirname "$0")/../.."
    shift
fi
[ "$#" -eq 1 ] || fail "$usage (name the half the image carries)"
named="$1"
case "$named" in
    int | float | full) ;;
    *) fail "unknown half '$named': $usage" ;;
esac

app="$src/user/apps/common/chaincheck/main.c"
[ -r "$app" ] || fail "cannot read $app"

scratch_dir
# <half>TAB<name> per arm. An arm under a conditional other than one half's would make the list
# depend on a posture this reader does not know, so it is refused, as is a name it cannot read.
awk -v tab="$TAB" '
    function refuse(why) { printf "%s:%d: %s\n", FILENAME, FNR, why > "/dev/stderr"; bad = 1 }
    /^[ \t]*#[ \t]*if/ {
        depth++
        half_at[depth] = ""
        if ($0 ~ /^[ \t]*#[ \t]*if[ \t]+CHAINCHECK_FP[ \t]*$/) {
            half_at[depth] = "-"
        } else if ($0 ~ /^[ \t]*#[ \t]*if[ \t]+CHAINCHECK_(INT|FLOAT|FULL)[ \t]*$/) {
            h = $0
            sub(/^.*CHAINCHECK_/, "", h)
            sub(/[ \t]*$/, "", h)
            half_at[depth] = tolower(h)
        }
        next
    }
    /^[ \t]*#[ \t]*(else|elif)/ {
        if (half_at[depth] != "") { refuse("a half takes no #else or #elif") }
        next
    }
    /^[ \t]*#[ \t]*endif/ { depth--; next }
    /^[ \t]*ck_[a-z0-9_]+\(/ {
        if ($0 !~ /^[ \t]*ck_[a-z0-9_]+\("[^"]+"/) {
            refuse("an arm whose name is not a literal")
            next
        }
        half = ""
        for (i = 1; i <= depth; i++) {
            if (half_at[i] == "") { refuse("an arm under a conditional"); next }
            if (half_at[i] == "-") { continue }
            if (half != "") { refuse("an arm under two halves"); next }
            half = half_at[i]
        }
        if (half == "") { refuse("an arm under no half"); next }
        name = $0
        sub(/^[ \t]*ck_[a-z0-9_]+\("/, "", name)
        sub(/".*/, "", name)
        print half tab name
    }
    END { exit bad }' "$app" > "$TMP/arms" \
    || cfail source "$app holds an arm this judge cannot place"

: > "$TMP/want"
awk -F "$TAB" -v h="$named" '$1 == h { print $2 }' "$TMP/arms" >> "$TMP/want"

want_n=$(wc -l < "$TMP/want")
[ "$want_n" -gt 0 ] || fail "$app holds no arm for the half $named, so a PASS would cover nothing"
dups=$(cut -f 2 "$TMP/arms" | sort | uniq -d)
if [ -n "$dups" ]; then
    cfail source "$app names more than one arm '$dups', so one line cannot stand for each"
fi

if judging_capture; then
    judge_capture chaincheck
else
    poll_image "$elf" '\[chaincheck\] (PASS|FAIL) \('
fi
has_e '\[chaincheck\] (PASS|FAIL) \(' || cfail verdict "chaincheck reached no verdict"
assert_no_panic "chaincheck panicked"

if has '\[chaincheck\] FAIL'; then
    printf '%s\n' "$OUT" | grep '\[chaincheck\] FAIL' >&2
    cfail error "chaincheck reported a wrong answer from the chain"
fi

printf '%s\n' "$OUT" | sed -n 's/^\[chaincheck\] ok - //p' | sort > "$TMP/got"
sort "$TMP/want" > "$TMP/want.sorted"
missing=$(comm -23 "$TMP/want.sorted" "$TMP/got")
if [ -n "$missing" ]; then
    printf 'no line for: %s\n' "$missing" >&2
    cfail arm "chaincheck printed no line for an arm $app names, so PASS covers less than it claims"
fi
got_n=$(wc -l < "$TMP/got")
if [ "$got_n" -ne "$want_n" ]; then
    cfail arm "chaincheck printed $got_n arm lines where $app names $want_n for the half $named"
fi
has_f "[chaincheck] PASS ($want_n arms)" \
    || cfail verdict "chaincheck's verdict does not claim the $want_n arms $app names"

echo "PASS: chaincheck's $want_n arms answered as the standard says (half: $named)"
exit 0
