#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Trap stack-geometry gate. A trap entry on rv32imac, rxv3, armv7m and armv6m builds a frame
# and then runs the kernel's C dispatch below it, privileged, and this script re-measures that
# descent and fails when it exceeds what the arch header enforces. WHICH STACK the pair lands
# on is per class, and it is what the failure clauses below key on:
#   thread  the interrupted unprivileged stack. The entry reserves a RED ZONE at the bottom of
#           it and refuses the trap when less room than that remains below sp, so the zone is
#           also compared against the spawn floor.
#   kernel  the interrupted thread's own per-thread kernel block, which the entry transfers to
#           instead of continuing on the sp the thread chose. Compared against
#           KICKOS_KERNEL_STACK_SIZE.
#   trap    a static per-hart stack the arch owns. No thread stack is spent, so no fit clause
#           here applies to it; its own size lives in the arch header.
#
# HOW IT MEASURES. It configures a SCRATCH tree of its own with
# -fcallgraph-info=su,da, which makes gcc drop a .ci file next to every object carrying
# that translation unit's frame sizes, alloca counts and call edges. tests/static/
# trap_redzone.py merges the whole tree's .ci files and takes the longest weighted path
# from the declared trap-path roots. The scratch tree has to stay separate from the
# caller's build dir: those flags change every object.
#
# The enforced numbers come out of the arch header, and WHICH macros to read comes from
# tests/static/trap_redzone_roots.txt. A macro that cannot be read is a hard failure and never
# a default: a gate that defaults its own reference compares a measurement against nothing.
# The header is RESOLVED THROUGH THE COMPILER rather than read as text, because a figure there
# can be posture-dependent (rv32imac's SYSPRIV depth resolves per KICKOS_BENCH) and a text
# scrape would either see both branches or silently take the wrong one. The block below the
# clause-0 checks states where the flags for that resolution come from.
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
# And two the script itself refuses on, named separately in its output: a spawn floor under a
# thread-stack class's red zone, and a kernel block under a kernel-stack class's requirement.
#
# AND ONE REFUSAL BEFORE ANY OF THAT: the scratch tree's own flags. The measurement is only
# the image's if the scratch tree compiled the image's ISA, so the cache is checked for the
# callgraph flag and for every token of KICKOS_MCPU_FLAGS, and a tree that fails is
# reconfigured from scratch rather than measured.
#
# THE FLOOR CLAUSE IS NOT ABOUT THE PROLOGUE. A thread-stack requirement larger than
# KICKOS_MIN_STACK_SIZE means a thread spawned at the floor passes the spawn check and then
# cannot complete that trap at all: refused forever where the entry bounds the sp, and an
# overflow where it does not bound it, which is the shape of a class whose caller was already
# privileged. So the floor is compared against every thread-stack class, and the remedy is
# named in the message. A class declared `stack=trap` or `stack=kernel` is skipped by NAME, not by its
# figure happening to fit; trap_redzone_roots.txt states what those flags mean and the other
# consequence they carry.
#
# AND THE BLOCK CLAUSE IS NOT ABOUT THE FLOOR. A `stack=kernel` class's frame plus depth is
# what one thread's kernel block has to hold, so it is compared against
# KICKOS_KERNEL_STACK_SIZE read out of the GENERATED board config of the tree just built,
# minus the canary word at the bottom of a block. Raising the spawn floor would answer that
# failure with nothing, which is why it is a clause of its own with its own remedy.
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
        frame = ""; depth = ""; onstack = "thread"; kstacks = "any"
        n = split($0, f, /[[:space:]]+/)
        for (i = 4; i <= n; i++) {
            if (f[i] ~ /^frame=/) { frame = substr(f[i], 7) }
            if (f[i] ~ /^depth=/) { depth = substr(f[i], 7) }
            if (f[i] == "stack=trap") { onstack = "trap" }
            if (f[i] == "stack=kernel") { onstack = "kernel" }
            if (f[i] == "kstacks=0") { kstacks = "0" }
            if (f[i] == "kstacks=1") { kstacks = "1" }
        }
        if (frame == "" || depth == "") { exit 1 }
        print f[3] "\t" frame "\t" depth "\t" onstack "\t" kstacks
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
# `cmake --fresh` drops the cache and CMakeFiles, and NOTHING ELSE: the generated Kconfig
# state under $BUILD/generated survives it, and tools/kconfig/genconfig.py then reads the
# live .config plus the .defconfig stamp naming the checkout that wrote it. From another
# source tree that stamp names another absolute path, so the configure is REFUSED
# ("REFUSED variant change") and the recovery this gate promises never happens. Removing
# the generated state is what makes --fresh actually fresh; a defconfig is re-read from the
# source tree on the next configure, so nothing is lost with it.
fresh_configure() {
    rm -rf "$BUILD/generated"
    configure_scratch --fresh
}

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
        fresh_configure
    else
        # A directory with generated Kconfig state and NO cache is the same refusal one
        # branch up: the cache is what --fresh drops, and it is the generated state that
        # makes genconfig refuse. Removing it here covers a partial cleanup as well as a
        # cross-tree one.
        echo "trap_redzone: configuring scratch tree $BUILD"
        rm -rf "$BUILD/generated"
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
    fresh_configure
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

# --- resolve the enforced figures out of the arch header ---------------------
[ -r "$HEADER" ] || fail "cannot read $HEADER, so the enforced red zone cannot be read;
    this gate compares a measurement against that header and has no default to fall back on"

# A FIGURE IN THAT HEADER CAN BE POSTURE-DEPENDENT, so reading the file as text is not enough:
# rv32imac's SYSPRIV depth resolves per KICKOS_BENCH, and a sed over the source would see both
# branches of that ladder and refuse the macro as defined twice, or take the first and compare
# the measurement against the wrong posture's number. So the COMPILER resolves it, and the
# scrape below runs over its macro dump rather than over the header: one macro name, one live
# value, and the same number the C floor assert in that arch's backend sees.
#
# THE FLAGS ARE THE IMAGE'S, taken from compile_commands.json of the tree just built (every
# preset here turns CMAKE_EXPORT_COMPILE_COMMANDS on) and restricted to a translation unit of
# the arch directory the header belongs to, which is its `include/` parent. That TU is the one
# that static_asserts against these very macros, so the posture is the one they are enforced
# under by construction rather than by a list of knobs this script would have to keep.
#
# ONLY -D, -I, -isystem, -include and -std are carried over. Preprocessing needs no more, and
# -fcallgraph-info would have the resolution drop a .ci file of its own. -Wundef -Werror is
# added rather than inherited, and it is load-bearing: cpp takes the 0 branch of `#if KNOB`
# for an undefined KNOB without saying so, which is exactly the defaulted reference this gate
# refuses everywhere else.
CCJSON="$BUILD/compile_commands.json"
[ -r "$CCJSON" ] || fail "cannot read $CCJSON, which is where the flags that resolve
    $HEADER_REL come from; without them a posture-dependent figure resolves to the wrong branch"
python3 - "$BUILD" "$SRC" "$HEADER_REL" > "$TMP/figures.h" 2> "$TMP/figures.err" <<'PYEOF'
import json
import os
import shlex
import subprocess
import sys

build, src, header_rel = sys.argv[1], sys.argv[2], sys.argv[3]
if '/include/' not in header_rel:
    sys.stderr.write('the arch header %s is not under an <archdir>/include/ path, so the'
                     ' translation unit whose flags resolve it cannot be derived\n'
                     % header_rel)
    sys.exit(1)
archdir = os.path.join(src, header_rel.split('/include/', 1)[0])
try:
    db = json.load(open(os.path.join(build, 'compile_commands.json')))
except (OSError, ValueError) as e:
    sys.stderr.write('cannot read the compile database: %s\n' % e)
    sys.exit(1)

entry = None
for e in sorted(db, key=lambda e: e.get('file', '')):
    f = e.get('file', '')
    if f.startswith(archdir + os.sep) and os.path.splitext(f)[1] in ('.cc', '.c', '.cpp'):
        entry = e
        break
if entry is None:
    sys.stderr.write('no translation unit under %s in the compile database, so there is no'
                     ' posture to resolve %s under\n' % (archdir, header_rel))
    sys.exit(1)

# `arguments` when the generator emits it; `command` is one shell-quoted string, so it is
# split the way a shell would and not on whitespace: a -D value can carry quotes.
argv = entry.get('arguments')
if argv is None:
    argv = shlex.split(entry['command'])
cxx = argv[0]
keep = []
i = 1
while i < len(argv):
    a = argv[i]
    if a.startswith(('-D', '-I', '-std=')) and len(a) > 2:
        keep.append(a)
    elif a in ('-D', '-I', '-isystem', '-include') and i + 1 < len(argv):
        keep.append(a)
        keep.append(argv[i + 1])
        i += 1
    elif a.startswith('-isystem'):
        keep.append(a)
    i += 1

cmd = [cxx, '-E', '-dM', '-Wundef', '-Werror', '-x', 'c++'] + keep \
      + ['-include', os.path.join(src, header_rel), os.devnull]
try:
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
except OSError as e:
    sys.stderr.write('cannot run %s: %s\n' % (cxx, e))
    sys.exit(1)
if r.returncode != 0:
    sys.stderr.write(r.stderr.decode('utf-8', 'replace')[:2000])
    sys.stderr.write('\nresolving %s failed; the command was:\n%s\n'
                     % (header_rel, ' '.join(cmd)))
    sys.exit(1)
sys.stdout.write(r.stdout.decode('utf-8', 'replace'))
PYEOF
if [ "$?" -ne 0 ]; then
    sed -n '1,30p' "$TMP/figures.err" >&2
    fail "could not resolve the figures in $HEADER_REL through the compiler; this gate has no
    default to fall back on"
fi
FIGURES="$TMP/figures.h"

# A plain integer only. The arch header's macros are read by the assembler as immediates and
# the generated board config carries nothing else, so an expression in either would mean the
# file changed shape and the scrape has to be revisited rather than guessed at. Run over the
# resolved dump for the arch header and over the generated header itself for the board config,
# which carries no ladder at all.
scrape_macro() { # <file> <macro>
    _hits="$(sed -n "s|^[[:space:]]*#[[:space:]]*define[[:space:]]\{1,\}$2[[:space:]]\{1,\}\([0-9]\{1,\}\)[[:space:]]*\(/\*.*\)\{0,1\}$|\1|p" \
             "$1")"
    if [ -z "$_hits" ]; then
        fail "$2 is not defined as a plain integer in $1; a scrape that cannot find its
    reference has nothing to compare against, and defaulting it would make this gate lie"
    fi
    if [ "$(printf '%s\n' "$_hits" | wc -l | tr -d ' ')" -ne 1 ]; then
        fail "$2 is defined more than once in $1; which one the compiler sees is not
    something this gate should guess"
    fi
    printf '%s\n' "$_hits"
}

# --- the per-thread kernel block, for the stack=kernel classes ----------------
# Read from the GENERATED board config of the tree just built, the same place the compiler
# read it from, and only when a class asks for it.
KUSABLE=""
KSIZE=""
KSTACKS=1
KCANARY=4
# The knob is read for a stack=kernel class, which is measured against the block, AND for a
# kstacks= class, whose whole point is that the image may not compile it. Both need the live
# value, so one read serves them.
if awk -F"$TAB" '{ if ($4 == "kernel" || $5 != "any") { found = 1 } } END { exit !found }' "$TMP/classes"; then
    BOARDCFG="$BUILD/generated/include/kickos/board_config.h"
    [ -r "$BOARDCFG" ] || fail "cannot read $BOARDCFG, which is where the gate reads
    KICKOS_KERNEL_STACKS and the per-thread kernel block a stack=kernel class is measured
    against"
    KSTACKS="$(scrape_macro "$BOARDCFG" KICKOS_KERNEL_STACKS)" || exit 1
    # The SIZE is only the block clause's business, so it is read only where a class is
    # measured against a block that exists.
    if [ "$KSTACKS" -ne 0 ] &&
       awk -F"$TAB" '{ if ($4 == "kernel") { found = 1 } } END { exit !found }' "$TMP/classes"
    then
        KSIZE="$(scrape_macro "$BOARDCFG" KICKOS_KERNEL_STACK_SIZE)" || exit 1
        # The LOWEST word of a block is the overflow canary kmain arms
        # (kernel/thread/thread.cc), so it is not stack a descent may reach: a requirement
        # that merely equals the block reports an overflow every time the deepest legitimate
        # path runs.
        KUSABLE=$((KSIZE - KCANARY))
    fi
fi

ENFORCED_ARGS=""
rc=0
bad() { echo "FAIL: $*" >&2; rc=1; }

echo "trap_redzone: preset=$PRESET arch=$ARCH"
echo "trap_redzone: header  $HEADER"
echo "trap_redzone: floor   KICKOS_MIN_STACK_SIZE=$FLOOR (from $CFGFILE)"
if [ -n "$KSIZE" ]; then
    echo "trap_redzone: block   KICKOS_KERNEL_STACK_SIZE=$KSIZE, $KUSABLE usable above the canary"
elif [ "$KSTACKS" -eq 0 ]; then
    echo "trap_redzone: block   KICKOS_KERNEL_STACKS=0 on this board, so every stack=kernel
    class below is MEASURED AND NOT ENFORCED: it describes an entry design this image does
    not compile"
fi
NOTCOMPILED_ARGS=""
while IFS="$TAB" read -r cls frame_macro depth_macro onstack kstacks; do
    [ -n "$cls" ] || continue
    frame="$(scrape_macro "$FIGURES" "$frame_macro")" || exit 1
    depth="$(scrape_macro "$FIGURES" "$depth_macro")" || exit 1
    zone=$((frame + depth))
    echo "trap_redzone: class $cls  $frame_macro=$frame  $depth_macro=$depth  zone=$zone ($onstack stack)"
    ENFORCED_ARGS="$ENFORCED_ARGS --enforced $cls=$frame,$depth"
    # THE MIRROR OF THE stack=kernel SKIP BELOW, and the reason it has to exist is that an
    # arch can compile TWO entry designs (armv7m does) while the call graph sees only one
    # merged tree. A kstacks= class belongs to ONE of them: where KICKOS_KERNEL_STACKS
    # disagrees, the image does not contain the path this class describes, so its DEPTH is a
    # measurement of the same C landing on a different stack and enforcing it charges the
    # board for a design it never links. Both halves go unenforced, and the tool is told so
    # it does not fail the depth either. PRINTED, never silent: a run that skipped every
    # registered preset of an arch would leave the figure unenforced, which one preset per
    # run cannot see.
    if [ "$kstacks" != any ] && [ "$kstacks" -ne "$KSTACKS" ]; then
        echo "trap_redzone: class $cls not enforced here (KICKOS_KERNEL_STACKS=$KSTACKS,
    this class is the kstacks=$kstacks design): MEASURED AND NOT ENFORCED, it describes an
    entry design this image does not compile"
        NOTCOMPILED_ARGS="$NOTCOMPILED_ARGS --not-compiled $cls"
        continue
    fi
    # A stack=trap class spends a static kernel array, not a thread stack, so the floor says
    # nothing about it. Skipped by NAME rather than by the figure happening to fit, or the
    # clause would start passing for the wrong reason the day the array grew.
    if [ "$onstack" = trap ]; then
        continue
    fi
    # A stack=kernel class spends the interrupted thread's own kernel block, so the floor
    # says nothing about it either. What it has to fit is that block, minus the canary word
    # at the bottom of it, and that is a clause of its own: raising the spawn floor would do
    # nothing here, and the array is per thread SLOT, so a byte costs KICKOS_THREAD_SLOTS.
    # A CLASS THE BOARD DOES NOT COMPILE IS MEASURED AND NOT ENFORCED, both halves, exactly
    # as the kstacks= skip above drops both. An arch may carry two entry designs (armv7m
    # does) and KICKOS_KERNEL_STACKS picks one per board, so at 0 there is no block to fit
    # AND the depth is the same C measured landing on a stack this image never puts it on.
    # THE DEPTH USED TO BE ENFORCED HERE ANYWAY, which made the clause print that it was not
    # enforcing a class and then fail on it: f302nucleo-st measures SVCK 680 against 768 with
    # 88 bytes of margin, unexcluded, through its own console driver's panic tail, and the
    # remedy that failure prints is to raise KICKOS_ARMV7M_TRAP_KERNEL_DEPTH_SVCK, which
    # raises KICKOS_KERNEL_STACK_SIZE for all 30 kstacks=1 presets. A board compiling no
    # blocks could force a fleet-wide ceiling raise, which is the same charge-twice the
    # kstacks= marker exists to refuse.
    #
    # NOTHING GOES QUIET: trap_redzone.py still reports the reading under `measured and NOT
    # enforced here`, and the 30 presets that DO compile blocks still enforce the figure. An
    # arch whose entry can only run on blocks says so in C instead: armv6m, rv32imac and rxv3
    # carry static_assert(KICKOS_KERNEL_STACKS != 0) on every image of every board. armv7m
    # carries no such assert and must not, one of its chips resolving 0 on purpose, so on
    # that arch what keeps this class honest is that its HAS_MPU presets resolve 1
    # unconditionally and this skip is printed rather than silent.
    if [ "$onstack" = kernel ] && [ "$KSTACKS" -eq 0 ]; then
        echo "trap_redzone: class $cls not enforced here (KICKOS_KERNEL_STACKS=0):
    MEASURED AND NOT ENFORCED, it describes an entry design this image does not compile"
        NOTCOMPILED_ARGS="$NOTCOMPILED_ARGS --not-compiled $cls"
        continue
    fi
    if [ "$onstack" = kernel ]; then
        if [ "$zone" -gt "$KUSABLE" ]; then
            bad "KERNEL STACK BELOW ITS REQUIREMENT: $ARCH needs $zone bytes of a $cls trap's
    kernel stack ($frame_macro=$frame + $depth_macro=$depth) but KICKOS_KERNEL_STACK_SIZE is
    $KSIZE, of which $KUSABLE is stack: the lowest word is the overflow canary. The deepest
    legitimate descent of that trap overwrites the canary, or the block, and the first report
    of it is a runtime canary failure on whichever board goes deepest.
    REMEDY: raise the per-arch default in $SRC/Kconfig (config KICKOS_KERNEL_STACK_SIZE,
    'default <n> if ARCH_$(printf '%s' "$ARCH" | tr '[:lower:]' '[:upper:]')') from $KSIZE to
    at least $((zone + KCANARY)). Do NOT shrink the depth to fit: it is a measurement."
        fi
        continue
    fi
    # Own clause, and NOT the same failure as an over-deep measurement: here the
    # measurement and the entry agree, and the SPAWN FLOOR is the thing that is wrong. The
    # CONSEQUENCE is per class and both halves are named, because a thread-stack class whose
    # sp the entry never bounds (an M-mode caller) cannot be refused at all.
    if [ "$zone" -gt "$FLOOR" ]; then
        bad "SPAWN FLOOR BELOW A THREAD-STACK REQUIREMENT: $ARCH needs $zone bytes of the
    interrupted thread's own stack for a $cls trap ($frame_macro=$frame + $depth_macro=$depth)
    but KICKOS_MIN_STACK_SIZE is $FLOOR. A thread spawned at the floor passes the spawn check
    and can then never complete that trap: where the entry bounds the sp it is REFUSED, every
    time, for the life of the thread, and where the entry applies no bound the kernel
    OVERFLOWS that thread's stack instead.
    REMEDY: raise the per-arch default in $SRC/Kconfig (config KICKOS_MIN_STACK_SIZE,
    'default <n> if ARCH_$(printf '%s' "$ARCH" | tr '[:lower:]' '[:upper:]')') from $FLOOR
    to at least $zone. Do NOT shrink the requirement to fit: it is a measurement."
    fi
done < "$TMP/classes"

# --- the measurement ---------------------------------------------------------
# shellcheck disable=SC2086
python3 "$TOOL" --ci-dir "$BUILD" --arch "$ARCH" --preset "$PRESET" \
    --roots "$ROOTS" --indirect "$INDIRECT" $ENFORCED_ARGS $NOTCOMPILED_ARGS
prc=$?
if [ "$prc" -ne 0 ]; then
    rc="$prc"
fi

if [ "$rc" -eq 0 ]; then
    echo "trap_redzone: OK ($PRESET/$ARCH, floor $FLOOR)"
fi
exit "$rc"
