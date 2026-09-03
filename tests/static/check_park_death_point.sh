#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The park class is closed: no thread parks at a site nobody has declared.
#
# Run from the repo root, no arguments: tests/static/check_park_death_point.sh
#
# THE PROPERTY IT GUARDS, which is not the one it checks. syscall_dispatch reads cancel_kind at
# the syscall boundary, outside the kernel lock. A cancel raised behind that read aborts no
# park, the target not being BLOCKED yet, so a thread parking after it is owed a wake by
# nobody. Every parking caller therefore re-asks under the lock through park_cancel_pending,
# in its own prologue ahead of its first side effect.
#
# THIS GATE CHECKS THE ENUMERATION AND NOT THE ASK, and the distinction is why it cannot be
# tightened into the rule. The ask is not local to the park: the funnel sites deliberately do
# not ask, their callers do, so a check keyed on the ThreadState::BLOCKED write cannot express
# it. Expressing it means walking each funnel's callers, and tests/static/fn_body.awk extracts
# column-0 definitions only, its own comment naming that test as what keeps it sound against
# multiline calls, while these are namespace-scope and indented. Relaxing that helper to reach
# them would weaken check_death_stack_seating.sh, which rests on it.
#
# A count cannot say a park is correct. It can say nobody added one without being asked to
# think about it, which is the failure this exists for.
#
# TWO CLAIMS. 1: park_cancel_pending exists, so the rule the records file describes is not a
# name nothing defines. It fires when the mechanism is REMOVED and deliberately not when it is
# merely renamed: the predicate is an inline in kernel/include/kickos/sync.h, so this corpus
# carries its one and only body and a rename keeps every count matching. 2: the
# ThreadState::BLOCKED writes in kernel/ are
# exactly the declared ones, by file and by count, and any other is named.
#
# Comments and literals are blanked before anything is read, so no claim can be met by prose.

set -u
# Findings accumulate over every site, so one run names all of them.
set -f
. "$(dirname "$0")/../lib/gate.sh"

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

export LC_ALL=C
scratch_dir

STRIP="$(dirname "$0")/../lib/strip_comments.awk"
SITES="$(dirname "$0")/park_death_sites.txt"
[ -r "$STRIP" ] || fail "tests/lib/strip_comments.awk is unreadable; nothing below can tell code from prose"
[ -r "$SITES" ] || fail "tests/static/park_death_sites.txt is unreadable; the declared set is UNKNOWN"

# An ASSIGNMENT of the parked state, never a comparison: every reader tests it with == or !=.
WRITE='[^=!<>]=[[:space:]]*ThreadState::BLOCKED'
ASK='park_cancel_pending'

git ls-files kernel > "$TMP/all" || fail "git ls-files failed; the corpus is UNKNOWN"
grep -E '\.(cc|h)$' "$TMP/all" > "$TMP/corpus"
NFILES=$(wc -l < "$TMP/corpus")
[ "$NFILES" -gt 0 ] || fail "no tracked C/C++ file under kernel/; an empty corpus passes forever"

: > "$TMP/stripcat"
: > "$TMP/writes"
for f in $(cat "$TMP/corpus"); do
    if ! awk -f "$STRIP" "$f" > "$TMP/stripped" 2> "$TMP/striperr"; then
        sed 's/^/      /' "$TMP/striperr" >&2
        fail "$f: comments and literals could not be blanked, so its verdict is UNKNOWN"
    fi
    cat "$TMP/stripped" >> "$TMP/stripcat"
    grep -nE "$WRITE" "$TMP/stripped" | sed "s|^|$f:|" | cut -d: -f1,2 >> "$TMP/writes"
done

# Claim 1. Without it a rename could empty the rule while every count still matched.
# The trailing paren is load-bearing: without it a rename that merely EXTENDS the name still
# matches, and this claim goes quiet exactly when it should fire.
grep -qE "bool[[:space:]]+$ASK[[:space:]]*\\(" "$TMP/stripcat" \
    || fail "$ASK is defined nowhere in kernel/; the rule this class exists for has no code"

# The declared set, as one count per file.
awk '$1 == "site" { n[$2]++ } END { for (f in n) { print f, n[f] } }' "$SITES" \
    | sort > "$TMP/declared"
NDECL=$(awk '$1 == "site"' "$SITES" | wc -l)
[ "$NDECL" -gt 0 ] || fail "$SITES declares no site; an empty declaration passes forever"

cut -d: -f1 "$TMP/writes" | sort | uniq -c | awk '{ print $2, $1 }' | sort > "$TMP/found"
NW=$(wc -l < "$TMP/writes")
[ "$NW" -gt 0 ] || fail "no ThreadState::BLOCKED write found at all; the pattern has gone stale"

if ! cmp -s "$TMP/declared" "$TMP/found"; then
    echo "FAIL: the set of parking sites is not the declared one." >&2
    echo "      declared (file, count):" >&2
    sed 's/^/        /' "$TMP/declared" >&2
    echo "      found in the tree:" >&2
    sed 's/^/        /' "$TMP/found" >&2
    echo "" >&2
    echo "      Every ThreadState::BLOCKED write parks the calling thread, and every park owes" >&2
    echo "      the death point: a cancel raised behind syscall_dispatch's entry read aborts no" >&2
    echo "      park, so a thread parking after it is owed a wake by nobody. Ask" >&2
    echo "      $ASK in the parking caller's own under-lock prologue, ahead of its" >&2
    echo "      first side effect, then declare the site in tests/static/park_death_sites.txt." >&2
    echo "      The lines this gate found:" >&2
    sed 's/^/        /' "$TMP/writes" >&2
    exit 1
fi

echo "PASS: $NDECL declared parking site(s) over $NFILES tracked kernel file(s) account for all"
echo "      $NW ThreadState::BLOCKED write(s), and $ASK is defined"
