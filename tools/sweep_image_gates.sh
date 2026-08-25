#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Configures every visible configure preset and runs `ctest -LE host` on each preset that
# registers one, ONE PRESET AT A TIME. The other half of tools/sweep_host_gates.sh, whose
# header says of this set: "This tool never runs that half, so a green sweep says nothing
# about it."
#
# An OPERATOR TOOL and deliberately not a gate, for the reasons the sibling gives: it
# configures the whole fleet, needs the cross toolchain families on the box, and a ctest
# entry doing that would invoke ctest from inside ctest on every board.
#
# WHAT SERIALISATION IS ENFORCED, AND WHY. `-LE host` is the set that boots a KickOS image,
# natively on the sim or under an emulator, so it reads a clock with no silicon behind it and
# a loaded box shifts the result. That makes a batched run an invalid instrument rather than
# a finding, which is why the sibling refuses this half outright. Enforced here:
#   - one preset at a time, in this single process, never a fan-out.
#   - `ctest -j1`, explicit rather than relying on the default, so no test in a suite runs
#     beside another test of that suite.
#   - a PID lock, so a second copy of this tool cannot run beside the first. It covers only
#     copies sharing SWEEP_OUT, and nothing here can see an unrelated build on the box.
# Build parallelism is kept: the build is not the measurement. Only the ctest step is timed
# by the tests themselves.
#
# THE CENSUS, AND WHY IT IS TAKEN TWICE. Which tests a preset registers is decided at
# CONFIGURE time, so `ctest -N -LE host` on a configured tree already reports a count and a
# preset with no image gate can be answered without building it. That is what makes the
# whole visible preset set affordable. But an UNBUILT tree does not report the same set as a
# built one: gtest_discover_tests writes its add_test calls at BUILD time, and until then
# CMake's GoogleTest module stands in one `<target>_NOT_BUILT` entry per target which does
# NOT inherit the `LABELS host` the real cases carry, so on an unbuilt sim tree one lands in
# `-LE host` for every GoogleTest target in the build. So:
#   - the pre-build census discounts the build fixture and any `*_NOT_BUILT` placeholder, and
#     decides only whether there is anything here to build.
#   - the count REPORTED is re-taken after the build, where no placeholder exists and the set
#     is the one ctest will run. A disagreement between the two is printed, so a placeholder
#     spelled some other way surfaces instead of inflating a count.
# A placeholder that survives the build is a GoogleTest target that did not build, and is
# reported as a failure rather than counted.
#
# EMPTY IS NOT PASS. `ctest` exits 0 when its filter selects nothing, so a preset with no
# image gate would otherwise read exactly like a preset whose image gates all passed. Worse
# than plain zero: `kickos_build` is deliberately unlabelled so the build fixture joins BOTH
# partitions, so `-LE host` on a silicon preset selects ONE test, and that one is a build.
# A preset whose selection is empty once the fixture is discounted is reported EMPTY.
#
# COUNTS, NOT PERCENTAGES. Every line carries the number of gates registered, run, failed
# and skipped. A suite that registers a fraction of its arms still passes 100% of what it
# registered, so a percentage cannot express the failure this tool exists to catch. The run's
# numbers are read from the junit ctest writes, not from its "N% tests passed" line, because
# only the junit carries skipped and disabled as their own counts.
#
# THE EMULATOR IS NAMED, NOT ASSUMED. A missing qemu-system is SKIP_RETURN_CODE 77 at the
# test, so ctest reports Skipped and exits 0: an emulator-less box greens this sweep while
# booting nothing. Presets whose board needs an emulator are therefore checked against the
# binary BEFORE the build and skipped BY NAME with the reason. The board set comes from the
# one list that declares it (KICKOS_QEMU_MPS2_BOARDS in user/apps/common/CMakeLists.txt);
# `microbit` and `qemu-riscv` are spelled at their call sites with no list to read, so they
# are named below. Any skip the run reports anyway is counted and shown, so a drift between
# that map and cmake/kickos.cmake surfaces as a skip this tool did not predict.
#
# THE GTEST PREFIX IS INERT ON THIS HALF, AND ACCEPTED ANYWAY. On the sibling the prefix is
# load-bearing, because without it the sim silently registers a fraction of its HOST tests.
# Every GoogleTest case carries `LABELS host`, so `-LE host` excludes all of them and no image
# gate depends on KICKOS_BUILD_UNIT_TESTS. The refusal of a prefix that is not on disk is kept
# so both tools take the same environment and produce comparable trees, but a run with
# SWEEP_GTEST_PREFIX=- measures the same image set as a run with it. What the prefix does
# change here is the placeholder count above, which is why that is discounted by name.
#
# Usage:
#   source .session/env.sh        # or whatever puts your cross toolchains in the env
#   tools/sweep_image_gates.sh                 # every visible configure preset
#   tools/sweep_image_gates.sh sim qemu qemu-riscv
#
# Env:
#   SWEEP_OUT=<dir>       build trees, logs and the summary  (default /var/tmp/kickos-imagesweep)
#   SWEEP_JOBS=<n>        BUILD parallelism only             (default 8)
#   SWEEP_GTEST_PREFIX    CMAKE_PREFIX_PATH for GTest        (default /var/tmp/kickos-conan)
#                         `-` disables it; inert on this half, see above.
#   SWEEP_FORCE=1         redo presets a previous run already passed
#
# THEREFORE NOT CAUGHT:
#   - a board with no silicon here and no emulator machine is configured, not run. Its image
#     gates are not registered at all, so this reports EMPTY for it and asserts nothing about
#     the board. Only the bench does.
#   - the emulator is not the chip. A gate green under qemu says the image boots on qemu.
#   - a preset builds and its image gates pass with the KICKOS_SERVICE_LIST the preset
#     defaults to. Nothing here selects an alternative provider.
#   - a load-dependent gate that passes when run alone still passes here. Serialising removes
#     the instrument's own noise; it does not prove a gate is load-independent, and a gate
#     that fails only under load is invisible to both halves of the sweep.
#   - a flaky gate is not distinguished from a broken one. Nothing is re-run.
#   - the source tree is read afresh by every preset, so a tree edited mid-run gives verdicts
#     that belong to DIFFERENT tree states, and nothing here snapshots or checks that. Docs
#     are the exception rather than the rule on this half: no image gate reads them, while
#     `oot_export` and the published-console gates configure a child tree and so do read the
#     CMake, Kconfig and linker files live.
#   - the lock sees only other copies of this tool under the same SWEEP_OUT. Any other heavy
#     job on the box perturbs these suites and nothing here can detect it.
#   - `ctest` reruns the build fixture inside the suite, so every line counts one build
#     alongside the gates. It is shown separately and never folded into the gate count.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)" || exit 1
OUT="${SWEEP_OUT:-/var/tmp/kickos-imagesweep}"
JOBS="${SWEEP_JOBS:-8}"
GPREFIX="${SWEEP_GTEST_PREFIX:-/var/tmp/kickos-conan}"
FORCE="${SWEEP_FORCE:-0}"

SUMMARY="$OUT/summary.txt"
SENTINEL="$OUT/DONE"
LOCK="$OUT/RUNNING"
FIXTURE=kickos_build

# KICKOS_QEMU_MPS2_BOARDS is read from its declaration; the other two emulator boards are
# spelled at their kickos_add_qemu_test call sites and have no list to read.
MPS2_SRC="$ROOT/user/apps/common/CMakeLists.txt"

die() { echo "FAIL: $*" >&2; exit 1; }

command -v cmake >/dev/null 2>&1 || die "cmake not found"
command -v ctest >/dev/null 2>&1 || die "ctest not found"
[ -f "$ROOT/CMakePresets.json" ] || die "no CMakePresets.json under $ROOT"
[ -f "$MPS2_SRC" ] || die "no $MPS2_SRC to read the MPS2 board set from"

# Seven sim image gates re-configure THIS tree in a child cmake, which needs an interpreter
# that can import kconfiglib, resolved the way cmake/kconfig.cmake resolves it. Without one
# they fail on "kconfiglib is not importable" and read as code regressions. Refused here
# rather than reported as seven findings. `oot_export` spawns a child configure too, but of
# `examples/oot-app` against the installed package, which runs no Kconfig.
KPY="${KICKOS_KCONFIG_PY:-}"
if [ -z "$KPY" ]; then
    KPY="$(command -v python3 2>/dev/null)" || KPY=""
fi
[ -n "$KPY" ] || die "no python3 and no KICKOS_KCONFIG_PY; every gate that spawns a child
      configure would fail on the interpreter. source .session/env.sh."
"$KPY" -c "import kconfiglib" >/dev/null 2>&1 \
    || die "$KPY cannot import kconfiglib, so every gate that spawns a child cmake configure
      fails on that and not on the code. source .session/env.sh (it exports
      KICKOS_KCONFIG_PY) and re-run."

mkdir -p "$OUT/logs" "$OUT/status" "$OUT/census" "$OUT/junit" || die "cannot create $OUT"

# FIRST, before anything can be read as evidence. A sentinel a re-run does not clear reads as
# "done" the instant the re-run starts, and a stale one here has already cost a whole sweep in
# this project.
rm -f "$SENTINEL"

if [ -f "$LOCK" ]; then
    OTHER="$(cat "$LOCK" 2>/dev/null)"
    if [ -n "$OTHER" ] && kill -0 "$OTHER" 2>/dev/null; then
        die "pid $OTHER is already sweeping $OUT. Two copies would batch these suites, which
      is the one thing this tool exists not to do. Wait for it, or point SWEEP_OUT elsewhere."
    fi
fi
echo "$$" > "$LOCK"
trap 'rm -f "$LOCK"' EXIT
trap 'rm -f "$LOCK"; exit 130' INT
trap 'rm -f "$LOCK"; exit 143' TERM

PREFIX_ARG=""
if [ "$GPREFIX" != "-" ]; then
    [ -d "$GPREFIX" ] || die "SWEEP_GTEST_PREFIX=$GPREFIX is not a directory. Regenerate it
      (conan install $ROOT/conan/conanfile.py --output-folder=$GPREFIX -s compiler.cppstd=20 --build=missing),
      point SWEEP_GTEST_PREFIX at another one, or pass SWEEP_GTEST_PREFIX=- (inert on this half)."
    PREFIX_ARG="-DCMAKE_PREFIX_PATH=$GPREFIX"
fi

# Same flattener the sibling and the service-list gate use, so the three cannot disagree about
# what a preset is or which board it resolves to.
PRESETS_TSV="$OUT/presets.tsv"
cmake "-DSRC=$ROOT" "-DOUT=$PRESETS_TSV" -P "$ROOT/tests/static/preset_boards.cmake" >/dev/null \
    || die "could not read the configure presets"
[ -s "$PRESETS_TSV" ] || die "the preset flattener produced no table"

MPS2_BOARDS="$(sed -n 's/^set(KICKOS_QEMU_MPS2_BOARDS \([^)]*\))$/\1/p' "$MPS2_SRC")"
[ -n "$MPS2_BOARDS" ] || die "could not read KICKOS_QEMU_MPS2_BOARDS from $MPS2_SRC; the
      emulator map would silently claim no board needs qemu-system-arm"

# Echoes the emulator binary a board needs, or nothing when the board boots natively.
emulator_for() {
    for _b in $MPS2_BOARDS microbit; do
        if [ "$1" = "$_b" ]; then
            echo qemu-system-arm
            return 0
        fi
    done
    if [ "$1" = qemu-riscv ]; then
        echo qemu-system-riscv32
    fi
    if [ "$1" = qemu-arm64 ]; then
        echo qemu-system-aarch64
    fi
}

# census <preset> <tree> <tag>
# Sets C_SEL, C_IMAGE, C_DISABLED, C_PLACE, C_FIXTURE from `ctest -LE host` on that tree, or
# returns non-zero with C_WHY set. C_SEL is ctest's own selection count, which is the one
# number that cannot be confused with a pass.
census() {
    _p="$1"
    _dir="$2"
    _tag="$3"
    C_SEL=0
    C_IMAGE=0
    C_DISABLED=0
    C_PLACE=0
    C_FIXTURE=0
    C_WHY=""
    _n="$OUT/census/$_p.$_tag.count"
    ctest --test-dir "$_dir" -N -LE host > "$_n" 2>> "$LOG"
    C_SEL="$(sed -n 's/^Total Tests: \([0-9][0-9]*\)$/\1/p' "$_n" | tail -n1)"
    if [ -z "$C_SEL" ]; then
        C_WHY="ctest printed no test count"
        return 1
    fi
    if [ "$C_SEL" -eq 0 ]; then
        return 0
    fi
    _j="$OUT/census/$_p.$_tag.json"
    _t="$OUT/census/$_p.$_tag.tsv"
    ctest --test-dir "$_dir" --show-only=json-v1 -LE host > "$_j" 2>> "$LOG"
    if ! cmake "-DJSON=$_j" "-DOUT=$_t" -P "$ROOT/tests/static/ctest_tests.cmake" \
            >> "$LOG" 2>&1; then
        C_WHY="could not read the $C_SEL selected test(s)"
        return 1
    fi
    while IFS="	" read -r _name _labels _disabled _prog; do
        if [ "$_name" = "$FIXTURE" ]; then
            C_FIXTURE=1
            continue
        fi
        case "$_name" in
            *_NOT_BUILT)
                C_PLACE=$((C_PLACE + 1))
                continue
                ;;
        esac
        C_IMAGE=$((C_IMAGE + 1))
        if [ "$_disabled" != 0 ]; then
            C_DISABLED=$((C_DISABLED + 1))
        fi
    done < "$_t"
    return 0
}

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
N_EMPTY=0
N_SKIP=0
N_PARTIAL=0
N_REUSED=0
N_GATES=0

{
    echo "== ctest -LE host, one preset at a time, over every named configure preset =="
    echo "root    $ROOT"
    echo "out     $OUT"
    echo "gtest   $GPREFIX (inert on this half)"
    echo "kconfig $KPY"
    echo "arm     $(command -v qemu-system-arm 2>/dev/null || echo ABSENT)"
    echo "riscv   $(command -v qemu-system-riscv32 2>/dev/null || echo ABSENT)"
    echo "started $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    echo ""
} > "$SUMMARY"

for p in $LIST; do
    N_TOTAL=$((N_TOTAL + 1))
    ST="$OUT/status/$p"
    LOG="$OUT/logs/$p.log"
    DIR="$OUT/trees/$p"
    JUNIT="$OUT/junit/$p.xml"
    BOARD="$(awk -F"\t" -v k="$p" '$1 == k { print $2 }' "$PRESETS_TSV")"

    if [ "$FORCE" != 1 ] && [ -f "$ST" ] && grep -q '^PASS' "$ST"; then
        N_REUSED=$((N_REUSED + 1))
        N_PASS=$((N_PASS + 1))
        # Reprinted from the recorded status, never re-asserted: a reused line has to stay
        # distinguishable from one this run measured.
        printf 'REUSED  %s\n' "$(cat "$ST")" >> "$SUMMARY"
        continue
    fi

    echo "=== $p ($BOARD) ===" >&2
    : > "$LOG"

    if ! cmake --preset "$p" -B "$DIR" $PREFIX_ARG >> "$LOG" 2>&1; then
        printf 'FAIL    %-22s configure failed, see logs/%s.log\n' "$p" "$p" > "$ST"
        N_FAIL=$((N_FAIL + 1))
        cat "$ST" >> "$SUMMARY"
        continue
    fi

    if ! census "$p" "$DIR" pre; then
        printf 'FAIL    %-22s %s, see logs/%s.log\n' "$p" "$C_WHY" "$p" > "$ST"
        N_FAIL=$((N_FAIL + 1))
        cat "$ST" >> "$SUMMARY"
        continue
    fi
    PRE_IMAGE="$C_IMAGE"
    PRE_PLACE="$C_PLACE"

    if [ "$PRE_IMAGE" -eq 0 ]; then
        WHY="-LE host selects nothing at all"
        if [ "$C_FIXTURE" = 1 ]; then
            WHY="only the build fixture is selected"
        fi
        if [ "$PRE_PLACE" -gt 0 ]; then
            WHY="$WHY, beside $PRE_PLACE unbuilt GoogleTest placeholder(s)"
        fi
        printf 'EMPTY   %-22s no image gate registered: %s\n' "$p" "$WHY" > "$ST"
        N_EMPTY=$((N_EMPTY + 1))
        cat "$ST" >> "$SUMMARY"
        continue
    fi

    EMU="$(emulator_for "$BOARD")"
    if [ -n "$EMU" ] && ! command -v "$EMU" >/dev/null 2>&1; then
        printf 'SKIP    %-22s %s not found; %s image gate(s) NOT run\n' \
            "$p" "$EMU" "$PRE_IMAGE" > "$ST"
        N_SKIP=$((N_SKIP + 1))
        cat "$ST" >> "$SUMMARY"
        continue
    fi
    if [ -z "$EMU" ] && [ "$BOARD" != sim ]; then
        printf 'SKIP    %-22s board %s has no emulator machine; %s image gate(s) need silicon\n' \
            "$p" "$BOARD" "$PRE_IMAGE" > "$ST"
        N_SKIP=$((N_SKIP + 1))
        cat "$ST" >> "$SUMMARY"
        continue
    fi

    if ! cmake --build "$DIR" "-j$JOBS" >> "$LOG" 2>&1; then
        printf 'FAIL    %-22s build failed with %s image gate(s) registered, see logs/%s.log\n' \
            "$p" "$PRE_IMAGE" "$p" > "$ST"
        N_FAIL=$((N_FAIL + 1))
        cat "$ST" >> "$SUMMARY"
        continue
    fi

    # The authoritative count: the placeholders are gone and this is the set ctest will run.
    if ! census "$p" "$DIR" post; then
        printf 'FAIL    %-22s %s after the build, see logs/%s.log\n' "$p" "$C_WHY" "$p" > "$ST"
        N_FAIL=$((N_FAIL + 1))
        cat "$ST" >> "$SUMMARY"
        continue
    fi
    N_SEL="$C_SEL"
    N_IMAGE="$C_IMAGE"
    N_DISABLED="$C_DISABLED"
    if [ "$C_PLACE" -gt 0 ]; then
        printf 'FAIL    %-22s %s GoogleTest placeholder(s) survived the build, see logs/%s.log\n' \
            "$p" "$C_PLACE" "$p" > "$ST"
        N_FAIL=$((N_FAIL + 1))
        cat "$ST" >> "$SUMMARY"
        continue
    fi
    N_GATES=$((N_GATES + N_IMAGE))

    NOTE=""
    if [ "$N_IMAGE" -ne "$PRE_IMAGE" ]; then
        NOTE="$NOTE; $PRE_IMAGE before the build, $N_IMAGE after"
    fi
    if [ "$N_DISABLED" -gt 0 ]; then
        NOTE="$NOTE; $N_DISABLED DISABLED"
    fi

    if [ "$N_DISABLED" -eq "$N_IMAGE" ]; then
        printf 'DECLINE %-22s all %s image gate(s) are DISABLED in this tree\n' \
            "$p" "$N_IMAGE" > "$ST"
        N_FAIL=$((N_FAIL + 1))
        cat "$ST" >> "$SUMMARY"
        continue
    fi

    rm -f "$JUNIT"
    ctest --test-dir "$DIR" -LE host -j1 --output-on-failure --output-junit "$JUNIT" \
        >> "$LOG" 2>&1
    RC=$?

    # ctest writes the testsuite element one ATTRIBUTE PER LINE, indented with a TAB, so both
    # have to become spaces before any attribute can be read.
    ATTRS="$(sed -n '/<testsuite/,/>/p' "$JUNIT" 2>/dev/null | head -n 40 | tr '\n\t' '  ')"
    RAN="$(echo "$ATTRS" | sed -n 's/.* tests="\([0-9][0-9]*\)".*/\1/p')"
    FAILED="$(echo "$ATTRS" | sed -n 's/.* failures="\([0-9][0-9]*\)".*/\1/p')"
    SKIPPED="$(echo "$ATTRS" | sed -n 's/.* skipped="\([0-9][0-9]*\)".*/\1/p')"
    if [ -z "$RAN" ] || [ -z "$FAILED" ] || [ -z "$SKIPPED" ]; then
        printf 'FAIL    %-22s ctest wrote no readable junit for %s image gate(s), see logs/%s.log\n' \
            "$p" "$N_IMAGE" "$p" > "$ST"
        N_FAIL=$((N_FAIL + 1))
        cat "$ST" >> "$SUMMARY"
        continue
    fi

    FIX=" + 0 build fixture"
    if [ "$C_FIXTURE" = 1 ]; then
        FIX=" + 1 build fixture"
    fi
    if [ "$RAN" -ne "$N_SEL" ]; then
        # The suite changed shape between the census and the run.
        NOTE="$NOTE; census selected $N_SEL, ctest ran $RAN"
    fi

    if [ "$FAILED" -gt 0 ]; then
        printf 'FAIL    %-22s %s image gate(s)%s: %s run, %s failed, %s skipped%s, see logs/%s.log\n' \
            "$p" "$N_IMAGE" "$FIX" "$RAN" "$FAILED" "$SKIPPED" "$NOTE" "$p" > "$ST"
        N_FAIL=$((N_FAIL + 1))
    elif [ "$SKIPPED" -gt 0 ]; then
        # Not a pass. A skip here is a gate that booted nothing, and the emulator check above
        # already cleared the one reason this tool can predict.
        printf 'PARTIAL %-22s %s image gate(s)%s: %s run, 0 failed, %s SKIPPED%s, see logs/%s.log\n' \
            "$p" "$N_IMAGE" "$FIX" "$RAN" "$SKIPPED" "$NOTE" "$p" > "$ST"
        N_PARTIAL=$((N_PARTIAL + 1))
    elif [ "$RC" -ne 0 ]; then
        printf 'FAIL    %-22s ctest exited %s with 0 junit failures over %s image gate(s)%s\n' \
            "$p" "$RC" "$N_IMAGE" "$FIX" > "$ST"
        N_FAIL=$((N_FAIL + 1))
    else
        printf 'PASS    %-22s %s image gate(s)%s: %s run, 0 failed, 0 skipped%s\n' \
            "$p" "$N_IMAGE" "$FIX" "$RAN" "$NOTE" > "$ST"
        N_PASS=$((N_PASS + 1))
    fi
    cat "$ST" >> "$SUMMARY"
done

# Appended by the run itself and nowhere else, so a summary cut short by a crash, a full disk
# or a killed shell is VISIBLY short rather than plausibly complete. Grep for it.
{
    echo ""
    echo "DONE $N_TOTAL preset(s), $N_GATES image gate(s) run: $N_PASS pass ($N_REUSED reused),"
    echo "     $N_PARTIAL partial, $N_FAIL fail, $N_SKIP skipped, $N_EMPTY with no image gate"
    echo "finished $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
} >> "$SUMMARY"

if [ "$N_FAIL" -eq 0 ] && [ "$N_PARTIAL" -eq 0 ]; then
    : > "$SENTINEL"
fi

tail -n 4 "$SUMMARY"
[ "$N_FAIL" -eq 0 ] && [ "$N_PARTIAL" -eq 0 ]
