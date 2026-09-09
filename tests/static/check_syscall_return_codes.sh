#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The syscall table's documented refusals against the dispatch's own. Every -KOS_E* code a
# dispatch arm RETURNS must appear on that syscall's entry in `enum kos_syscall_nr`, so the
# code list a caller reads in <kickos/sys/abi.h> cannot drift from the kernel that answers
# it. The invariant is `syscall-return-abi` in docs/reference/invariants.md, whose `source:`
# list already names this header beside the dispatch.
#
# Run from the repo root, no arguments, no build directory:
#   tests/static/check_syscall_return_codes.sh
#
#   corpus     the table header (RAW: the documented codes ARE its comments), and every
#              tracked *.cc / *.h under kernel/, arch/ and system/, each through
#              tests/lib/strip_comments.awk so a comment naming a code is never read as a
#              code the kernel returns. arch/ is in it because the chip backends are where
#              several refusals are actually spelled.
#
#   the codes  system/include/kickos/sys/errno.h is the taxonomy, and the only thing that
#              tells a code from an English word in a comment. An empty extraction is
#              refused: every check below would pass with nothing to compare.
#
#   ONE DIRECTION. A code the dispatch can return and the entry does not document is the
#              finding. The reverse is NOT checked: an entry naming a code its arm cannot
#              return passes here, because a refusal reached through a helper this scan does
#              not open would be indistinguishable from an invented one, and an exemption per
#              arm would put the second authority back. Nor is a WIDENED entry refused: one
#              that names every code in the taxonomy satisfies this vacuously. What IS
#              refused is a code name errno.h does not define, in its `-KOS_E*` spelling; a
#              BARE name in an enumeration is read as prose, because a comment spells EMPTY
#              and EXITED the same way.
#
#   depth      the arm's own return statements, plus the return statements of the functions
#              the arm CALLS by name. ONE level: a code a callee's callee returns is not
#              read. A name defined in the arm's own unit resolves there; otherwise EVERY
#              definition of it in the corpus counts and their codes union, because a
#              per-board seam is written once per backend and this table is one contract over
#              all of them. A MEMBER call (`pool.at()`) resolves against a type and is not
#              followed at all. The counts are printed, so the reach is visible rather than
#              assumed.
#
#   not read   a code forwarded through a variable (`return rc`, `return -err`), and a code
#              handed to ANOTHER thread by writing its wait_result, which is that thread's
#              own syscall answering. So a clean run is not proof that an entry is complete,
#              only that nothing it can be seen to return is missing.
#
#   exhausted  a syscall that charges the calling task's object budget must document all
#              THREE exhaustion answers: -KOS_ENOMEM (the pool), -KOS_EMFILE (the caller's
#              capability table), -KOS_EOVERFLOW (the budget). Their fixes are opposite, so a
#              collapsed pair sends a reader at the wrong one. The set is the budget gate's
#              own call sites and never a list kept here.
#
#   grouped    one arm under several `case` labels answers several syscalls out of ONE body,
#              and no text says which label reaches which refusal, so a code that arm returns
#              has to be documented by ONE label of the group. KOS_SYS_FRAME_MAP and
#              KOS_SYS_FRAME_UNMAP share an arm today.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# NOT set -e: the point is to collect EVERY finding in one run, not to stop at the first.

[ -f CMakeLists.txt ] || fail "run from the repo root (see WORKING_DIRECTORY)"
# `.git` is a FILE in a git worktree, not a directory, so -d alone fails every worktree.
[ -d .git ] || [ -f .git ] || fail "run from the repo root (no .git here)"
command -v git >/dev/null 2>&1 || fail "git not found; the corpus cannot be built"

scratch_dir

STRIP="$(dirname "$0")/../lib/strip_comments.awk"
HARVEST="$(dirname "$0")/syscall_return_harvest.awk"
AGREE="$(dirname "$0")/syscall_return_agree.awk"
[ -r "$STRIP" ] || fail "tests/lib/strip_comments.awk is unreadable; nothing below can strip a comment"
[ -r "$HARVEST" ] || fail "tests/static/syscall_return_harvest.awk is unreadable; the code side cannot be read"
[ -r "$AGREE" ] || fail "tests/static/syscall_return_agree.awk is unreadable; the table side cannot be read"

ABI="user/include/kickos/sys/abi.h"
DISPATCH="kernel/syscall/syscall.cc"
ERRNO="system/include/kickos/sys/errno.h"
CORPUS_FLOOR=200

for f in "$ABI" "$DISPATCH" "$ERRNO"; do
    [ -r "$f" ] || fail "$f is unreadable; the check has nothing to compare"
    git ls-files --error-unmatch -- "$f" > /dev/null 2>&1 \
        || fail "$f is not tracked; a gate builds its corpus from git and would read nothing"
done

# The taxonomy, out of the header that owns it. Reported, and floored: a sed that matched
# almost nothing would leave every code below unrecognised and every entry clean.
CODES="$(sed -n 's/^[[:blank:]]*KOS_\(E[A-Z][A-Z0-9]*\)[[:blank:]]*=.*/\1/p' "$ERRNO" | tr '\n' ' ')"
NCODES="$(printf '%s\n' "$CODES" | tr ' ' '\n' | sed '/^$/d' | wc -l | tr -d ' ')"
[ "$NCODES" -ge 10 ] \
    || fail "the error taxonomy came out of $ERRNO with $NCODES code(s); a short list leaves
      every documented code unrecognised and every entry vacuously clean"

# --- the instrument, on planted fixtures, before the corpus --------------------
# Each control is a MINIMAL PAIR: the positive and the negative differ in ONE property, the
# expected finding count is exact, and each fixture is read on its own so a control that is
# silent for the wrong reason shows up as the wrong number rather than as a pass.
mkdir -p "$TMP/st"

st_abi() { # <out> : entry lines on stdin, wrapped in the table
    {
        echo "enum kos_syscall_nr"
        echo "{"
        cat
        echo "};"
    } > "$1"
}

st_code() { # <out> <arms 0|1> [-v...] : source on stdin, stripped then harvested
    _o="$1"
    _a="$2"
    shift 2
    cat > "$TMP/st/src.cc"
    awk -f "$STRIP" "$TMP/st/src.cc" > "$TMP/st/src.res" 2> "$TMP/st/strip.err"
    _rc=$?
    [ "$_rc" -eq 0 ] || fail "the self-test fixture would not strip (awk exited $_rc); the controls below prove nothing"
    awk -v F="fake/dispatch.cc" -v ARMS="$_a" "$@" -f "$HARVEST" "$TMP/st/src.res" > "$_o" 2> "$TMP/st/harv.err"
    _rc=$?
    [ "$_rc" -eq 0 ] || {
        sed 's/^/      /' "$TMP/st/harv.err" >&2
        fail "the harvester exited $_rc on a self-test fixture; the controls below prove nothing"
    }
}

st_agree() { # <abi> <records> <out> [-v...] : exits with the awk's own status
    _ab="$1"
    _rc_in="$2"
    _out="$3"
    shift 3
    awk -v ABI="$_ab" -v DISPATCH="fake/dispatch.cc" -v CODES="$CODES" "$@" \
        -f "$AGREE" "$_ab" "$_rc_in" > "$_out" 2> "$_out.err"
    return $?
}

st_count() { # <agree output> : how many findings, out of a FILE and never a pipe
    awk -F"$TAB" '$1 == "find" { n++ } END { print n + 0 }' "$1"
}

st_expect() { # <what> <abi> <records> <expected count> [-v...]
    _what="$1"
    _ab="$2"
    _rec="$3"
    _want="$4"
    shift 4
    if ! st_agree "$_ab" "$_rec" "$TMP/st/out" "$@"; then
        sed 's/^/      /' "$TMP/st/out.err" >&2
        fail "the join refused the $_what control, so its count is unattributable"
    fi
    _got="$(st_count "$TMP/st/out")"
    [ "$_got" -eq "$_want" ] || {
        awk -F"$TAB" '$1 == "find" { print "      " $2 }' "$TMP/st/out" >&2
        fail "the $_what control reported $_got finding(s), expected $_want"
    }
}

# 1. A code the ARM ITSELF returns. The pair differs only in whether the entry names it.
st_code "$TMP/st/rec.arm" 1 <<'EOF'
uint64_t syscall_body(uintptr_t nr)
{
    switch (nr)
    {
        case KOS_SYS_ALPHA:
        {
            return static_cast<uint64_t>(-KOS_ENOMEM);
        }
    }
}
EOF
st_abi "$TMP/st/abi.bare" <<'EOF'
    KOS_SYS_ALPHA = 1, // () -> 0
EOF
st_abi "$TMP/st/abi.named" <<'EOF'
    KOS_SYS_ALPHA = 1, // () -> 0, or -KOS_ENOMEM (the pool)
EOF
st_expect "arm-returns-undocumented" "$TMP/st/abi.bare" "$TMP/st/rec.arm" 1
st_expect "arm-returns-documented" "$TMP/st/abi.named" "$TMP/st/rec.arm" 0

# 2. A code the CALLEE returns, one level down. Same pair, and the entry that names the
#    code the ARM returns is not enough: this is the reach the item exists for.
st_code "$TMP/st/rec.callee" 1 <<'EOF'
int helper(uint32_t* out)
{
    return -KOS_EBADF;
}
uint64_t syscall_body(uintptr_t nr)
{
    switch (nr)
    {
        case KOS_SYS_ALPHA:
        {
            return static_cast<uint64_t>(helper(&h));
        }
    }
}
EOF
st_abi "$TMP/st/abi.cbare" <<'EOF'
    KOS_SYS_ALPHA = 1, // () -> 0
EOF
st_abi "$TMP/st/abi.cnamed" <<'EOF'
    KOS_SYS_ALPHA = 1, // () -> 0, or -KOS_EBADF (bad cap)
EOF
st_expect "callee-returns-undocumented" "$TMP/st/abi.cbare" "$TMP/st/rec.callee" 1
st_expect "callee-returns-documented" "$TMP/st/abi.cnamed" "$TMP/st/rec.callee" 0

# 3. A refusal reached only with NO CALLER CONTEXT is not a syscall's answer. The pair
#    differs in the condition alone: a guard with anything else in it is a real gate.
st_code "$TMP/st/rec.guard" 1 <<'EOF'
int helper(uint32_t* out)
{
    Thread* c = sched::current();
    if (c == nullptr)
    {
        return -KOS_EPERM;
    }
    return 0;
}
uint64_t syscall_body(uintptr_t nr)
{
    switch (nr)
    {
        case KOS_SYS_ALPHA:
        {
            return static_cast<uint64_t>(helper(&h));
        }
    }
}
EOF
st_code "$TMP/st/rec.gate" 1 <<'EOF'
int helper(uint32_t* out)
{
    Thread* c = sched::current();
    if (c == nullptr or not c->privileged)
    {
        return -KOS_EPERM;
    }
    return 0;
}
uint64_t syscall_body(uintptr_t nr)
{
    switch (nr)
    {
        case KOS_SYS_ALPHA:
        {
            return static_cast<uint64_t>(helper(&h));
        }
    }
}
EOF
st_expect "null-context-guard" "$TMP/st/abi.cbare" "$TMP/st/rec.guard" 0
st_expect "privilege-gate" "$TMP/st/abi.cbare" "$TMP/st/rec.gate" 1

# 4. A code written to ANOTHER thread's wait_result is that thread's syscall answering.
st_code "$TMP/st/rec.wait" 1 <<'EOF'
int helper(uint32_t* out)
{
    s->wait_result = -KOS_ETIMEDOUT;
    return 0;
}
uint64_t syscall_body(uintptr_t nr)
{
    switch (nr)
    {
        case KOS_SYS_ALPHA:
        {
            return static_cast<uint64_t>(helper(&h));
        }
    }
}
EOF
st_code "$TMP/st/rec.ret" 1 <<'EOF'
int helper(uint32_t* out)
{
    return -KOS_ETIMEDOUT;
}
uint64_t syscall_body(uintptr_t nr)
{
    switch (nr)
    {
        case KOS_SYS_ALPHA:
        {
            return static_cast<uint64_t>(helper(&h));
        }
    }
}
EOF
st_expect "wait-result-write" "$TMP/st/abi.cbare" "$TMP/st/rec.wait" 0
st_expect "own-return" "$TMP/st/abi.cbare" "$TMP/st/rec.ret" 1

# 5. `as KOS_SYS_<other>` inherits the other entry's codes, which is how the timed twins are
#    written. The pair differs only in that phrase.
st_code "$TMP/st/rec.two" 1 <<'EOF'
uint64_t syscall_body(uintptr_t nr)
{
    switch (nr)
    {
        case KOS_SYS_ALPHA:
        {
            return static_cast<uint64_t>(-KOS_ENOMEM);
        }
        case KOS_SYS_BETA:
        {
            return static_cast<uint64_t>(-KOS_ENOMEM);
        }
    }
}
EOF
st_abi "$TMP/st/abi.as" <<'EOF'
    KOS_SYS_ALPHA = 1, // () -> 0, or -KOS_ENOMEM (the pool)
    KOS_SYS_BETA = 2,  // () -> as KOS_SYS_ALPHA, plus -KOS_ETIMEDOUT
EOF
st_abi "$TMP/st/abi.noas" <<'EOF'
    KOS_SYS_ALPHA = 1, // () -> 0, or -KOS_ENOMEM (the pool)
    KOS_SYS_BETA = 2,  // () -> like the entry above, plus -KOS_ETIMEDOUT
EOF
st_expect "inherited-codes" "$TMP/st/abi.as" "$TMP/st/rec.two" 0
st_expect "no-inheritance-phrase" "$TMP/st/abi.noas" "$TMP/st/rec.two" 1

# 6. A code name the taxonomy does not define, which is the one reverse claim held here. The
#    pair is the SPELLING: prefixed is a claim about a code, bare is prose, and the gate says
#    so rather than reporting every capitalised word in a comment.
st_abi "$TMP/st/abi.bogus" <<'EOF'
    KOS_SYS_ALPHA = 1, // () -> 0, or -KOS_ENOMEM, or -KOS_EWOULDBLOCK
EOF
st_abi "$TMP/st/abi.prose" <<'EOF'
    KOS_SYS_ALPHA = 1, // () -> 0, or -KOS_ENOMEM (the pool); the group is EMPTY afterwards
EOF
st_expect "undefined-code-name" "$TMP/st/abi.bogus" "$TMP/st/rec.arm" 1
st_expect "bare-word-is-prose" "$TMP/st/abi.prose" "$TMP/st/rec.arm" 0

# 7. The table and the dispatch must name the same syscalls, in both directions. The arm
#    here answers NO code, so a totality finding cannot be an undocumented one wearing a
#    different hat.
st_code "$TMP/st/rec.plain" 1 <<'EOF'
uint64_t syscall_body(uintptr_t nr)
{
    switch (nr)
    {
        case KOS_SYS_ALPHA:
        {
            return 0;
        }
    }
}
EOF
st_abi "$TMP/st/abi.only" <<'EOF'
    KOS_SYS_ALPHA = 1, // () -> 0
EOF
st_abi "$TMP/st/abi.extra" <<'EOF'
    KOS_SYS_ALPHA = 1, // () -> 0
    KOS_SYS_GAMMA = 2, // () -> 0
EOF
st_abi "$TMP/st/abi.missing" <<'EOF'
    KOS_SYS_DELTA = 1, // () -> 0
EOF
st_expect "table-and-dispatch-agree" "$TMP/st/abi.only" "$TMP/st/rec.plain" 0
st_expect "entry-with-no-arm" "$TMP/st/abi.extra" "$TMP/st/rec.plain" 1
# DELTA has no arm and ALPHA has no entry: two findings, one each way.
st_expect "arm-with-no-entry" "$TMP/st/abi.missing" "$TMP/st/rec.plain" 2

# 8. The three exhaustion answers, at a syscall that charges the task object budget.
st_code "$TMP/st/rec.admit" 1 <<'EOF'
int helper(uint32_t* out)
{
    if (not task_object_admit(CapType::CAP_SEM, c->task))
    {
        return -KOS_EOVERFLOW;
    }
    return -KOS_ENOMEM;
}
uint64_t syscall_body(uintptr_t nr)
{
    switch (nr)
    {
        case KOS_SYS_ALPHA:
        {
            return static_cast<uint64_t>(helper(&h));
        }
    }
}
EOF
st_abi "$TMP/st/abi.one" <<'EOF'
    KOS_SYS_ALPHA = 1, // () -> 0, or -KOS_ENOMEM (the pool), -KOS_EOVERFLOW (the budget)
EOF
st_abi "$TMP/st/abi.three" <<'EOF'
    KOS_SYS_ALPHA = 1, // () -> 0, or -KOS_ENOMEM (the pool), -KOS_EMFILE (the table),
                       //   -KOS_EOVERFLOW (the budget)
EOF
st_expect "collapsed-exhaustion" "$TMP/st/abi.one" "$TMP/st/rec.admit" 1
st_expect "whole-exhaustion" "$TMP/st/abi.three" "$TMP/st/rec.admit" 0

# 9. A grouped arm answers two syscalls from one body, so one label documenting the code is
#    the most this can ask; neither documenting it is still a finding, reported once.
st_code "$TMP/st/rec.group" 1 <<'EOF'
uint64_t syscall_body(uintptr_t nr)
{
    switch (nr)
    {
        case KOS_SYS_ALPHA:
        case KOS_SYS_BETA:
        {
            return static_cast<uint64_t>(-KOS_EBUSY);
        }
    }
}
EOF
st_abi "$TMP/st/abi.grpone" <<'EOF'
    KOS_SYS_ALPHA = 1, // () -> 0, or -KOS_EBUSY (held)
    KOS_SYS_BETA = 2,  // () -> 0
EOF
st_abi "$TMP/st/abi.grpnone" <<'EOF'
    KOS_SYS_ALPHA = 1, // () -> 0
    KOS_SYS_BETA = 2,  // () -> 0
EOF
st_expect "grouped-one-label" "$TMP/st/abi.grpone" "$TMP/st/rec.group" 0
st_expect "grouped-no-label" "$TMP/st/abi.grpnone" "$TMP/st/rec.group" 1

# --- the mutation: turn each clause off and the counts must MOVE ---------------
# Each arm below disables ONE clause over the fixture that clause keeps quiet, and the
# expected count differs per clause.
st_code "$TMP/st/rec.guard.noguard" 1 -v NOGUARD=1 <<'EOF'
int helper(uint32_t* out)
{
    Thread* c = sched::current();
    if (c == nullptr)
    {
        return -KOS_EPERM;
    }
    return 0;
}
uint64_t syscall_body(uintptr_t nr)
{
    switch (nr)
    {
        case KOS_SYS_ALPHA:
        {
            return static_cast<uint64_t>(helper(&h));
        }
    }
}
EOF
st_expect "null-context-guard DISABLED" "$TMP/st/abi.cbare" "$TMP/st/rec.guard.noguard" 1
st_expect "inheritance DISABLED" "$TMP/st/abi.as" "$TMP/st/rec.two" 1 -v NOAS=1

# --- the scanner's own death, which must never read as clean -------------------
# A gate whose reader dies and whose caller counts lines out of a pipe reports PASS over a
# corpus it never read. Each arm below is a separate refusal, not a finding count.
st_abi "$TMP/st/abi.open" <<'EOF'
    KOS_SYS_ALPHA = 1, // () -> 0
EOF
sed '$d' "$TMP/st/abi.open" > "$TMP/st/abi.unclosed"
if st_agree "$TMP/st/abi.unclosed" "$TMP/st/rec.arm" "$TMP/st/out"; then
    fail "the join accepted a table that never closes; every entry below the break would read clean"
fi
if st_agree "$TMP/st/abi.bare" "$TMP/st/rec.arm" "$TMP/st/out" -v CODES=""; then
    fail "the join accepted an EMPTY taxonomy, which leaves every documented code unrecognised"
fi
: > "$TMP/st/rec.empty"
if st_agree "$TMP/st/abi.bare" "$TMP/st/rec.empty" "$TMP/st/out"; then
    fail "the join accepted records holding no dispatch arm, so a harvester that read nothing would pass"
fi
cat > "$TMP/st/src.unbalanced" <<'EOF'
uint64_t syscall_body(uintptr_t nr)
{
    switch (nr)
    {
        case KOS_SYS_ALPHA:
        {
            return static_cast<uint64_t>(-KOS_ENOMEM);
        }
    }
EOF
awk -f "$STRIP" "$TMP/st/src.unbalanced" > "$TMP/st/res.unbalanced" 2>/dev/null
if awk -v F="fake/dispatch.cc" -v ARMS=1 -f "$HARVEST" "$TMP/st/res.unbalanced" \
        > /dev/null 2>/dev/null; then
    fail "the harvester accepted a file whose braces never balance, so the arms below the break read as empty"
fi
printf 'int f(void)\n{\n    return 0;\n}\n' > "$TMP/st/res.noarm"
if awk -v F="fake/dispatch.cc" -v ARMS=1 -f "$HARVEST" "$TMP/st/res.noarm" \
        > /dev/null 2>/dev/null; then
    fail "the harvester accepted a dispatch unit holding no case label, so a renamed switch would read clean"
fi

# --- the corpus ---------------------------------------------------------------
git ls-files -- 'kernel/*.cc' 'kernel/*.h' 'arch/*.cc' 'arch/*.h' 'system/*.cc' 'system/*.h' \
    > "$TMP/all" || fail "git ls-files failed"
require_nonempty "$TMP/all" "git ls-files matched no kernel or arch source file; every check below would pass vacuously"
grep -qxF "$DISPATCH" "$TMP/all" \
    || fail "$DISPATCH is not in the corpus; the dispatch arms would go unread and every entry would read clean"
N="$(wc -l < "$TMP/all" | tr -d ' ')"
[ "$N" -ge "$CORPUS_FLOOR" ] \
    || fail "the corpus is $N source file(s), under the floor of $CORPUS_FLOOR; a corpus that
      shrank is how a gate goes quiet without a count moving"

: > "$TMP/records"
: > "$TMP/refused"
READ=0
i=0
while IFS= read -r f; do
    [ -f "$f" ] || fail "tracked file is missing from the worktree: $f"
    i=$((i + 1))
    if awk -f "$STRIP" "$f" > "$TMP/res.$i" 2>> "$TMP/refused"; then
        :
    else
        rc=$?
        [ "$rc" -eq 2 ] || fail "awk exited $rc stripping $f"
        continue
    fi
    arms=0
    if [ "$f" = "$DISPATCH" ]; then
        arms=1
    fi
    awk -v F="$f" -v ARMS="$arms" -f "$HARVEST" "$TMP/res.$i" >> "$TMP/records" 2>> "$TMP/refused"
    rc=$?
    [ "$rc" -eq 0 ] || fail "the harvester exited $rc on $f; its records are missing and the
      entries it feeds would read clean (see the refusal above)"
    READ=$((READ + 1))
    rm -f "$TMP/res.$i"
done < "$TMP/all"

if [ -s "$TMP/refused" ]; then
    sed 's/^/      /' "$TMP/refused" >&2
    fail "the scan could not read $(wc -l < "$TMP/refused" | tr -d ' ') file(s) to the end, so their verdict is UNKNOWN, not clean"
fi
[ "$READ" -eq "$N" ] \
    || fail "$READ of $N corpus file(s) were harvested; a silent skip is how a corpus shrinks"

awk -v ABI="$ABI" -v DISPATCH="$DISPATCH" -v CODES="$CODES" -f "$AGREE" \
    "$ABI" "$TMP/records" > "$TMP/out" 2> "$TMP/outerr"
rc=$?
if [ "$rc" -ne 0 ]; then
    sed 's/^/      /' "$TMP/outerr" >&2
    fail "the join exited $rc; NOTHING was compared and this is not a clean tree"
fi

ENTRIES="$(awk -F"$TAB" '$1 == "stat" && $2 == "entries" { print $3 }' "$TMP/out")"
ARMS_N="$(awk -F"$TAB" '$1 == "stat" && $2 == "arms" { print $3 }' "$TMP/out")"
LABELS="$(awk -F"$TAB" '$1 == "stat" && $2 == "labels" { print $3 }' "$TMP/out")"
RESOLVED="$(awk -F"$TAB" '$1 == "stat" && $2 == "callees_resolved" { print $3 }' "$TMP/out")"
AMBIG="$(awk -F"$TAB" '$1 == "stat" && $2 == "callees_multi_unit" { print $3 }' "$TMP/out")"
EXTERN="$(awk -F"$TAB" '$1 == "stat" && $2 == "callees_external" { print $3 }' "$TMP/out")"
[ -n "$ENTRIES" ] || fail "the join printed no entry count; its output is not what this reads"
[ "$ENTRIES" -ge 2 ] || fail "the table came out as $ENTRIES entry(ies); the parse is wrong"
[ "$LABELS" -ge 2 ] || fail "the dispatch came out as $LABELS label(s); the parse is wrong"

echo "== checked $ENTRIES table entry(ies) against $ARMS_N dispatch arm(s) over $LABELS label(s), out of $READ tracked kernel, arch and system file(s); $NCODES code(s) in the taxonomy, $RESOLVED callee(s) resolved, $AMBIG of them in more than one unit, $EXTERN outside the kernel corpus =="

NFIND="$(awk -F"$TAB" '$1 == "find" { n++ } END { print n + 0 }' "$TMP/out")"
if [ "$NFIND" -gt 0 ]; then
    awk -F"$TAB" '$1 == "find" { print "  " $2 }' "$TMP/out" >&2
    echo "" >&2
    echo "FAIL: $NFIND finding(s): a syscall answers a code its table entry in $ABI does not document." >&2
    echo "      Document the code on the entry, or take the refusal out of the arm. Do NOT" >&2
    echo "      widen an entry past what its arm answers: the table is what a caller reads." >&2
    exit 1
fi

echo "PASS: every -KOS_E* code the dispatch is seen to return is documented on its own table entry"
