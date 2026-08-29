# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The scratch callgraph tree two gates share. SOURCED, never executed, and only after
# gate.sh, whose fail() and $TMP it uses:
#   . "$(dirname "$0")/../lib/gate.sh"
#   . "$(dirname "$0")/../lib/scratch_ci.sh"
#
# It configures and builds ONE preset with -fcallgraph-info=su,da, which makes gcc drop a
# .ci file next to every object carrying that translation unit's frame sizes, alloca counts
# and call edges. The tree is kept out of the caller's build directory because those flags
# change every object, and it is REUSED across runs because a cold cross build is minutes.
#
# A REUSED TREE HAS TO BE THE RIGHT TREE, and the two checks below are what say so: the
# cache must carry the callgraph flag and every token of this board's ISA baseline, and its
# CMAKE_HOME_DIRECTORY must be the source tree the caller named. A wrong-ISA or
# wrong-checkout measurement is the one failure mode neither gate can see in its own output.
#
# NOTHING HERE RE-DERIVES THE GENERATED Kconfig STATE on the reuse path. Moving a Kconfig
# default does not reach a tree that already exists, so a gate whose clause reads one of
# those figures has to be told to start from a clean tree.
#
# KOS_CI_TAG is the caller's name and prefixes every line printed here, so a gate's output
# still reads as that gate's.

KOS_CI_FLAGS="-fcallgraph-info=su,da"
KOS_CI_TAG="${KOS_CI_TAG:-scratch_ci}"

# The ENVIRONMENT, not -DCMAKE_C_FLAGS. Every cross toolchain file here puts this board's ISA
# baseline in CMAKE_<LANG>_FLAGS_INIT, and a -D on the command line REPLACES the cache value
# that comes from it; CFLAGS/CXXFLAGS are combined with it instead.
_scratch_ci_configure() { # <extra cmake arg>...
    CFLAGS="$KOS_CI_FLAGS" CXXFLAGS="$KOS_CI_FLAGS" \
    "$_KOS_CI_CMAKE" -S "$_KOS_CI_SRC" -B "$_KOS_CI_BUILD" --preset "$_KOS_CI_PRESET" "$@" \
        > "$TMP/cfg.log" 2>&1 \
        || { sed -n '1,40p' "$TMP/cfg.log" >&2
             fail "configure failed for preset $_KOS_CI_PRESET (cross toolchains come from the environment)"; }
}

# `cmake --fresh` drops the cache and CMakeFiles and NOTHING ELSE: the generated Kconfig state
# under $BUILD/generated survives it, and genconfig.py then refuses a configure whose
# .defconfig stamp names another checkout. Removing that state is what makes --fresh fresh.
_scratch_ci_fresh() {
    rm -rf "$_KOS_CI_BUILD/generated"
    _scratch_ci_configure --fresh
}

_scratch_ci_flags_ok() {
    _cxx="$(sed -n 's/^CMAKE_CXX_FLAGS:STRING=//p' "$_KOS_CI_BUILD/CMakeCache.txt" | head -n1)"
    case " $_cxx " in
        *" $KOS_CI_FLAGS "*) ;;
        *) echo "$KOS_CI_TAG: scratch tree lacks $KOS_CI_FLAGS" >&2; return 1 ;;
    esac
    # A CMake list in the cache, so semicolon-separated; the flags reach the compiler one
    # per argument, and this loop compares them one at a time.
    # PRESENT-BUT-EMPTY IS LEGAL AND ABSENT IS NOT: the xtensa toolchain names no ISA flag,
    # the windowed ABI being the compiler's default, so grep for the KEY and let the value be
    # empty. Testing the value alone would read a toolchain that stopped seeding the cache as
    # a board with nothing to check.
    grep -q '^KICKOS_MCPU_FLAGS:INTERNAL=' "$_KOS_CI_BUILD/CMakeCache.txt" \
        || { echo "$KOS_CI_TAG: scratch tree has no KICKOS_MCPU_FLAGS" >&2; return 1; }
    _mcpu="$(sed -n 's/^KICKOS_MCPU_FLAGS:INTERNAL=//p' "$_KOS_CI_BUILD/CMakeCache.txt" \
             | head -n1 | tr ';' ' ')"
    for _tok in $_mcpu; do
        case " $_cxx " in
            *" $_tok "*) ;;
            *) echo "$KOS_CI_TAG: scratch tree is missing the ISA flag $_tok" >&2
               return 1 ;;
        esac
    done
    return 0
}

# The scratch tree is named by PRESET alone, so two source trees on one box share it. A cache
# still pointing at another checkout measures THAT checkout, so a source-dir mismatch is treated
# exactly like a flag mismatch.
_scratch_ci_source_ok() {
    _sc_home="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' \
                "$_KOS_CI_BUILD/CMakeCache.txt" 2>/dev/null)"
    [ -n "$_sc_home" ] || return 1
    [ "$_sc_home" = "$_KOS_CI_SRC" ]
}

scratch_ci_build() { # <src-dir> <cmake> <preset> <build-dir>
    _KOS_CI_SRC="$1"
    _KOS_CI_CMAKE="$2"
    _KOS_CI_PRESET="$3"
    _KOS_CI_BUILD="$4"

    if [ -f "$_KOS_CI_BUILD/CMakeCache.txt" ] && _scratch_ci_source_ok && _scratch_ci_flags_ok
    then
        echo "$KOS_CI_TAG: reusing scratch tree $_KOS_CI_BUILD"
    else
        if [ -f "$_KOS_CI_BUILD/CMakeCache.txt" ]; then
            if ! _scratch_ci_source_ok; then
                echo "$KOS_CI_TAG: reconfiguring $_KOS_CI_BUILD from scratch; its cache names another"
                echo "$KOS_CI_TAG: source tree, so what it holds is another checkout's depths"
            else
                echo "$KOS_CI_TAG: reconfiguring $_KOS_CI_BUILD from scratch; its flags are not the ones"
                echo "$KOS_CI_TAG: this measurement needs, so what it holds is another ISA's depths"
            fi
            _scratch_ci_fresh
        else
            # A directory with generated Kconfig state and NO cache is the same refusal one
            # branch up: the cache is what --fresh drops, and it is the generated state that makes
            # genconfig refuse.
            echo "$KOS_CI_TAG: configuring scratch tree $_KOS_CI_BUILD"
            rm -rf "$_KOS_CI_BUILD/generated"
            _scratch_ci_configure
        fi
        _scratch_ci_flags_ok || fail "the scratch tree still lacks the flags this gate needs after a
    fresh configure of preset $_KOS_CI_PRESET; the toolchain file may no longer seed
    CMAKE_<LANG>_FLAGS_INIT, in which case CFLAGS/CXXFLAGS no longer reach the compiler"
    fi
    # An aborted earlier run leaves a cache that passes both checks above beside generated rules
    # that do not load, so the build gets exactly one --fresh retry before it counts as a failure.
    if ! "$_KOS_CI_CMAKE" --build "$_KOS_CI_BUILD" > "$TMP/build.log" 2>&1; then
        echo "$KOS_CI_TAG: the scratch build failed; reconfiguring $_KOS_CI_BUILD from scratch and retrying once"
        _scratch_ci_fresh
        "$_KOS_CI_CMAKE" --build "$_KOS_CI_BUILD" > "$TMP/build.log" 2>&1 \
            || { sed -n '$!d;1,60p' "$TMP/build.log" >&2
                 grep -E 'error|Error' "$TMP/build.log" | sed -n '1,20p' >&2
                 fail "build failed in $_KOS_CI_BUILD, twice, the second time after a fresh configure"; }
    fi
}
