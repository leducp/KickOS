#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The four budget-below-pool asserts of `object-pool-keeps-a-slot-from-any-one-task`, held
# to being PRESENT and STRICT. Those asserts are the structural half of the invariant.
# `task_object_ceiling` reads a budget and no pool width, so what keeps a task's ceiling
# below its pool is a BUILD-TIME relation and nothing else. Delete one, or relax its `<` to
# `<=`, and every board still configures, builds and boots: the assert has no other reader.
# This is that reader.
#
# Run from the repo root, no arguments, no build directory:
#   tests/static/check_object_budget_asserts.sh
#
#   corpus     kernel/include/kickos/cap.h alone, taken from `git ls-files` so an untracked
#              copy cannot be what gets read.
#
#   the kinds  DERIVED from task_object_ceiling's own switch arms, never listed here: a
#              charged kind added there and forgotten in the asserts is a missing record
#              rather than a name nobody added to a list. That is the whole reason this is
#              a reader over the function and not four greps.
#
#   the clauses, per charged kind:
#     1. exactly one static_assert names that kind's budget macro in the pool-above-budget
#        shape. Zero is the deleted assert; two would make it ambiguous which one holds.
#     2. its operator is `<` and not `<=`. At `<=` a task may hold a pool's LAST slot, which
#        is precisely the denial the invariant exists to prevent.
#     3. the same pool macro stands on both sides, so the `== 0` escape is the absence of
#        the pool the budget is compared against and not of some other pool.
#     4. no two kinds share a pool macro, or one pool would carry two budgets and a
#        copy-paste of the wrong line would read as a check.
#
# WHAT THIS DOES NOT COVER
#   - a charged kind deleted from task_object_ceiling AND from the asserts together. That
#     removes the pool from the mechanism rather than leaving it unguarded, and the arms in
#     tests/unit/taskbudget are what would notice.
#   - whether the budgets and pools resolve to sane NUMBERS on any board. The asserts
#     themselves are what says that, once they are here and strict.
#   - the AMP port seat, which is a HOLD no admission sees and is bounded in CMakeLists.txt
#     instead. See docs/reference/invariants.md.

set -u
. "$(dirname "$0")/../lib/gate.sh"

export LC_ALL=C

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

scratch_dir

STRIP="$(dirname "$0")/../lib/strip_comments.awk"
READER="$(dirname "$0")/object_budget_asserts.awk"
[ -r "$STRIP" ] || fail "tests/lib/strip_comments.awk is unreadable; prose would read as code"
[ -r "$READER" ] || fail "$READER is unreadable"

HEADER=kernel/include/kickos/cap.h

# --- the scanner, as one function, so the self-test runs the SAME program as the tree -----
#
# read_header <file> <workdir> <strip> <reader>, leaving <workdir>/records.
read_header() {
    _f="$1"
    _w="$2"
    _strip="$3"
    _reader="$4"
    mkdir -p "$_w" || fail "mkdir failed under $_w"
    : > "$_w/records"
    [ -r "$_f" ] || fail "cannot read $_f"
    # ANY nonzero status is a refusal: an awk that dies on its own program prints nothing,
    # and an empty record set would then read as a header with no charged kind in it.
    awk -f "$_strip" "$_f" > "$_w/stripped" 2> "$_w/striperr" \
        || fail "the comment strip exited nonzero over $_f, so the verdict is UNKNOWN:
$(sed 's/^/        /' "$_w/striperr")"
    awk -f "$_reader" "$_w/stripped" > "$_w/records" 2> "$_w/readerr" \
        || fail "the reader exited nonzero over $_f, so the verdict is UNKNOWN:
$(sed 's/^/        /' "$_w/readerr")"
}

# judge <workdir>, leaving <workdir>/findings and printing the kinds it read.
judge() {
    _w="$1"
    : > "$_w/findings"
    _kinds="$(awk '$1 == "CEIL" { print $2 }' "$_w/records")"
    _n="$(printf '%s\n' "$_kinds" | grep -c '[^[:blank:]]')"
    if [ "$_n" -lt 2 ]; then
        printf 'the reader found %s charged kind(s) in task_object_ceiling; a reader that\n' "$_n" >> "$_w/findings"
        printf 'cannot see the switch would report a clean tree over an unguarded one\n' >> "$_w/findings"
        return 0
    fi
    # A pipeline subshell cannot carry _seen_pools out, so the loop reads a FILE.
    _seen_pools=""
    awk '$1 == "CEIL"' "$_w/records" > "$_w/ceil"
    while read -r _tag _kind _bud; do
        [ -n "${_bud:-}" ] || continue
        awk -v B="$_bud" '$1 == "ASSERT" && $3 == B' "$_w/records" > "$_w/hit"
        _h="$(grep -c '[^[:blank:]]' "$_w/hit")"
        if [ "$_h" -eq 0 ]; then
            printf '%s charges %s and NO static_assert holds it below its pool: one task\n' \
                   "$_kind" "$_bud" >> "$_w/findings"
            printf '    could take the last slot of that pool on any board that sized the two alike\n' >> "$_w/findings"
            continue
        fi
        if [ "$_h" -ne 1 ]; then
            printf '%s charges %s and %s asserts name it; which one holds is ambiguous\n' \
                   "$_kind" "$_bud" "$_h" >> "$_w/findings"
            continue
        fi
        _pool1="$(awk '{ print $2 }' "$_w/hit")"
        _op="$(awk '{ print $4 }' "$_w/hit")"
        _pool2="$(awk '{ print $5 }' "$_w/hit")"
        if [ "$_op" != "<" ]; then
            printf '%s asserts %s %s %s: at anything but a strict < a task may hold the\n' \
                   "$_kind" "$_bud" "$_op" "$_pool1" >> "$_w/findings"
            printf '    LAST slot of that pool, which is the denial this invariant exists to prevent\n' >> "$_w/findings"
        fi
        if [ "$_pool1" != "$_pool2" ]; then
            printf '%s asserts %s == 0 beside %s < %s: the zero escape names one pool and\n' \
                   "$_kind" "$_pool1" "$_bud" "$_pool2" >> "$_w/findings"
            printf '    the comparison another, so a board with neither present is unchecked\n' >> "$_w/findings"
        fi
        case " $_seen_pools " in
            *" $_pool2 "*)
                printf '%s is measured against %s, which another charged kind already\n' \
                       "$_kind" "$_pool2" >> "$_w/findings"
                printf '    names: one pool carrying two budgets is a copy-paste reading as a check\n' >> "$_w/findings"
                ;;
            *) _seen_pools="$_seen_pools $_pool2" ;;
        esac
    done < "$_w/ceil"
}

# --- self-test: prove the reader both ways before it reads the tree -----------------------
# Every clause gets a MINIMAL PAIR, and the positive control comes first: a reader that saw
# nothing would pass every negative arm below and report a clean tree.
selftest_dir="$TMP/self"
mkdir -p "$selftest_dir" || fail "mkdir failed under $selftest_dir"

write_case() { # <file> <op-for-kind-b> <pool-for-kind-b> <emit-assert-b>
    _out="$1"
    _opb="$2"
    _poolb="$3"
    _hasb="$4"
    cat > "$_out" <<EOF
namespace kickos
{
    constexpr int task_object_ceiling(CapType kind)
    {
        switch (kind)
        {
        case CapType::CAP_A:
        {
            return KICKOS_TASK_A_BUDGET;
        }
        case CapType::CAP_B:
        {
            return KICKOS_TASK_B_BUDGET;
        }
        default:
        {
            return 0;
        }
        }
    }

    static_assert(KICKOS_MAX_A == 0
                      or KICKOS_TASK_A_BUDGET < KICKOS_MAX_A,
                  "");
EOF
    if [ "$_hasb" = yes ]; then
        cat >> "$_out" <<EOF
    static_assert($_poolb == 0
                      or KICKOS_TASK_B_BUDGET $_opb $_poolb,
                  "");
EOF
    fi
    printf '}\n' >> "$_out"
}

selftest_expect() { # <name> <expected-finding-count>
    _name="$1"
    _want="$2"
    read_header "$selftest_dir/$_name.h" "$selftest_dir/$_name" "$STRIP" "$READER"
    judge "$selftest_dir/$_name"
    _got="$(grep -c '[^[:blank:]]' "$selftest_dir/$_name/findings")"
    if [ "$_want" = 0 ] && [ "$_got" -ne 0 ]; then
        sed 's/^/        /' "$selftest_dir/$_name/findings" >&2
        fail "self-test '$_name': the reader reported on a header that satisfies every clause"
    fi
    if [ "$_want" != 0 ] && [ "$_got" -eq 0 ]; then
        fail "self-test '$_name': the reader passed a header it must refuse, so this gate
    would report a clean tree over an unguarded one"
    fi
}

# Positive control FIRST: two charged kinds, two strict asserts, a pool each.
write_case "$selftest_dir/good.h" "<" KICKOS_MAX_B yes
selftest_expect good 0
# And it really read the switch, rather than finding nothing to complain about.
_kinds="$(awk '$1 == "CEIL" { print $2 }' "$selftest_dir/good/records" | tr '\n' ' ')"
case "$_kinds" in
    *A*B*) : ;;
    *) fail "self-test 'good': the reader named kinds '$_kinds' and not the two in the
    header, so every negative arm below would be vacuous" ;;
esac

# One assert deleted.
write_case "$selftest_dir/missing.h" "<" KICKOS_MAX_B no
selftest_expect missing 1
# The relation relaxed to <=.
write_case "$selftest_dir/relaxed.h" "<=" KICKOS_MAX_B yes
selftest_expect relaxed 1
# The second kind measured against the FIRST kind's pool.
write_case "$selftest_dir/shared.h" "<" KICKOS_MAX_A yes
selftest_expect shared 1
# A header with no switch at all: the reader must refuse rather than report clean.
printf 'namespace kickos { int nothing_here = 0; }\n' > "$selftest_dir/blind.h"
selftest_expect blind 1

# --- the tree ----------------------------------------------------------------------------
git ls-files --error-unmatch "$HEADER" > /dev/null 2>&1 \
    || fail "$HEADER is not tracked, so the corpus would be a file git does not carry"

read_header "$HEADER" "$TMP/tree" "$STRIP" "$READER"
judge "$TMP/tree"

echo "object_budget_asserts: $HEADER"
awk '$1 == "CEIL" { printf "object_budget_asserts:   %s charges %s\n", $2, $3 }' \
    "$TMP/tree/records"

if [ -s "$TMP/tree/findings" ]; then
    sed 's/^/    /' "$TMP/tree/findings" >&2
    fail "the budget-below-pool relation is not held for every charged pool; see
    docs/reference/invariants.md (object-pool-keeps-a-slot-from-any-one-task)"
fi

echo "object_budget_asserts: OK"
exit 0
