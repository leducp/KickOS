#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Trap red-zone gate. A trap prologue on rv32imac, rxv3 and armv7m reserves a RED ZONE at
# the bottom of the interrupted unprivileged stack and refuses the trap when less room
# than that remains below sp. The red zone has to cover the frame the prologue stores PLUS
# the worst-case depth of the kernel C dispatch that then runs below that frame,
# privileged, on the same thread stack. This script re-measures that depth and fails when
# it exceeds what the prologue enforces.
#
# HOW IT MEASURES. It configures a SCRATCH tree of its own with
# -fcallgraph-info=su,da, which makes gcc drop a .ci file next to every object carrying
# that translation unit's frame sizes, alloca counts and call edges. tests/static/
# trap_redzone.py merges the whole tree's .ci files and takes the longest weighted path
# from the declared trap-path roots. The scratch tree has to stay separate from the
# caller's build dir: those flags change every object.
#
# The enforced numbers are scraped out of the arch header, and WHICH macros to scrape
# comes from tests/static/trap_redzone_roots.txt. A macro the scrape cannot find is a hard
# failure and never a default: a gate that defaults its own reference compares a
# measurement against nothing.
#
# HOW trap_redzone.py REFUSES TO ANSWER, each refusal its own named failure:
#   - a class measures deeper than its red zone reserves.
#   - an indirect call site reachable from a root is not bound in
#     trap_redzone_indirect.txt. gcc emits one anonymous __indirect_call edge per site and
#     nothing more, so an unbound site turns the whole figure into a lower bound.
#   - a binding names a site or a callee the graph does not have.
#   - a reachable frame allocates an alloca/VLA, which has no static size.
#   - a reachable cycle, or a reachable node with no frame size that is not in the
#     declared unsized-allowance list.
#   - a reachable symbol still has two rival definitions after the unextracted seam
#     fallbacks are dropped, so which one linked is not something the graph can say.
#   - a class has no root at all. A class that runs no C on the guarded stack says so with
#     `root <arch> <CLASS> NONE reason: ...`, which charges 0 and prints the reason, so an
#     empty root set can never be an accident.
#
# AND ONE REFUSAL BEFORE ANY OF THAT: the scratch tree's own flags. The measurement is only
# the image's if the scratch tree compiled the image's ISA, so the cache is checked for the
# callgraph flag and for every token of KICKOS_MCPU_FLAGS, and a tree that fails is
# reconfigured from scratch rather than measured.
#
# THE FLOOR CLAUSE IS NOT ABOUT THE PROLOGUE. A red zone larger than
# KICKOS_MIN_STACK_SIZE means a thread spawned at the floor passes the spawn check and
# then cannot take that trap at all: the prologue refuses it forever. So the floor is
# compared against every class's red zone, and the remedy is named in the message. The one
# exception is a class declared `stack=trap`, whose descent runs on a static kernel stack
# and spends no thread stack at all; trap_redzone_roots.txt states what that flag means and
# the other consequence it carries.
#
# SCOPE. Know these before trusting a green run:
#   - ONE BOARD PER RUN. The measurement covers the objects THIS preset compiles, and the
#     console backend is per board, so a board is measured on the runs it gets.
#   - COMPILED IS NOT LINKED. The compiler writes a .ci for every TU, including the seam
#     fallbacks an extraction kept out of the image. trap_redzone.py drops those by the
#     rule arch/CMakeLists.txt states, which is keyed on the _default.cc naming.
#   - THE ROOT SET IS A DECLARATION. The depth below a call switch.S makes is in the
#     number when the roots file names it, so the roots are the part of this gate a reader
#     keeps honest against the assembly.
#   - THE COMPILER'S OWN FRAME NUMBERS ARE TAKEN ON TRUST, and they are per translation
#     unit, so they hold for the objects this configuration produced and for the same
#     optimization level. The gate builds MinSizeRel because the presets do.
#   - INLINING IS ALREADY IN THEM, and a callee gcc inlined has no node and no edge. That
#     is correct for the depth, its locals being in the caller's frame, and it is why the
#     winning chain printed below can be shorter than the source reads.

set -u
# Every path arrives as an argument and is re-split unquoted below; a glob character in a
# build path must not expand against the cwd.
set -f
. "$(dirname "$0")/../lib/gate.sh"

# The macro scrape and the awk record parses are structural, never keyed on a translated
# heading.
export LC_ALL=C

if [ "$#" -ne 4 ]; then
    echo "usage: $0 <src-dir> <cmake> <preset> <arch>" >&2
    exit 2
fi

SRC="$1"
CMAKE="$2"
PRESET="$3"
ARCH="$4"

case "$SRC$CMAKE$PRESET$ARCH" in
    *[[:space:]]*) fail "an argument contains whitespace; every path here is re-split" ;;
esac

HERE="$(dirname "$0")"
ROOTS="$HERE/trap_redzone_roots.txt"
INDIRECT="$HERE/trap_redzone_indirect.txt"
TOOL="$HERE/trap_redzone.py"
for f in "$ROOTS" "$INDIRECT" "$TOOL"; do
    [ -r "$f" ] || fail "cannot read $f"
done
[ -d "$SRC" ] || fail "source dir does not exist: $SRC"
command -v "$CMAKE" >/dev/null 2>&1 || [ -x "$CMAKE" ] || fail "cmake not executable: $CMAKE"

scratch_dir

# Strips '#' comments, joins '\' continuations, keeps only the records of one kind for one
# arch.
decl() { # <kind>
    awk -v KIND="$1" -v A="$ARCH" '
        { sub(/#.*/, "") }
        /\\[[:space:]]*$/ { sub(/\\[[:space:]]*$/, " "); acc = acc $0; next }
        { line = acc $0; acc = "" }
        { n = split(line, f, /[[:space:]]+/) }
        n >= 2 && f[1] == KIND && f[2] == A { print line }
    ' "$ROOTS"
}

# --- clause 0: the preset/arch pair is declared -------------------------------
# A pair nobody declared has no header to scrape and no root set, so it cannot be
# measured; refusing beats measuring the wrong thing.
if ! decl preset | awk -v P="$PRESET" '{ if ($3 == P) { found = 1 } } END { exit !found }'; then
    KNOWN="$(decl preset | awk '{ printf "%s ", $3 }')"
    fail "preset/arch pair $PRESET/$ARCH is not declared in $ROOTS (for $ARCH: $KNOWN)"
fi

HEADER_REL="$(decl arch | sed -n 's/.*header=\([^ ]*\).*/\1/p' | head -n1)"
[ -n "$HEADER_REL" ] || fail "$ROOTS declares no header= for arch $ARCH"
HEADER="$SRC/$HEADER_REL"

decl class | awk '
    {
        frame = ""; depth = ""; onstack = "thread"
        n = split($0, f, /[[:space:]]+/)
        for (i = 4; i <= n; i++) {
            if (f[i] ~ /^frame=/) { frame = substr(f[i], 7) }
            if (f[i] ~ /^depth=/) { depth = substr(f[i], 7) }
            if (f[i] == "stack=trap") { onstack = "trap" }
        }
        if (frame == "" || depth == "") { exit 1 }
        print f[3] "\t" frame "\t" depth "\t" onstack
    }' > "$TMP/classes" || fail "$ROOTS has a class record for $ARCH without frame=/depth="
require_nonempty "$TMP/classes" "$ROOTS declares no class for arch $ARCH"

# --- configure and build the scratch tree ------------------------------------
# Named per preset so two boards do not fight over one tree, and reused so a re-run is
# cheap. Under /var/tmp because these trees are large and must outlive a /tmp wipe.
BUILD="${KICKOS_TRAP_REDZONE_DIR:-/var/tmp/kickos-trap-redzone-$PRESET}"
CGFLAGS="-fcallgraph-info=su,da"

# THE ENVIRONMENT, NOT -DCMAKE_C_FLAGS. Every cross toolchain file here puts this board's
# ISA baseline in CMAKE_<LANG>_FLAGS_INIT, and a -D on the command line REPLACES the cache
# value that comes from it. CFLAGS/CXXFLAGS are combined with it instead, so the scratch
# tree compiles the same ISA the image does.
configure_scratch() { # <extra cmake arg>...
    CFLAGS="$CGFLAGS" CXXFLAGS="$CGFLAGS" \
    "$CMAKE" -S "$SRC" -B "$BUILD" --preset "$PRESET" "$@" > "$TMP/cfg.log" 2>&1 \
        || { sed -n '1,40p' "$TMP/cfg.log" >&2
             fail "configure failed for preset $PRESET (cross toolchains come from the environment)"; }
}

# A REUSED TREE HAS TO BE THE RIGHT TREE. The cache is checked rather than trusted: the
# callgraph flag must be there, and so must every token of this board's ISA baseline, which
# the toolchain file exports as KICKOS_MCPU_FLAGS. Without this check a tree left behind by
# an older recipe is measured instead, and a measurement of the wrong ISA is the one failure
# mode this gate cannot see in its own output.
scratch_flags_ok() {
    _cxx="$(sed -n 's/^CMAKE_CXX_FLAGS:STRING=//p' "$BUILD/CMakeCache.txt" | head -n1)"
    case " $_cxx " in
        *" $CGFLAGS "*) ;;
        *) echo "trap_redzone: scratch tree lacks $CGFLAGS" >&2; return 1 ;;
    esac
    # A CMake list in the cache, so semicolon-separated; the flags reach the compiler one
    # per argument, and this loop compares them one at a time.
    _mcpu="$(sed -n 's/^KICKOS_MCPU_FLAGS:INTERNAL=//p' "$BUILD/CMakeCache.txt" | head -n1 \
             | tr ';' ' ')"
    [ -n "$_mcpu" ] || { echo "trap_redzone: scratch tree has no KICKOS_MCPU_FLAGS" >&2
                         return 1; }
    for _tok in $_mcpu; do
        case " $_cxx " in
            *" $_tok "*) ;;
            *) echo "trap_redzone: scratch tree is missing the ISA flag $_tok" >&2
               return 1 ;;
        esac
    done
    return 0
}

# The scratch tree is named by PRESET alone, so two source trees on one box share it. A
# cache still pointing at another checkout measures THAT checkout, or fails obscurely once
# it is deleted, so a source-dir mismatch is treated exactly like a flag mismatch.
scratch_source_ok() {
    _sc_home="$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$BUILD/CMakeCache.txt" 2>/dev/null)"
    [ -n "$_sc_home" ] || return 1
    [ "$_sc_home" = "$SRC" ]
}

if [ -f "$BUILD/CMakeCache.txt" ] && scratch_source_ok && scratch_flags_ok; then
    echo "trap_redzone: reusing scratch tree $BUILD"
else
    if [ -f "$BUILD/CMakeCache.txt" ]; then
        if ! scratch_source_ok; then
            echo "trap_redzone: reconfiguring $BUILD from scratch; its cache names another"
            echo "trap_redzone: source tree, so what it holds is another checkout's depths"
        else
            echo "trap_redzone: reconfiguring $BUILD from scratch; its flags are not the ones"
            echo "trap_redzone: this measurement needs, so what it holds is another ISA's depths"
        fi
        configure_scratch --fresh
    else
        echo "trap_redzone: configuring scratch tree $BUILD"
        configure_scratch
    fi
    scratch_flags_ok || fail "the scratch tree still lacks the flags this gate needs after a
    fresh configure of preset $PRESET; the toolchain file may no longer seed
    CMAKE_<LANG>_FLAGS_INIT, in which case CFLAGS/CXXFLAGS no longer reach the compiler"
fi
# An aborted earlier run leaves a cache that passes both checks above beside generated rules
# that do not load. That is a scratch tree to rebuild rather than a measurement error, so
# the build gets exactly one --fresh retry before it counts as a failure.
if ! "$CMAKE" --build "$BUILD" > "$TMP/build.log" 2>&1; then
    echo "trap_redzone: the scratch build failed; reconfiguring $BUILD from scratch and retrying once"
    configure_scratch --fresh
    "$CMAKE" --build "$BUILD" > "$TMP/build.log" 2>&1 \
        || { sed -n '$!d;1,60p' "$TMP/build.log" >&2
             grep -E 'error|Error' "$TMP/build.log" | sed -n '1,20p' >&2
             fail "build failed in $BUILD, twice, the second time after a fresh configure"; }
fi

# The generated cmake fragment, not the configure log: the log only exists on the run that
# configured, and this gate deliberately reuses a tree.
CFGFILE="$BUILD/generated/kickos_config.cmake"
[ -r "$CFGFILE" ] || fail "cannot read $CFGFILE, which is where the gate reads KICKOS_MIN_STACK_SIZE"
FLOOR="$(sed -n 's/^[[:space:]]*set(KICKOS_MIN_STACK_SIZE[[:space:]]\{1,\}\([0-9]\{1,\}\)).*/\1/p' \
         "$CFGFILE" | head -n1)"
[ -n "$FLOOR" ] || fail "no KICKOS_MIN_STACK_SIZE in $CFGFILE"

# --- scrape the enforced figures out of the arch header ----------------------
[ -r "$HEADER" ] || fail "cannot read $HEADER, so the enforced red zone cannot be read;
    this gate compares a measurement against that header and has no default to fall back on"

# A plain integer only. These macros are read by the assembler as immediates, so an
# expression here would mean the header changed shape and the scrape has to be revisited
# rather than guessed at.
scrape_macro() { # <macro>
    _hits="$(sed -n "s|^[[:space:]]*#[[:space:]]*define[[:space:]]\{1,\}$1[[:space:]]\{1,\}\([0-9]\{1,\}\)[[:space:]]*\(/\*.*\)\{0,1\}$|\1|p" \
             "$HEADER")"
    if [ -z "$_hits" ]; then
        fail "$1 is not defined as a plain integer in $HEADER; a scrape that cannot find its
    reference has nothing to compare against, and defaulting it would make this gate lie"
    fi
    if [ "$(printf '%s\n' "$_hits" | wc -l | tr -d ' ')" -ne 1 ]; then
        fail "$1 is defined more than once in $HEADER; which one the assembler sees is not
    something this gate should guess"
    fi
    printf '%s\n' "$_hits"
}

ENFORCED_ARGS=""
rc=0
bad() { echo "FAIL: $*" >&2; rc=1; }

echo "trap_redzone: preset=$PRESET arch=$ARCH"
echo "trap_redzone: header  $HEADER"
echo "trap_redzone: floor   KICKOS_MIN_STACK_SIZE=$FLOOR (from $CFGFILE)"
while IFS="$TAB" read -r cls frame_macro depth_macro onstack; do
    [ -n "$cls" ] || continue
    frame="$(scrape_macro "$frame_macro")" || exit 1
    depth="$(scrape_macro "$depth_macro")" || exit 1
    zone=$((frame + depth))
    echo "trap_redzone: class $cls  $frame_macro=$frame  $depth_macro=$depth  zone=$zone ($onstack stack)"
    ENFORCED_ARGS="$ENFORCED_ARGS --enforced $cls=$frame,$depth"
    # A stack=trap class spends a static kernel array, not a thread stack, so the floor says
    # nothing about it. Skipped by NAME rather than by the figure happening to fit, or the
    # clause would start passing for the wrong reason the day the array grew.
    if [ "$onstack" = trap ]; then
        continue
    fi
    # Own clause, and NOT the same failure as an over-deep measurement: here the
    # measurement and the prologue agree, and the SPAWN FLOOR is the thing that is wrong.
    if [ "$zone" -gt "$FLOOR" ]; then
        bad "SPAWN FLOOR BELOW RED ZONE: $ARCH reserves $zone bytes for a $cls trap
    ($frame_macro=$frame + $depth_macro=$depth) but KICKOS_MIN_STACK_SIZE is $FLOOR. A
    thread spawned at the floor passes the spawn check and can then never take that trap:
    the prologue refuses it, every time, for the life of the thread.
    REMEDY: raise the per-arch default in $SRC/Kconfig (config KICKOS_MIN_STACK_SIZE,
    'default <n> if ARCH_$(printf '%s' "$ARCH" | tr '[:lower:]' '[:upper:]')') from $FLOOR
    to at least $zone. Do NOT shrink the red zone to fit: it is a measurement."
    fi
done < "$TMP/classes"

# --- the measurement ---------------------------------------------------------
# shellcheck disable=SC2086
python3 "$TOOL" --ci-dir "$BUILD" --arch "$ARCH" --preset "$PRESET" \
    --roots "$ROOTS" --indirect "$INDIRECT" $ENFORCED_ARGS
prc=$?
if [ "$prc" -ne 0 ]; then
    rc="$prc"
fi

if [ "$rc" -eq 0 ]; then
    echo "trap_redzone: OK ($PRESET/$ARCH, floor $FLOOR)"
fi
exit "$rc"
