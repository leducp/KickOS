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
# <declined> is `yes` when KICKOS_BUILD_INTEGRATION_TESTS is OFF, which is what makes the
# DISABLED leg decidable. <complete> is `yes` on a tree that registers every build-rooted
# program declared, and only there does "no test runs it" mean the declaration is dead
# rather than that this board does not build it.
#
# <excused> names the LAYERS this tree was configured without, comma separated. A `build`
# declaration carrying one of them is excused BY NAME and counted, and every other unrun
# `build` declaration is refused.
#
# <complete> is NOT keyed on a knob: keying it on KICKOS_BUILD_UNIT_TESTS excuses the clause in
# exactly the configuration a box with no GTest produces, which is where the missing programs
# and their ctest arms are.
#
# A program marked with a layer this tree lacks is excused here; the PROVISIONED tree, where no
# layer is excused, is what refuses it.
#
# The classification is DECLARED, in tests/static/test_classes.txt, and cannot be derived:
# no property of a ctest command line separates the two sets. The gate maps each entry to
# the program it runs and REFUSES an entry whose program is undeclared, so a new test forces
# an explicit choice rather than inheriting one.
#
# SCOPE. This pins the DECLARATION against the labels, so a program declared with the wrong
# class is ratified. A gate that reads a clock WITHOUT running an image is `host` here and
# still unbatchable.

set -u
. "$(dirname "$0")/../lib/gate.sh"
# Findings accumulate over the whole suite, so set -e must stay off.

if [ "$#" -ne 7 ]; then
    fail "usage: check_test_labels.sh <ctest> <cmake> <build-dir> <src-dir> <declined:yes|no> <complete:yes|no> <excused-layers>"
fi
CTEST="$1"
CMAKE="$2"
# Taken verbatim from CMake, never re-resolved: the JSON spells these directories the way
# CMake does, and a `cd && pwd` here would resolve a symlink it did not.
BUILD="${3%/}"
SRC="${4%/}"
DECLINED="$5"
COMPLETE="$6"
EXCUSED="$7"

[ -x "$CTEST" ] || fail "no ctest at $CTEST"
[ -x "$CMAKE" ] || fail "no cmake at $CMAKE"
[ -d "$BUILD" ] || fail "no build directory at $BUILD"
case "$DECLINED" in
    yes | no) ;;
    *) fail "<declined> is 'yes' or 'no', not '$DECLINED'" ;;
esac
case "$COMPLETE" in
    yes | no) ;;
    *) fail "<complete> is 'yes' or 'no', not '$COMPLETE'" ;;
esac

# EVERY LAYER THIS GATE KNOWS, and the knob that removes each. A declaration may name one
# and CMake may excuse one; anything else is refused, so a renamed knob or a typo cannot
# quietly widen the excused set.
LAYERS="unit"

# Sets LAYER_WHY rather than printing it: a caller's `$(layer_why ...)` would confine the
# fail() below to a subshell, so a layer with no reason would print an empty one and pass.
layer_why() { # <layer>
    case "$1" in
        unit)
            LAYER_WHY="KICKOS_BUILD_UNIT_TESTS is OFF, so no GoogleTest host unit binary is built"
            ;;
        *)
            fail "no reason recorded for layer '$1'; write one beside its name in layer_why"
            ;;
    esac
}

known_layer() { # <layer>
    for _l in $LAYERS; do
        if [ "$_l" = "$1" ]; then
            return 0
        fi
    done
    return 1
}

# Every name in LAYERS resolves to a reason here rather than at the first excuse, which is a
# path a provisioned tree never takes.
for _l in $LAYERS; do
    layer_why "$_l"
done

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
: > "$TMP/layered.txt"
while read -r cls root rel layer extra; do
    [ -z "$extra" ] || fail "$DECL: trailing junk after '$cls $root $rel $layer': $extra"
    case "$root" in
        src | build) ;;
        *) fail "$DECL: '$root' is not a root; use src or build" ;;
    esac
    case "$rel" in
        "" | /* | *..*) fail "$DECL: '$rel' is not a path relative to $root" ;;
    esac
    if [ -n "$layer" ]; then
        known_layer "$layer" \
            || fail "$DECL: '$layer' is not a layer; this gate knows: $LAYERS"
        [ "$root" = build ] \
            || fail "$DECL: src/$rel carries layer '$layer'; a src path is pinned on every board"
        echo "$root $rel $layer" >> "$TMP/layered.txt"
    fi
    if [ "$root" = src ] && [ ! -f "$SRC/$rel" ]; then
        fail "$DECL declares src/$rel, which no longer exists"
    fi
    case "$cls" in
        host) echo "$root $rel" >> "$TMP/host.txt" ;;
        image) echo "$root $rel" >> "$TMP/image.txt" ;;
        *) fail "$DECL: '$cls' is not a class; use host or image" ;;
    esac
done < "$TMP/decl.txt"

# A layer no declaration claims is a name that was renamed on one side only, which would let
# CMake excuse a set that is empty and read as a clean sweep.
for _l in $LAYERS; do
    grep -q " $_l\$" "$TMP/layered.txt" \
        || fail "$DECL claims no program for layer '$_l', so excusing it would excuse nothing"
done

# The layers THIS tree was configured without.
: > "$TMP/excused_layers.txt"
_rest="$EXCUSED"
while [ -n "$_rest" ]; do
    case "$_rest" in
        *,*)
            _one="${_rest%%,*}"
            _rest="${_rest#*,}"
            ;;
        *)
            _one="$_rest"
            _rest=""
            ;;
    esac
    [ -n "$_one" ] || fail "<excused> holds an empty layer name: '$EXCUSED'"
    known_layer "$_one" || fail "<excused> names '$_one', which is not a layer; this gate knows: $LAYERS"
    echo "$_one" >> "$TMP/excused_layers.txt"
done
if [ "$COMPLETE" = no ] && [ -s "$TMP/excused_layers.txt" ]; then
    fail "<excused> names a layer on a <complete> no tree, where nothing is swept and so nothing can be excused"
fi

cat "$TMP/host.txt" "$TMP/image.txt" > "$TMP/all.txt"
DUP="$(sort "$TMP/all.txt" | uniq -d)"
[ -z "$DUP" ] || fail "$DECL declares a program twice: $DUP"

# --- one pass over the suite --------------------------------------------------
: > "$TMP/findings.txt"
: > "$TMP/seen.txt"
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
            report "$name: ctest reports no command, so its program cannot be classified; build the tree first"
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
    echo "$root $rel" >> "$TMP/seen.txt"

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
        report "$name: runs $root/$rel, which $DECL does not classify; add a host or image line for it"
    fi
done < "$TMP/table"

# A suite the gate could not read leaves every assertion above unexecuted.
[ "$N_TOTAL" -gt 0 ] || fail "read zero tests out of $BUILD"
[ "$SAW_FIXTURE" = 1 ] || fail "no $FIXTURE fixture in this suite, so every test here may have run a stale binary"
[ "$N_HOST" -gt 0 ] || fail "not one host test in this suite; the source-tree gates register on every board, so the mapping is broken"

# --- the other direction, on the one tree that can read it --------------------
# A `src` declaration is pinned to a file above and rots visibly on every board. A `build`
# one names a program only some configurations produce, so an unrun declaration is a
# finding only where <complete> says the whole set was expected.
#
# A declaration whose LAYER this tree lacks is excused and named below. It is not a way out
# of the sweep: the excuse is per declaration and per layer, so a program missing for any
# other reason is still a finding, and a program the excused layer owns that registered
# ANYWAY is one too.
N_EXCUSED=0
: > "$TMP/excused.txt"
if [ "$COMPLETE" = yes ]; then
    while read -r root rel; do
        [ "$root" = build ] || continue
        _layer="$(awk -v r="$rel" '$1 == "build" && $2 == r { print $3 }' "$TMP/layered.txt")"
        _off=no
        if [ -n "$_layer" ] && grep -qxF "$_layer" "$TMP/excused_layers.txt"; then
            _off=yes
        fi
        if grep -qxF "build $rel" "$TMP/seen.txt"; then
            if [ "$_off" = yes ]; then
                report "$DECL declares build/$rel behind the $_layer layer, which this tree does not have, yet a test runs it"
            fi
            continue
        fi
        if [ "$_off" = yes ]; then
            N_EXCUSED=$((N_EXCUSED + 1))
            printf '%s\t%s\n' "$_layer" "$rel" >> "$TMP/excused.txt"
            continue
        fi
        report "$DECL declares build/$rel, which no test in this tree runs"
    done < "$TMP/all.txt"
fi

echo "== $N_TOTAL test(s): $N_HOST host, $N_IMAGE image, 1 build fixture; $(wc -l < "$TMP/decl.txt" | tr -d ' ') program(s) declared, integration declined: $DECLINED, build set complete: $COMPLETE =="

# THE COUNT AND EVERY NAME, on stdout, because the whole point of excusing rather than
# skipping is that an absent program is loud. Each of these is a GoogleTest binary
# registering one ctest entry per case, so the arms they carry are absent from this suite too.
if [ "$N_EXCUSED" -gt 0 ]; then
    echo "== $N_EXCUSED build declaration(s) excused, none of them run by this suite =="
    for _l in $(cut -f1 "$TMP/excused.txt" | sort -u); do
        layer_why "$_l"
        echo "==   layer $_l: $LAYER_WHY =="
        awk -F"$TAB" -v l="$_l" '$1 == l { print "==     " $2 " ==" }' "$TMP/excused.txt"
    done
fi

if [ -s "$TMP/findings.txt" ]; then
    cat "$TMP/findings.txt" >&2
    echo "" >&2
    echo "FAIL: $(wc -l < "$TMP/findings.txt" | tr -d ' ') finding(s) against $DECL." >&2
    echo "      Fix the label, declare the program's class, or drop a declaration nothing runs." >&2
    echo "      Every direction here is silent without this gate." >&2
    exit 1
fi

echo "PASS: every registered test carries the label its declared class requires"
