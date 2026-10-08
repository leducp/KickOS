#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Every park asks park_cancel_pending under the lock first: a cancel raised behind
# syscall_dispatch's entry read aborts no park, so a thread parking after it is owed a wake by
# nobody. The type holds that: Thread::state takes no assignment, to<BLOCKED>() fails its
# static_assert, ThreadStateCell::block is private to park_queueless, and park_queueless is
# reached only through a ParkToken, which only park_cancel_pending mints.
#
# What the type cannot refuse is a write that bypasses the cell: a cast or a copy into its byte,
# or a second block() inside its friend. So kernel/ holds exactly two writes of BLOCKED, the
# cell's own assignment in thread.h and park_queueless's one block() in sync.cc, counted in the
# residue tests/lib/strip_comments.awk leaves.
#
# Run from the repo root, no arguments: tests/static/check_park_death_point.sh

set -u
set -f
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root

scratch_dir

STRIP="$(dirname "$0")/../lib/strip_comments.awk"
[ -r "$STRIP" ] || fail "tests/lib/strip_comments.awk is unreadable; nothing below can tell code from prose"

# An ASSIGNMENT, never a comparison: every reader tests the state with == or !=.
ASSIGN='[^=!<>]=[[:space:]]*ThreadState::BLOCKED'
BLOCK='\.block[[:space:]]*\([[:space:]]*\)'

corpus "$TMP/all" "tracked file under kernel/" kernel
grep -E '\.(cc|h|h\.in)$' "$TMP/all" > "$TMP/corpus"
NFILES=$(wc -l < "$TMP/corpus")
[ "$NFILES" -gt 0 ] || fail "no tracked C/C++ file under kernel/; an empty corpus passes forever"

: > "$TMP/writes"
for f in $(cat "$TMP/corpus"); do
    if ! awk -f "$STRIP" "$f" > "$TMP/stripped" 2> "$TMP/striperr"; then
        sed 's/^/      /' "$TMP/striperr" >&2
        fail "$f: comments and literals could not be blanked, so its verdict is UNKNOWN"
    fi
    grep -nE "$ASSIGN|$BLOCK" "$TMP/stripped" | sed "s|^|$f:|" >> "$TMP/writes"
done

cut -d: -f1 "$TMP/writes" | sort | uniq -c | awk '{ print $2, $1 }' > "$TMP/found"
printf '%s\n' 'kernel/include/kickos/thread.h 1' 'kernel/sync/sync.cc 1' > "$TMP/expected"

if ! cmp -s "$TMP/expected" "$TMP/found"; then
    echo "FAIL: the writes of ThreadState::BLOCKED in kernel/ are not the cell's two." >&2
    echo "      expected (file, count):" >&2
    sed 's/^/        /' "$TMP/expected" >&2
    echo "      found:" >&2
    sed 's/^/        /' "$TMP/found" >&2
    echo "      Park through park_queueless with the ParkToken park_cancel_pending minted," >&2
    echo "      asked in the parking caller's under-lock prologue ahead of its first side" >&2
    echo "      effect. The lines this gate found:" >&2
    sed 's/^/        /' "$TMP/writes" >&2
    exit 1
fi

echo "PASS: over $NFILES tracked kernel file(s), BLOCKED is written only by ThreadStateCell::block"
echo "      and its one call in park_queueless"
