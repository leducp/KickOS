#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Configures every visible configure preset and runs `ctest -LE host` on each preset that
# registers one, ONE PRESET AT A TIME. The other half of tools/sweep_host_gates.sh.
#
# An OPERATOR TOOL and deliberately not a gate, for the reason the sibling gives: a ctest
# entry doing this would invoke ctest from inside ctest on every board.
#
# SERIALISATION IS ENFORCED. `-LE host` boots a KickOS image, so it reads a clock with no
# silicon behind it and a loaded box shifts the result. Enforced here:
#   - one preset at a time, in this single process, never a fan-out.
#   - `ctest -j1`, explicit rather than relying on the default.
#   - a PID lock, so a second copy of this tool cannot run beside the first. It covers only
#     copies sharing SWEEP_OUT.
# Build parallelism is kept: only the ctest step is timed by the tests themselves.
#
# THE CENSUS IS TAKEN TWICE. Which tests a preset registers is decided at CONFIGURE time, so
# `ctest -N -LE host` on a configured tree answers a preset with no image gate without
# building it. But gtest_discover_tests writes its add_test calls at BUILD time, and until
# then CMake's GoogleTest module stands in one `<target>_NOT_BUILT` entry per target which
# does NOT carry `LABELS host`, so an unbuilt sim tree lands one in `-LE host` per target.
#   - the pre-build census discounts the build fixture and any `*_NOT_BUILT` placeholder.
#   - the count REPORTED is re-taken after the build. A disagreement between the two is
#     printed, so a placeholder spelled some other way surfaces instead of inflating a count.
# A placeholder that survives the build is a GoogleTest target that did not build.
#
# EMPTY IS NOT PASS. `ctest` exits 0 when its filter selects nothing, and `kickos_build` is
# deliberately unlabelled so the build fixture joins BOTH partitions: `-LE host` on a silicon
# preset selects ONE test, and that one is a build. A selection empty once the fixture is
# discounted is reported EMPTY.
#
# Every preset in the selection reaches one of three ends and is counted in it: it ran every
# test its post-build census selected, it registered no image gate, or it was not run at all.
# The last two refuse unless SWEEP_EXPECT_EMPTY and SWEEP_EXPECT_SKIP declare how many of
# each this selection holds, and the declared figures have to match EXACTLY.
#
# A SHORT RUN IS A REFUSAL. `ctest` running a different number of tests from the number the
# post-build census selected means the suite changed shape between the two. The classifier
# answers seven planted counts at startup before any verdict rests on it.
#
# The sentinel is written only when every clause above holds, it carries the figures it
# asserts, and nothing else writes it. Its absence and a non-zero exit are the same verdict.
#
# THE RECORDED STATUS BELONGS TO A TREE. A previous run's PASS is reused only when the source
# tree is the one it was recorded against and the recorded line still names its own gate
# count; a directory named for a branch is not a tree.
#
# Every line carries the number of gates registered, run, failed and skipped: a suite that
# registers a fraction of its arms still passes 100% of what it registered. The run's numbers
# come from the junit ctest writes, only that carrying skipped and disabled as their own.
#
# THE EMULATOR IS NAMED. A missing qemu-system is SKIP_RETURN_CODE 77 at the test, so ctest
# reports Skipped and exits 0: an emulator-less box would green this sweep while booting
# nothing. Presets whose board needs an emulator are checked against the binary BEFORE the
# build and skipped BY NAME. The board set comes from KICKOS_QEMU_MPS2_BOARDS in
# user/apps/common/CMakeLists.txt; `microbit` and `qemu-riscv` are named below. Any skip the
# run reports anyway is counted and shown.
#
# The GTest prefix is inert on this half, every GoogleTest case carrying `LABELS host`. The
# refusal of a prefix that is not on disk is kept so both tools take the same environment.
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
#   SWEEP_EXPECT_EMPTY=<n>  presets in this selection with no image gate   (default 0)
#   SWEEP_EXPECT_SKIP=<n>   presets in this selection that cannot be run   (default 0)
#
#     alongside the gates. It is shown separately and never folded into the gate count.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)" || exit 1
OUT="${SWEEP_OUT:-/var/tmp/kickos-imagesweep}"
JOBS="${SWEEP_JOBS:-8}"
GPREFIX="${SWEEP_GTEST_PREFIX:-/var/tmp/kickos-conan}"
FORCE="${SWEEP_FORCE:-0}"
EXPECT_EMPTY="${SWEEP_EXPECT_EMPTY:-0}"
EXPECT_SKIP="${SWEEP_EXPECT_SKIP:-0}"

SUMMARY="$OUT/summary.txt"
SENTINEL="$OUT/DONE"
LOCK="$OUT/RUNNING"
FIXTURE=kickos_build

# KICKOS_QEMU_MPS2_BOARDS is read from its declaration; the other two emulator boards are
# spelled at their kickos_add_qemu_test call sites and have no list to read.
MPS2_SRC="$ROOT/user/apps/common/CMakeLists.txt"

die() { echo "FAIL: $*" >&2; exit 1; }

# verdict <selected> <ran> <failed> <skipped> <ctest rc>
# The one word a preset's run earns, from the census count and the junit attributes. SHORT
# covers a run of zero as well as a run cut short.
verdict() {
    if [ "$2" -ne "$1" ]; then
        echo SHORT
    elif [ "$3" -gt 0 ]; then
        echo FAILED
    elif [ "$4" -gt 0 ]; then
        echo PARTIAL
    elif [ "$5" -ne 0 ]; then
        echo RCFAIL
    else
        echo PASS
    fi
}

# One planted count per clause, each a minimal pair against the one above it, so a clause
# that is silent for the wrong reason answers the wrong word rather than passing.
control() { # <expected word> <selected> <ran> <failed> <skipped> <ctest rc>
    _want="$1"
    shift
    _got="$(verdict "$@")"
    [ "$_got" = "$_want" ] || die "the run classifier answers $_got where it must answer
      $_want, for $*. It cannot be trusted to refuse a short run, so nothing below it can
      report an absence."
}
control PASS    3 3 0 0 0
control SHORT   3 2 0 0 0
control SHORT   3 0 0 0 0
control SHORT   3 4 0 0 0
control FAILED  3 3 1 0 0
control PARTIAL 3 3 0 1 0
control RCFAIL  3 3 0 0 1

for _n in "$EXPECT_EMPTY" "$EXPECT_SKIP"; do
    case "$_n" in
        ""|*[!0-9]*) die "SWEEP_EXPECT_EMPTY and SWEEP_EXPECT_SKIP take a count, not '$_n'" ;;
    esac
done

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

# FIRST, before anything can be read as evidence: a sentinel a re-run does not clear reads as
# "done" the instant the re-run starts.
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

# What the recorded status is evidence ABOUT. Without git the identity is unique to this
# run, so nothing is ever reused and the sweep is whole rather than partly reprinted.
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
    if [ "$1" = qemu-riscv64 ]; then
        echo qemu-system-riscv64
    fi
    if [ "$1" = qemu-arm64 ]; then
        echo qemu-system-aarch64
    fi
    if [ "$1" = imx8mp-evk ]; then
        echo qemu-system-aarch64
    fi
    # Without this row the board falls to the no-emulator branch below and is reported as
    # needing SILICON, which files it beside the boards that really do and hides every image
    # gate it declares.
    if [ "$1" = qemu-x86_64 ]; then
        echo qemu-system-x86_64
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

N_LIST=0
for p in $LIST; do
    cut -f1 "$PRESETS_TSV" | grep -qxF "$p" || die "'$p' is not a visible configure preset"
    N_LIST=$((N_LIST + 1))
done
# Handed nothing, this tool would sweep nothing and report nothing wrong.
[ "$N_LIST" -gt 0 ] || die "the selection is empty, so there is no corpus to sweep. A run
      over zero presets has no image gate to fail and would report clean over nothing."

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

    if [ "$FORCE" != 1 ] && [ -f "$ST" ]; then
        # A recorded line that no longer names its own gate count is not readable evidence.
        RGATES="$(sed -n 's/^PASS  *[^ ][^ ]*  *\([0-9][0-9]*\) image gate(s).*/\1/p' "$ST" \
            | tail -n1)"
        if [ -n "$RGATES" ]; then
            N_REUSED=$((N_REUSED + 1))
            N_PASS=$((N_PASS + 1))
            N_GATES=$((N_GATES + RGATES))
            # Reprinted from the recorded status, never re-asserted.
            printf 'REUSED  %s\n' "$(cat "$ST")" >> "$SUMMARY"
            continue
        fi
    fi

    echo "=== $p ($BOARD) ===" >&2
    : > "$LOG"

    # -S "$ROOT" IS LOAD-BEARING: `cmake --preset` resolves CMakePresets.json against the
    # CURRENT DIRECTORY, and nothing here ever changes it. Invoked from another checkout this
    # configured 59 of 60 presets against THAT tree while every result was stamped against
    # $ROOT, and the only preset that failed was the one this tree alone carries, so the
    # sweep read as a clean pass over a tree it never compiled. THE TREE STAMP IS NOT
    # PROTECTION: it is taken from $ROOT by `git -C`, so it agrees with itself no matter which
    # sources cmake actually read.
    if ! cmake -S "$ROOT" --preset "$p" -B "$DIR" $PREFIX_ARG >> "$LOG" 2>&1; then
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
    if [ "$RAN" -gt "$C_FIXTURE" ]; then
        N_GATES=$((N_GATES + RAN - C_FIXTURE))
    fi

    case "$(verdict "$N_SEL" "$RAN" "$FAILED" "$SKIPPED" "$RC")" in
        SHORT)
            printf 'FAIL    %-22s the census selected %s test(s) and ctest ran %s over %s image gate(s)%s; the suite changed shape between the two, see logs/%s.log\n' \
                "$p" "$N_SEL" "$RAN" "$N_IMAGE" "$FIX" "$p" > "$ST"
            N_FAIL=$((N_FAIL + 1))
            ;;
        FAILED)
            printf 'FAIL    %-22s %s image gate(s)%s: %s run, %s failed, %s skipped%s, see logs/%s.log\n' \
                "$p" "$N_IMAGE" "$FIX" "$RAN" "$FAILED" "$SKIPPED" "$NOTE" "$p" > "$ST"
            N_FAIL=$((N_FAIL + 1))
            ;;
        PARTIAL)
            # A skip here is a gate that booted nothing, and the emulator check above
            # already cleared the one reason this tool can predict.
            printf 'PARTIAL %-22s %s image gate(s)%s: %s run, 0 failed, %s SKIPPED%s, see logs/%s.log\n' \
                "$p" "$N_IMAGE" "$FIX" "$RAN" "$SKIPPED" "$NOTE" "$p" > "$ST"
            N_PARTIAL=$((N_PARTIAL + 1))
            ;;
        RCFAIL)
            printf 'FAIL    %-22s ctest exited %s with 0 junit failures over %s image gate(s)%s\n' \
                "$p" "$RC" "$N_IMAGE" "$FIX" > "$ST"
            N_FAIL=$((N_FAIL + 1))
            ;;
        *)
            printf 'PASS    %-22s %s image gate(s)%s: %s run, 0 failed, 0 skipped%s\n' \
                "$p" "$N_IMAGE" "$FIX" "$RAN" "$NOTE" > "$ST"
            N_PASS=$((N_PASS + 1))
            ;;
    esac
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

# Every clause below is something this run has to have DONE.
WHY=""
refuse() { WHY="$WHY
      $*"; }

N_SEEN=$((N_PASS + N_PARTIAL + N_FAIL + N_EMPTY + N_SKIP))
[ "$N_TOTAL" -eq "$N_LIST" ] \
    || refuse "$N_TOTAL of the $N_LIST selected preset(s) were reached"
[ "$N_SEEN" -eq "$N_TOTAL" ] \
    || refuse "$N_SEEN of $N_TOTAL preset(s) reached a verdict; the rest left no record"
[ "$N_FAIL" -eq 0 ] \
    || refuse "$N_FAIL preset(s) failed"
[ "$N_PARTIAL" -eq 0 ] \
    || refuse "$N_PARTIAL preset(s) skipped a gate at run time"
[ "$N_EMPTY" -eq "$EXPECT_EMPTY" ] \
    || refuse "$N_EMPTY preset(s) registered no image gate against SWEEP_EXPECT_EMPTY=$EXPECT_EMPTY"
[ "$N_SKIP" -eq "$EXPECT_SKIP" ] \
    || refuse "$N_SKIP preset(s) were not run at all against SWEEP_EXPECT_SKIP=$EXPECT_SKIP"
[ "$N_PASS" -gt 0 ] \
    || refuse "no preset ran the image gates its census selected"
[ "$N_GATES" -gt 0 ] \
    || refuse "zero image gates ran"

if [ -z "$WHY" ]; then
    # The only writer of this file, and it carries the figures it asserts.
    {
        echo "tree     $TREE_ID"
        echo "selected $N_LIST preset(s)"
        echo "ran      $N_PASS preset(s) ran every test their census selected ($N_REUSED reused)"
        echo "gates    $N_GATES image gate(s), 0 failed, 0 skipped"
        echo "declared $N_EMPTY with no image gate, $N_SKIP not runnable here"
        echo "finished $(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    } > "$SENTINEL"
    tail -n 4 "$SUMMARY"
    exit 0
fi

{
    echo "REFUSED: this sweep is not a witness for the selection it was handed:$WHY"
    echo "      no DONE sentinel written under $OUT"
} >> "$SUMMARY"
tail -n 8 "$SUMMARY" >&2
exit 1
