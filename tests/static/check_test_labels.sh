#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate on the `host` ctest label, which is load-bearing in two directions:
#
#   - `kickos_decline_image_tests` in the root CMakeLists sets DISABLED TRUE on any test NOT
#     labelled `host` when KICKOS_BUILD_INTEGRATION_TESTS=OFF, so a host test that forgets
#     the label silently stops running under that knob.
#   - `ctest -L host` is the batchable set and `ctest -LE host` is the set that must run
#     standalone, the sim and qemu suites having no silicon clock and failing under the load
#     of a back-to-back run. An image test wrongly labelled `host` joins the batch and
#     survives the knob meant to skip it.
#
# Usage:
#   check_test_labels.sh <ctest> <cmake> <build-dir> <src-dir> <declined:yes|no>
# <declined> is `yes` when KICKOS_BUILD_INTEGRATION_TESTS is OFF; it is what makes the
# DISABLED leg decidable.
#
# The classification is DECLARED, in tests/static/test_classes.txt, and cannot be derived:
# no property of a ctest command line separates the two sets. The gate maps each entry to
# the program it runs and REFUSES an entry whose program is undeclared, so a new test forces
# an explicit choice rather than inheriting one.
#
# THEREFORE NOT CAUGHT. Know these before trusting a green run:
#   - a program declared with the WRONG class is ratified, not questioned. This pins the
#     declaration against the labels; a human pins the declaration against the script.
#   - a declaration line no board registers any more is dead, and only its `src` half rots
#     visibly (the file check below). One board's run cannot see the fleet's union.
#   - a gate that reads a clock WITHOUT running an image would be `host` here and still
#     unbatchable. None exists today.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# NOT set -e: the point is to collect EVERY finding in one run, not to stop at the first.

if [ "$#" -ne 5 ]; then
    fail "usage: check_test_labels.sh <ctest> <cmake> <build-dir> <src-dir> <declined:yes|no>"
fi
CTEST="$1"
CMAKE="$2"
# Taken verbatim from CMake, never re-resolved: the JSON spells these directories the way
# CMake does, and a `cd && pwd` here would resolve a symlink it did not.
BUILD="${3%/}"
SRC="${4%/}"
DECLINED="$5"

[ -x "$CTEST" ] || fail "no ctest at $CTEST"
[ -x "$CMAKE" ] || fail "no cmake at $CMAKE"
[ -d "$BUILD" ] || fail "no build directory at $BUILD"
case "$DECLINED" in
    yes | no) ;;
    *) fail "<declined> is 'yes' or 'no', not '$DECLINED'" ;;
esac

DECL="$SRC/tests/static/test_classes.txt"
FLATTEN="$SRC/tests/static/ctest_tests.cmake"
[ -f "$DECL" ] || fail "no declaration file at $DECL"
[ -f "$FLATTEN" ] || fail "no flattener at $FLATTEN"

FIXTURE=kickos_build

scratch_dir

# --- the suite, as ctest itself reports it ------------------------------------
tool_out "$TMP/tests.json" '"tests"' \
    "$CTEST" --test-dir "$BUILD" --show-only=json-v1
tool_out "$TMP/flatten.log" '' \
    "$CMAKE" "-DJSON=$TMP/tests.json" "-DOUT=$TMP/table" -P "$FLATTEN"
require_nonempty "$TMP/table" "the flattener produced no test table"

# --- the declarations ---------------------------------------------------------
sed -e 's/#.*//' "$DECL" | grep -v '^[[:space:]]*$' > "$TMP/decl.txt"
require_nonempty "$TMP/decl.txt" "$DECL declares nothing"

: > "$TMP/host.txt"
: > "$TMP/image.txt"
while read -r cls root rel extra; do
    [ -z "$extra" ] || fail "$DECL: trailing junk after '$cls $root $rel': $extra"
    case "$root" in
        src | build) ;;
        *) fail "$DECL: '$root' is not a root; use src or build" ;;
    esac
    case "$rel" in
        "" | /* | *..*) fail "$DECL: '$rel' is not a path relative to $root" ;;
    esac
    if [ "$root" = src ] && [ ! -f "$SRC/$rel" ]; then
        fail "$DECL declares src/$rel, which no longer exists"
    fi
    case "$cls" in
        host) echo "$root $rel" >> "$TMP/host.txt" ;;
        image) echo "$root $rel" >> "$TMP/image.txt" ;;
        *) fail "$DECL: '$cls' is not a class; use host or image" ;;
    esac
done < "$TMP/decl.txt"

DUP="$(cat "$TMP/host.txt" "$TMP/image.txt" | sort | uniq -d)"
[ -z "$DUP" ] || fail "$DECL declares a program twice: $DUP"

# --- one pass over the suite --------------------------------------------------
: > "$TMP/findings.txt"
report() { echo "$1" >> "$TMP/findings.txt"; }

N_HOST=0
N_IMAGE=0
N_TOTAL=0
SAW_FIXTURE=0

while IFS="$TAB" read -r name labels disabled prog; do
    N_TOTAL=$((N_TOTAL + 1))
    LABELLED=0
    case "$labels" in
        *,host,*) LABELLED=1 ;;
    esac

    # The build fixture is deliberately unlabelled: fixtures are resolved AFTER label
    # filtering, so it joins BOTH partitions and rebuilds either one. Labelling it would
    # not break `-L host`, which is why nothing else here would notice.
    if [ "$name" = "$FIXTURE" ]; then
        SAW_FIXTURE=1
        [ "$prog" = "@build" ] || report "$name: the build fixture runs $prog, not a build"
        [ "$LABELLED" = 0 ] || report "$name: the build fixture must stay unlabelled"
        [ "$disabled" = 0 ] || report "$name: the build fixture is DISABLED"
        continue
    fi

    case "$prog" in
        @none)
            report "$name: ctest reports no command, so its program cannot be classified -- build the tree first"
            continue
            ;;
        "$BUILD"/*)
            root=build
            rel="${prog#"$BUILD"/}"
            ;;
        "$SRC"/*)
            root=src
            rel="${prog#"$SRC"/}"
            ;;
        *)
            report "$name: runs $prog, which is in neither the source nor the build tree"
            continue
            ;;
    esac

    if grep -qxF "$root $rel" "$TMP/host.txt"; then
        N_HOST=$((N_HOST + 1))
        if [ "$LABELLED" = 0 ]; then
            report "$name: runs no image ($root/$rel) and carries no LABELS host, so KICKOS_BUILD_INTEGRATION_TESTS=OFF DISABLES it"
        fi
        if [ "$DECLINED" = yes ] && [ "$disabled" != 0 ]; then
            report "$name: runs no image ($root/$rel) yet was declined"
        fi
        if [ "$DECLINED" = no ] && [ "$disabled" != 0 ]; then
            report "$name: DISABLED with nothing in this tree to disable it"
        fi
    elif grep -qxF "$root $rel" "$TMP/image.txt"; then
        N_IMAGE=$((N_IMAGE + 1))
        if [ "$LABELLED" = 1 ]; then
            report "$name: runs an image ($root/$rel) and carries LABELS host, so it joins the batchable set and survives KICKOS_BUILD_INTEGRATION_TESTS=OFF"
        fi
        if [ "$DECLINED" = yes ] && [ "$disabled" = 0 ]; then
            report "$name: runs an image ($root/$rel) and was not declined"
        fi
        if [ "$DECLINED" = no ] && [ "$disabled" != 0 ]; then
            report "$name: DISABLED with nothing in this tree to disable it"
        fi
    else
        report "$name: runs $root/$rel, which $DECL does not classify -- add a host or image line for it"
    fi
done < "$TMP/table"

# A suite the gate could not read leaves every assertion above unexecuted.
[ "$N_TOTAL" -gt 0 ] || fail "read zero tests out of $BUILD"
[ "$SAW_FIXTURE" = 1 ] || fail "no $FIXTURE fixture in this suite, so every test here may have run a stale binary"
[ "$N_HOST" -gt 0 ] || fail "not one host test in this suite -- the source-tree gates register on every board, so the mapping is broken"

echo "== $N_TOTAL test(s): $N_HOST host, $N_IMAGE image, 1 build fixture; $(wc -l < "$TMP/decl.txt" | tr -d ' ') program(s) declared, integration declined: $DECLINED =="

if [ -s "$TMP/findings.txt" ]; then
    cat "$TMP/findings.txt" >&2
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/findings.txt" | tr -d ' ') test(s) disagree with $DECL." >&2
    echo "      Fix the label, or declare the program's class. Both directions are silent without this gate." >&2
    exit 1
fi

echo "PASS: every registered test carries the label its declared class requires"
