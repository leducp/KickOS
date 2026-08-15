#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Configures every visible configure preset and runs `ctest -L host` on each.
#
# An OPERATOR TOOL and deliberately not a gate. It configures and builds 51 trees, needs
# all four cross toolchain families on the box, and runs for hours; a ctest entry doing
# that would invoke ctest from inside ctest, and since the source-tree gates register on
# every board it would be registered 51 times, each copy sweeping the whole fleet.
# tests/static/ is for checks that are cheap, build nothing and can run everywhere.
#
# WHY -L host AND NOTHING ELSE. `-L host` is the batchable set BY DESIGN: those tests
# execute no KickOS image, read no clock and cannot be perturbed by load. `-LE host` is
# the set that must run STANDALONE, the sim and qemu suites having no silicon clock, and
# batching it produces load-dependent failures that are an invalid instrument rather than
# a finding. This tool never runs that half, so a green sweep says nothing about it.
#
# WHAT IT IS FOR. Every K-seam gate failed to LINK under `sim-telem` for two milestones
# because nothing routinely ran that preset. A preset nobody configures is a preset
# nobody checks, and its breakage is indistinguishable from its passing.
#
# Usage:
#   source .session/env.sh        # or whatever puts your cross toolchains in the env
#   tools/sweep_host_gates.sh                 # every visible configure preset
#   tools/sweep_host_gates.sh sim sim-telem microbit
#
# Env:
#   SWEEP_OUT=<dir>       build trees, logs and the summary  (default /var/tmp/kickos-hostsweep)
#   SWEEP_JOBS=<n>        build parallelism                  (default 8)
#   SWEEP_GTEST_PREFIX    CMAKE_PREFIX_PATH for GTest        (default /var/tmp/kickos-conan)
#                         `-` disables it, which SHRINKS the sim suite; see below.
#   SWEEP_FORCE=1         redo presets a previous run already passed
#
# THE GTEST PREFIX IS NOT OPTIONAL ON THE SIM. find_package(GTest) is reached on the sim
# arch and nowhere else, so the prefix is inert on the other 49 presets and load-bearing
# on two: without it the sim registers a fraction of its tests, every host unit arm is
# silently ABSENT, and the suite still reports 100% pass. The tool refuses a prefix that
# is not on disk rather than sweeping a suite with the arms cut out.
#
# THEREFORE NOT CAUGHT:
#   - the whole `-LE host` half. Every gate that boots an image, natively or under an
#     emulator, is outside this sweep by construction and stays that way.
#   - a preset builds and its host gates pass with the KICKOS_SERVICE_LIST the preset
#     defaults to. Nothing here selects an alternative provider, so a service list that
#     does not compile is invisible; tests/static/service_lists.txt names them and the
#     bench is what runs them.
#   - a board with no silicon here is compiled, not run.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)" || exit 1
OUT="${SWEEP_OUT:-/var/tmp/kickos-hostsweep}"
JOBS="${SWEEP_JOBS:-8}"
GPREFIX="${SWEEP_GTEST_PREFIX:-/var/tmp/kickos-conan}"
FORCE="${SWEEP_FORCE:-0}"

SUMMARY="$OUT/summary.txt"
SENTINEL="$OUT/DONE"

die() { echo "FAIL: $*" >&2; exit 1; }

command -v cmake >/dev/null 2>&1 || die "cmake not found"
command -v ctest >/dev/null 2>&1 || die "ctest not found"
[ -f "$ROOT/CMakePresets.json" ] || die "no CMakePresets.json under $ROOT"

mkdir -p "$OUT/logs" "$OUT/status" || die "cannot create $OUT"

# FIRST, before anything can be read as evidence. A sentinel a re-run does not clear
# reads as "done" the instant the re-run starts, and a stale one here has already cost a
# whole sweep in this project.
rm -f "$SENTINEL"

PREFIX_ARG=""
if [ "$GPREFIX" != "-" ]; then
    [ -d "$GPREFIX" ] || die "SWEEP_GTEST_PREFIX=$GPREFIX is not a directory. Regenerate it
      (conan install $ROOT/conan/conanfile.py --output-folder=$GPREFIX -s compiler.cppstd=20 --build=missing),
      point SWEEP_GTEST_PREFIX at another one, or pass SWEEP_GTEST_PREFIX=- to sweep the
      sim with its host unit tests OFF and know that is what you measured."
    PREFIX_ARG="-DCMAKE_PREFIX_PATH=$GPREFIX"
fi

# The preset list comes from the same flattener the service-list gate uses, so the two
# halves of this mechanism can never disagree about what a preset is.
PRESETS_TSV="$OUT/presets.tsv"
cmake "-DSRC=$ROOT" "-DOUT=$PRESETS_TSV" -P "$ROOT/tests/static/preset_boards.cmake" >/dev/null \
    || die "could not read the configure presets"
[ -s "$PRESETS_TSV" ] || die "the preset flattener produced no table"

if [ "$#" -gt 0 ]; then
    LIST="$*"
else
    LIST="$(cut -f1 "$PRESETS_TSV" | tr '\n' ' ')"
fi

for p in $LIST; do
    cut -f1 "$PRESETS_TSV" | grep -qxF "$p" || die "'$p' is not a visible configure preset"
done

N_TOTAL=0
N_PASS=0
N_FAIL=0
N_REUSED=0

{
    echo "== ctest -L host over every named configure preset =="
    echo "root    $ROOT"
    echo "out     $OUT"
    echo "gtest   $GPREFIX"
    echo "started $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo ""
} > "$SUMMARY"

for p in $LIST; do
    N_TOTAL=$((N_TOTAL + 1))
    ST="$OUT/status/$p"
    LOG="$OUT/logs/$p.log"
    DIR="$OUT/trees/$p"

    if [ "$FORCE" != 1 ] && [ -f "$ST" ] && grep -q '^PASS' "$ST"; then
        N_REUSED=$((N_REUSED + 1))
        N_PASS=$((N_PASS + 1))
        # Reprinted from the recorded status, never re-asserted: a reused line has to
        # stay distinguishable from one this run measured.
        printf 'REUSED  %s\n' "$(cat "$ST")" >> "$SUMMARY"
        continue
    fi

    echo "=== $p ===" >&2
    : > "$LOG"
    STAGE=""
    if ! cmake --preset "$p" -B "$DIR" $PREFIX_ARG >> "$LOG" 2>&1; then
        STAGE=configure
    elif ! cmake --build "$DIR" "-j$JOBS" >> "$LOG" 2>&1; then
        STAGE=build
    fi

    if [ -n "$STAGE" ]; then
        printf 'FAIL    %-22s %s failed, see logs/%s.log\n' "$p" "$STAGE" "$p" > "$ST"
        N_FAIL=$((N_FAIL + 1))
        cat "$ST" >> "$SUMMARY"
        continue
    fi

    ctest --test-dir "$DIR" -L host --output-on-failure >> "$LOG" 2>&1
    RC=$?
    # "N% tests passed, F tests failed out of T". A run whose total cannot be read is
    # refused: a suite that registered nothing passes 100% of nothing.
    TOTAL="$(sed -n 's/.*tests failed out of \([0-9][0-9]*\).*/\1/p' "$LOG" | tail -n1)"
    FAILED="$(sed -n 's/.*, \([0-9][0-9]*\) tests* failed out of.*/\1/p' "$LOG" | tail -n1)"
    if [ -z "$TOTAL" ]; then
        printf 'FAIL    %-22s ctest printed no total, see logs/%s.log\n' "$p" "$p" > "$ST"
        N_FAIL=$((N_FAIL + 1))
    elif [ "$TOTAL" -eq 0 ]; then
        printf 'FAIL    %-22s registered ZERO host tests\n' "$p" > "$ST"
        N_FAIL=$((N_FAIL + 1))
    elif [ "$RC" -ne 0 ]; then
        printf 'FAIL    %-22s %s of %s host test(s) failed, see logs/%s.log\n' \
            "$p" "$FAILED" "$TOTAL" "$p" > "$ST"
        N_FAIL=$((N_FAIL + 1))
    else
        printf 'PASS    %-22s %s host test(s)\n' "$p" "$TOTAL" > "$ST"
        N_PASS=$((N_PASS + 1))
    fi
    cat "$ST" >> "$SUMMARY"
done

# Appended by the run itself and nowhere else, so a summary cut short by a crash, a full
# disk or a killed shell is VISIBLY short rather than plausibly complete. Grep for it.
{
    echo ""
    echo "DONE $N_TOTAL preset(s): $N_PASS pass ($N_REUSED reused), $N_FAIL fail; -LE host not run"
    echo "finished $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
} >> "$SUMMARY"

if [ "$N_FAIL" -eq 0 ]; then
    : > "$SENTINEL"
fi

tail -n 3 "$SUMMARY"
[ "$N_FAIL" -eq 0 ]
