#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The CI workflow selects tests BY NAME in about thirty places, and `ctest -R` is happy when an
# alternative of a `|` list matches nothing. tests/ci/ctest_named.sh is what turns that into a
# refusal. Two things have to hold for that to mean anything, and this gate asserts both:
#
#   the corpus   no name-based selection in .github/workflows/ci.yml reaches ctest directly.
#                A `ctest -R '<list>'` line added beside the helper reopens the hole in
#                silence, the step staying green while one name of several selects nothing.
#
#   the instrument  ctest_named.sh still refuses a dead alternative. It is driven here against
#                a planted `ctest` that answers scripted counts, so what is proven is the
#                HELPER'S DECISION and not ctest's matcher: the real matcher is exercised by
#                every CI step that calls it.
#
# WHAT THIS CANNOT SAY, and it is the failure that sits next to it: whether a name is
# registered ON THE PRESET THAT NAMES IT. Every name this file has had to drop so far
# (fp_switch off a soft-float board, mpu_fault off a board with KICKOS_HAVE_MPU at zero) is
# registered by some OTHER preset in the same tree, so a "this name exists somewhere" check
# answers green on all of them. Only configuring that preset decides it, which is the CI job
# itself. Do not grow this gate in that direction; it would pass and say nothing.
#
# Run from the repo root, no arguments, no build directory:
#   tests/static/check_ci_named_lists.sh

set -u
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root

scratch_dir

CI_YML=".github/workflows/ci.yml"
HELPER="tests/ci/ctest_named.sh"

# About half the call sites the workflow carries. A scan that read a truncated file, or was
# pointed at the wrong path, would otherwise report a clean corpus.
CALL_FLOOR=14

[ -f "$CI_YML" ] || fail "$CI_YML is not here, so nothing below was checked"
[ -f "$HELPER" ] || fail "$HELPER is not here; the workflow names it at every list"
[ -x "$HELPER" ] || fail "$HELPER is not executable, so every CI step calling it dies on exec"

# --- the scan -------------------------------------------------------------------------------
# A logical line: comments erased, then backslash continuations joined, so a selection split
# across two YAML lines is read as the one command it is.
scan_yaml() { # <yaml> <findings-out> <calls-out>
    sed -e 's/^[[:blank:]]*#.*$//' -e 's/[[:blank:]]#[[:blank:]].*$//' "$1" \
        | awk '{
              if (held != "") { line = held " " $0; held = "" } else { line = $0 }
              if (line ~ /\\[[:blank:]]*$/) { sub(/\\[[:blank:]]*$/, "", line); held = line; next }
              print line
          }
          END { if (held != "") print held }' > "$TMP/logical"
    LC_ALL=C grep -nE '(^|[^[:alnum:]_/.-])ctest([[:blank:]]|$)' "$TMP/logical" \
        | LC_ALL=C grep -E '(^|[[:blank:]])-R([[:blank:]]|=)' > "$2"
    LC_ALL=C grep -cF 'ctest_named.sh' "$TMP/logical" > "$3"
    return 0
}

: > "$TMP/findings"
: > "$TMP/calls"
scan_yaml "$CI_YML" "$TMP/findings" "$TMP/calls"
CALLS="$(cat "$TMP/calls")"

# --- the controls ---------------------------------------------------------------------------
# The scanner, on a workflow planted with one bypass beside one ordinary helper call. Without
# this, a scanner whose regex reaches nothing reports the same clean corpus as a clean file.
cat > "$TMP/ctl.yml" <<'CTL'
jobs:
  planted:
    steps:
      - run: |
          ctest --preset qemu -R 'hello|fp_switch'
          tests/ci/ctest_named.sh 'hello|mpu_fault' \
            --preset qemu --no-tests=error
          ctest --preset qemu -E "$POLLED" -LE tree
CTL
: > "$TMP/ctl.findings"
: > "$TMP/ctl.calls"
scan_yaml "$TMP/ctl.yml" "$TMP/ctl.findings" "$TMP/ctl.calls"
[ "$(wc -l < "$TMP/ctl.findings" | tr -d ' ')" -eq 1 ] \
    || fail "the planted bypass was not the one and only finding on the control workflow, so
      the scan below reports absence and not cleanliness"
LC_ALL=C grep -q 'fp_switch' "$TMP/ctl.findings" \
    || fail "the control's finding is not the planted bypass line, so the scan matches
      something other than what it claims"
[ "$(cat "$TMP/ctl.calls")" -eq 1 ] \
    || fail "the control counted $(cat "$TMP/ctl.calls") helper call(s) against the one planted"

# The helper, driven against a planted ctest. The table maps an alternative to the count the
# stub answers for it; a name absent from the table answers zero, which is the dead name.
mkdir "$TMP/bin"
cat > "$TMP/bin/ctest" <<'STUB'
#!/bin/sh
set -u
_dry=0
_sel=""
_prev=""
for _a in "$@"; do
    if [ "$_prev" = "-R" ]; then
        _sel="$_a"
    fi
    if [ "$_a" = "-N" ]; then
        _dry=1
    fi
    _prev="$_a"
done
if [ "$_dry" -eq 0 ]; then
    printf '%s\n' "$_sel" >> "$KOS_STUB_RAN"
    exit 0
fi
if [ "${KOS_STUB_MUTE:-0}" = 1 ]; then
    echo "no count from this build"
    exit 0
fi
# An EMPTY -R selects every test, which is what ctest does and the opposite of selecting
# none. A stub answering zero here would let a helper that dropped its empty-alternative
# check refuse through the zero path and read as still covered.
if [ -z "$_sel" ]; then
    wc -l < "$KOS_STUB_TABLE" | sed 's/^[[:blank:]]*/Total Tests: /'
    exit 0
fi
_n="$(sed -n "s/^$_sel=//p" "$KOS_STUB_TABLE")"
if [ -z "$_n" ]; then
    _n=0
fi
echo "Total Tests: $_n"
exit 0
STUB
chmod +x "$TMP/bin/ctest"

cat > "$TMP/table" <<'TAB'
hello=1
mpu_fault=2
TAB
KOS_STUB_TABLE="$TMP/table"
export KOS_STUB_TABLE
HELPER_ABS="$PWD/$HELPER"

# <prose> <expected 0 for pass / 1 for refusal> <expected run count> <list>
drive() {
    _prose="$1"
    _want="$2"
    _runs="$3"
    _list="$4"
    KOS_STUB_RAN="$TMP/ran"
    export KOS_STUB_RAN
    : > "$KOS_STUB_RAN"
    PATH="$TMP/bin:$PATH" "$HELPER_ABS" "$_list" --test-dir "$TMP" > "$TMP/out" 2>&1
    _rc=$?
    if [ "$_want" -eq 0 ] && [ "$_rc" -ne 0 ]; then
        sed 's/^/      /' "$TMP/out" >&2
        fail "$_prose: the helper refused a list every alternative of which selects a test.
      A gate that refuses a live list is one nobody can run."
    fi
    if [ "$_want" -eq 1 ] && [ "$_rc" -eq 0 ]; then
        sed 's/^/      /' "$TMP/out" >&2
        fail "$_prose: the helper exited 0 on a list it owes a refusal. Every CI step calling
      it would run whatever the list still selects and say nothing about the rest, which is
      the whole failure this helper exists to stop."
    fi
    _got="$(wc -l < "$KOS_STUB_RAN" | tr -d ' ')"
    [ "$_got" -eq "$_runs" ] \
        || fail "$_prose: the helper ran the selection $_got time(s) against $_runs expected"
}

drive "a live list" 0 1 'hello|mpu_fault'
drive "a dead alternative beside a live one" 1 0 'hello|reclaimwit_park'
drive "a dead alternative first" 1 0 'reclaimwit_park|hello'
drive "an empty alternative" 1 0 'hello||mpu_fault'

# A count the helper cannot read is UNKNOWN and not zero, and a run it cannot judge is refused
# rather than run.
KOS_STUB_MUTE=1
export KOS_STUB_MUTE
drive "a build that answers no count" 1 0 'hello|mpu_fault'
KOS_STUB_MUTE=0

# The controls above are what make the live list meaningful: without them a helper rewritten to
# `exec ctest "$@" -R "$1"` passes the first drive and every other one too.
drive "a live list, after the refusals" 0 1 'hello|mpu_fault'

# --- the verdict ----------------------------------------------------------------------------
[ "$CALLS" -ge "$CALL_FLOOR" ] \
    || fail "$CI_YML holds $CALLS ctest_named.sh call site(s), beneath the floor of $CALL_FLOOR:
      this is not the workflow, so a clean result below is a scan that read the wrong file"

echo "== checked $CALLS name-list call site(s) in $CI_YML, and $HELPER against 6 planted list(s) =="

if [ -s "$TMP/findings" ]; then
    sed 's/^/      /' "$TMP/findings" >&2
    echo "" >&2
    echo "FAIL: the line(s) above select tests by name through ctest directly. --no-tests=error" >&2
    echo "      fires only on an ENTIRELY empty selection, so one name of several going away" >&2
    echo "      leaves the step running the rest and exiting 0. Call $HELPER," >&2
    echo "      which asks ctest for each alternative on its own. Where selecting nothing is" >&2
    echo "      the right answer on that board, name the board's own gates instead of sharing" >&2
    echo "      a list with boards that register more." >&2
    exit 1
fi

echo "PASS: every name-based selection in $CI_YML goes through $HELPER, which refuses a dead name"
