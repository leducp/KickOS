#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Configures every visible configure preset and runs `ctest -L host` on each.
#
# An OPERATOR TOOL and deliberately not a gate: it configures and builds one tree per visible
# configure preset and needs all four cross toolchain families on the box. A ctest entry
# doing that would invoke ctest from inside ctest, once per preset, each copy sweeping the
# whole fleet. tests/static/ is for checks that are cheap, build nothing and run everywhere.
#
# WHY -L host AND NOTHING ELSE. `-L host` is the batchable set by design: those tests execute
# no KickOS image, read no clock and cannot be perturbed by load. `-LE host` must run
# STANDALONE, and batching it produces load-dependent failures that are an invalid instrument
# rather than a finding. This tool never runs that half.
#
# Usage:
#   source .session/env.sh        # or whatever puts your cross toolchains in the env
#   tools/sweep_host_gates.sh                 # every visible configure preset
#   tools/sweep_host_gates.sh sim sim-telem microbit
#
# Env:
#   SWEEP_OUT=<dir>       build trees, logs and the summary  (default /var/tmp/kickos-hostsweep)
#   SWEEP_JOBS=<n>        build parallelism WITHIN one preset (default 4)
#   SWEEP_PAR=<n>         presets at once                     (default nproc / SWEEP_JOBS)
#   SWEEP_GTEST_PREFIX    CMAKE_PREFIX_PATH for GTest        (default /var/tmp/kickos-conan)
#                         `-` disables it, which SHRINKS the sim suite; see below.
#   SWEEP_FORCE=1         redo presets a previous run already passed
#
# The sentinel is written only when every selected preset reached a verdict, at least one of
# them ran, and the fleet total of host tests is above zero. An empty selection, a lost
# worker and a run that reused every line without building are all refusals.
#
# THE RECORDED STATUS BELONGS TO A TREE. A previous run's PASS is reused only when the source
# tree is byte for byte the one it was recorded against. A directory named for a branch is
# not a tree.
#
# THE GTEST PREFIX IS NOT OPTIONAL ON THE SIM. find_package(GTest) is reached on the sim arch
# and nowhere else: without the prefix the sim registers a fraction of its tests, every host
# unit arm is silently ABSENT, and the suite still reports 100% pass. The tool refuses a
# prefix that is not on disk.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)" || exit 1
OUT="${SWEEP_OUT:-/var/tmp/kickos-hostsweep}"
JOBS="${SWEEP_JOBS:-4}"
GPREFIX="${SWEEP_GTEST_PREFIX:-/var/tmp/kickos-conan}"
FORCE="${SWEEP_FORCE:-0}"

# PRESETS AT ONCE. Sized so SWEEP_PAR x SWEEP_JOBS is the core count. Only the BUILD is
# parallel here; `ctest -L host` is the batchable set by design (see the header). The
# `-LE host` half is not run by this tool at all and must never be batched.
NCPU="$(nproc 2>/dev/null || echo 4)"
PAR="${SWEEP_PAR:-$(( NCPU / JOBS ))}"
[ "$PAR" -lt 1 ] && PAR=1

SUMMARY="$OUT/summary.txt"
SENTINEL="$OUT/DONE"

die() { echo "FAIL: $*" >&2; exit 1; }

# ctest_tail <log>
# Sets T_TOTAL and T_FAILED from ctest's "N% tests passed, F tests failed out of T" line, or
# leaves T_TOTAL empty when there is no such line to read.
ctest_tail() {
    T_TOTAL="$(sed -n 's/.*tests* failed out of \([0-9][0-9]*\).*/\1/p' "$1" | tail -n1)"
    T_FAILED="$(sed -n 's/.*, \([0-9][0-9]*\) tests* failed out of.*/\1/p' "$1" | tail -n1)"
}

# The two numbers every verdict below rests on are proved on planted lines before any log is
# read: a parse that has never come back with the right figures says nothing when it comes
# back empty.
control() { # <expected total> <expected failed> <planted ctest tail>
    _want_t="$1"
    _want_f="$2"
    shift 2
    _c="$(mktemp)" || die "mktemp failed"
    printf '%s\n' "$*" > "$_c"
    ctest_tail "$_c"
    rm -f "$_c"
    [ "${T_TOTAL:-none}" = "$_want_t" ] && [ "${T_FAILED:-none}" = "$_want_f" ] \
        || die "the ctest tail parse answered total '${T_TOTAL:-}' failed '${T_FAILED:-}' where
      it must answer '$_want_t' and '$_want_f', for: $*
      Every count below it would be read from a line it cannot parse."
}
control 96 0    "100% tests passed, 0 tests failed out of 96"
control 96 3    "96% tests passed, 3 tests failed out of 96"
control 96 1    "98% tests passed, 1 test failed out of 96"
control none none "Total Tests: 96"

command -v cmake >/dev/null 2>&1 || die "cmake not found"
command -v ctest >/dev/null 2>&1 || die "ctest not found"
[ -f "$ROOT/CMakePresets.json" ] || die "no CMakePresets.json under $ROOT"

mkdir -p "$OUT/logs" "$OUT/status" || die "cannot create $OUT"

# FIRST, before anything can be read as evidence. A sentinel a re-run does not clear reads as
# "done" the instant the re-run starts.
rm -f "$SENTINEL"

PREFIX_ARG=""
if [ "$GPREFIX" != "-" ]; then
    [ -d "$GPREFIX" ] || die "SWEEP_GTEST_PREFIX=$GPREFIX is not a directory. Regenerate it
      (conan install $ROOT/conan/conanfile.py --output-folder=$GPREFIX -s compiler.cppstd=20 --build=missing),
      point SWEEP_GTEST_PREFIX at another one, or pass SWEEP_GTEST_PREFIX=- to sweep the
      sim with its host unit tests OFF and know that is what you measured."
    PREFIX_ARG="-DCMAKE_PREFIX_PATH=$GPREFIX"
fi

# ONE PRESET, in its own process, writing only its own status file. That is what lets the
# loop below fan out: nothing here is shared but the log and status directories, and each
# preset owns one entry in each. The counts are totted up afterwards FROM those files, so a
# preset that dies without writing one is missing rather than silently counted as a pass.
sweep_one() {
    p="$1"
    ST="$OUT/status/$p"
    LOG="$OUT/logs/$p.log"
    DIR="$OUT/trees/$p"

    # Reused only when the recorded line still names its own test count: a line that does
    # not is not readable evidence.
    if [ "$FORCE" != 1 ] && [ -f "$ST" ] \
            && sed -n 's/^PASS  *[^ ][^ ]*  *[0-9][0-9]* host test(s).*/y/p' "$ST" \
               | grep -q y; then
        : > "$OUT/status/$p.reused"
        return 0
    fi
    rm -f "$OUT/status/$p.reused"

    echo "=== $p ===" >&2
    : > "$LOG"
    STAGE=""
    # -S "$ROOT" IS LOAD-BEARING: `cmake --preset` resolves CMakePresets.json against the
    # CURRENT DIRECTORY, and nothing here ever changes it. Invoked from another checkout this
    # configured 59 of 60 presets against THAT tree while every result was stamped against
    # $ROOT, and the only preset that failed was the one this tree alone carries, so the
    # sweep read as a clean pass over a tree it never compiled. THE TREE STAMP IS NOT
    # PROTECTION: it is taken from $ROOT by `git -C`, so it agrees with itself no matter which
    # sources cmake actually read.
    if ! cmake -S "$ROOT" --preset "$p" -B "$DIR" $PREFIX_ARG >> "$LOG" 2>&1; then
        STAGE=configure
    elif ! cmake --build "$DIR" "-j$JOBS" >> "$LOG" 2>&1; then
        STAGE=build
    fi

    if [ -n "$STAGE" ]; then
        printf 'FAIL    %-22s %s failed, see logs/%s.log\n' "$p" "$STAGE" "$p" > "$ST"
        return 0
    fi

    ctest --test-dir "$DIR" -L host --output-on-failure >> "$LOG" 2>&1
    RC=$?
    # A run whose total cannot be read is refused: a suite that registered nothing passes
    # 100% of nothing.
    ctest_tail "$LOG"
    TOTAL="$T_TOTAL"
    FAILED="$T_FAILED"
    if [ -z "$TOTAL" ]; then
        printf 'FAIL    %-22s ctest printed no total, see logs/%s.log\n' "$p" "$p" > "$ST"
    elif [ "$TOTAL" -eq 0 ]; then
        printf 'FAIL    %-22s registered ZERO host tests\n' "$p" > "$ST"
    elif [ "$RC" -ne 0 ]; then
        printf 'FAIL    %-22s %s of %s host test(s) failed, see logs/%s.log\n' \
            "$p" "$FAILED" "$TOTAL" "$p" > "$ST"
    else
        printf 'PASS    %-22s %s host test(s)\n' "$p" "$TOTAL" > "$ST"
    fi
}

# Re-entry point for the fan-out below. Kept as a hidden argument rather than an exported
# shell function because this is /bin/sh, which does not export functions.
if [ "${1:-}" = "--sweep-one" ]; then
    shift
    sweep_one "$1"
    exit 0
fi

# What the recorded status is evidence ABOUT. Without git the identity is unique to this run,
# so nothing is ever reused. Below the re-entry point above, so the workers do not each pay
# for it.
TREE_ID="$(git -C "$ROOT" rev-parse HEAD 2>/dev/null)"
if [ -n "$TREE_ID" ]; then
    TREE_ID="$TREE_ID $(git -C "$ROOT" status --porcelain 2>/dev/null | cksum)"
else
    TREE_ID="no git under $ROOT, run $$ at $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
fi
STAMP="$OUT/tree.stamp"
if [ -f "$STAMP" ] && [ "$(cat "$STAMP")" != "$TREE_ID" ]; then
    echo "note: $OUT holds status recorded against another tree; discarding it" >&2
    rm -rf "$OUT/status" || die "cannot clear $OUT/status"
    mkdir -p "$OUT/status" || die "cannot create $OUT/status"
fi
printf '%s\n' "$TREE_ID" > "$STAMP"

# The preset list comes from the same flattener the service-list gate uses.
PRESETS_TSV="$OUT/presets.tsv"
cmake "-DSRC=$ROOT" "-DOUT=$PRESETS_TSV" -P "$ROOT/tests/static/preset_boards.cmake" >/dev/null \
    || die "could not read the configure presets"
[ -s "$PRESETS_TSV" ] || die "the preset flattener produced no table"

if [ "$#" -gt 0 ]; then
    LIST="$*"
else
    LIST="$(cut -f1 "$PRESETS_TSV" | tr '\n' ' ')"
fi

N_LIST=0
for p in $LIST; do
    cut -f1 "$PRESETS_TSV" | grep -qxF "$p" || die "'$p' is not a visible configure preset"
    N_LIST=$((N_LIST + 1))
done
# Handed nothing, this tool would sweep nothing and report nothing wrong.
[ "$N_LIST" -gt 0 ] || die "the selection is empty, so there is no corpus to sweep. A run
      over zero presets has no host gate to fail and would report clean over nothing."

N_TOTAL=0
N_PASS=0
N_FAIL=0
N_REUSED=0
N_TESTS=0

{
    echo "== ctest -L host over every named configure preset =="
    echo "root    $ROOT"
    echo "out     $OUT"
    echo "gtest   $GPREFIX"
    echo "started $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo ""
} > "$SUMMARY"

printf '%s\n' $LIST | xargs -P "$PAR" -n1 "$0" --sweep-one

# The counts come from the status files and not from the workers, which ran in their own
# processes. A preset whose file is ABSENT is reported as such.
for p in $LIST; do
    N_TOTAL=$((N_TOTAL + 1))
    ST="$OUT/status/$p"
    if [ ! -f "$ST" ]; then
        printf 'FAIL    %-22s no status written, the worker died\n' "$p" >> "$SUMMARY"
        N_FAIL=$((N_FAIL + 1))
        continue
    fi
    if [ -f "$OUT/status/$p.reused" ]; then
        RGATES="$(sed -n 's/^PASS  *[^ ][^ ]*  *\([0-9][0-9]*\) host test(s).*/\1/p' "$ST")"
        if [ -n "$RGATES" ]; then
            N_REUSED=$((N_REUSED + 1))
            N_PASS=$((N_PASS + 1))
            N_TESTS=$((N_TESTS + RGATES))
            # Reprinted from the recorded status, never re-asserted.
            printf 'REUSED  %s\n' "$(cat "$ST")" >> "$SUMMARY"
            continue
        fi
        printf 'FAIL    %-22s a reused status that names no test count is not evidence\n' \
            "$p" >> "$SUMMARY"
        N_FAIL=$((N_FAIL + 1))
        continue
    fi
    PGATES="$(sed -n 's/^PASS  *[^ ][^ ]*  *\([0-9][0-9]*\) host test(s).*/\1/p' "$ST")"
    if [ -n "$PGATES" ]; then
        N_PASS=$((N_PASS + 1))
        N_TESTS=$((N_TESTS + PGATES))
    else
        N_FAIL=$((N_FAIL + 1))
    fi
    cat "$ST" >> "$SUMMARY"
done

# Appended by the run itself and nowhere else, so a summary cut short by a crash, a full
# disk or a killed shell is VISIBLY short rather than plausibly complete. Grep for it.
{
    echo ""
    echo "DONE $N_TOTAL preset(s), $N_TESTS host test(s): $N_PASS pass ($N_REUSED reused), $N_FAIL fail; -LE host not run"
    echo "finished $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
} >> "$SUMMARY"

# Every clause below is something this run has to have DONE.
WHY=""
refuse() { WHY="$WHY
      $*"; }

[ "$N_TOTAL" -eq "$N_LIST" ] \
    || refuse "$N_TOTAL of the $N_LIST selected preset(s) were reached"
[ "$N_FAIL" -eq 0 ] \
    || refuse "$N_FAIL preset(s) failed"
[ "$N_PASS" -gt 0 ] \
    || refuse "no preset passed its host gates"
[ "$N_TESTS" -gt 0 ] \
    || refuse "zero host tests ran across the whole selection"

if [ -z "$WHY" ]; then
    # The only writer of this file, and it carries what it asserts.
    {
        echo "tree     $TREE_ID"
        echo "selected $N_LIST preset(s)"
        echo "ran      $N_PASS preset(s) passed their host gates ($N_REUSED reused)"
        echo "tests    $N_TESTS host test(s), 0 failed"
        echo "finished $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    } > "$SENTINEL"
    tail -n 3 "$SUMMARY"
    exit 0
fi

{
    echo "REFUSED: this sweep is not a witness for the selection it was handed:$WHY"
    echo "      no DONE sentinel written under $OUT"
} >> "$SUMMARY"
tail -n 8 "$SUMMARY" >&2
exit 1
