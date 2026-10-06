#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The refusals of a system target (docs/design-m10-target.md, sections 5 and 6), each an image
# built out of tree against the build's installed package (tests/lib/system_link), which must
# fail naming its rule:
#
#   check_system_link.sh <kickos-build> <kickos-source> <cmake> <arm>
#
#   none      KickOS::kernel with no system target: the link names kickos_link_one_system_target,
#             and no undefined symbol of the chip script's comes first
#   two       two system targets: the symbol is defined twice
#   entry     a composition naming an entry no source defines: the link names it
#   stack     a stack at KICKOS_MIN_STACK_SIZE in an image with a thread-local object: the
#             thread-local assert
#   arena     a stack the chip file's arena holds and the linked image's does not: the arena assert
#   heap      a `heap` past what the image carves for it: the heap assert
#   noheap    a system target whose link defines no heap: the assert tying the heap to it
#   refused   a task above the build's priority range: the configure fails with the tool's line,
#             and says the tool refused it
#   notool    a host tool that cannot run: the configure fails saying so
#   nocc      a project that does not enable C: kickos_compose refuses it at configure
#   rerun     no arm of the link: an unchanged configure does not run the host tool, and a
#             changed composition does
#
# Every composition is the package's default one with one field changed, so each arm differs
# from a system the build itself links by that field alone.

set -u
. "$(dirname "$0")/../lib/gate.sh"

USAGE="usage: check_system_link.sh <kickos-build> <kickos-source> <cmake> none|two|entry|stack|arena|heap|noheap|refused|notool|nocc|rerun"
KICKOS_BUILD="${1:?$USAGE}"
KICKOS_SRC="${2:?$USAGE}"
CMAKE="${3:?$USAGE}"
ARM="${4:?$USAGE}"

scratch_dir

"$CMAKE" --install "$KICKOS_BUILD" --prefix "$TMP/prefix" >/dev/null || fail "cmake --install failed"
package_toolchain "$TMP/prefix"
KICKOS_TOOLCHAIN=$(sed -n 's/^KICKOS_TOOLCHAIN:PATH=//p' "$KICKOS_BUILD/CMakeCache.txt")
[ -n "$KICKOS_TOOLCHAIN" ] || fail "the build's cache names no KICKOS_TOOLCHAIN"
export KICKOS_TOOLCHAIN
NINJA="$(sed -n 's/^CMAKE_MAKE_PROGRAM:[^=]*=//p' "$KICKOS_BUILD/CMakeCache.txt")"
BOARD="$(sed -n 's/^KICKOS_BOARD:[A-Z]*=//p' "$KICKOS_BUILD/CMakeCache.txt")"
PACKAGE="$TMP/prefix/lib/cmake/KickOS"
MANIFEST="$PACKAGE/manifest.yaml"
DEFAULT="$PACKAGE/boards/$BOARD/composition.yaml"
[ -f "$DEFAULT" ] || fail "the package installs no default composition at $DEFAULT"

manifest_number() { # <key>: the manifest's `<key>: <n>`, decimal or hexadecimal
    _mn="$(sed -n "s/^ *$1: *\\(0x[0-9a-fA-F]*\\|[0-9]*\\) *\$/\\1/p" "$MANIFEST" | head -n 1)"
    [ -n "$_mn" ] || fail "$MANIFEST states no $1"
    printf '%d' "$_mn"
}

# The default composition with the line matching <ere> replaced by <line>; the substitution must
# change exactly one line, or the arm would build the default.
COMPOSITION="$TMP/composition.yaml"
edited() { # <ere> <line>
    sed -E "s|$1|$2|" "$DEFAULT" > "$COMPOSITION"
    [ "$(diff "$DEFAULT" "$COMPOSITION" | grep -c '^>')" -eq 1 ] \
        || fail "the $ARM arm's edit '$1' changed no single line of $DEFAULT"
}

configure() { # <probe arm> <composition> <build dir> [cmake argument]...
    _cf_arm="$1"
    _cf_composition="$2"
    _cf_dir="$3"
    shift 3
    "$CMAKE" -S "$KICKOS_SRC/tests/lib/system_link" -B "$_cf_dir" -G Ninja \
        -DCMAKE_MAKE_PROGRAM="$NINJA" -DCMAKE_TOOLCHAIN_FILE="$PACKAGE_TC" \
        -DCMAKE_PREFIX_PATH="$TMP/prefix" -DKICKOS_PROBE_ARM="$_cf_arm" \
        -DKICKOS_PROBE_COMPOSITION="$_cf_composition" "$@" >"$_cf_dir.configure.log" 2>&1
}

if [ "$ARM" = rerun ]; then
    cp "$DEFAULT" "$COMPOSITION"
    configure composed "$COMPOSITION" "$TMP/build" || fail "the default composition did not configure"
    grep -q 'kickos_compose(probe_system): emitted ' "$TMP/build.configure.log" \
        || fail "the first configure did not run the host tool"
    configure composed "$COMPOSITION" "$TMP/build" || fail "the second configure failed"
    grep -q 'kickos_compose(probe_system): .* and its inputs unchanged' "$TMP/build.configure.log" \
        || fail "an unchanged configure ran the host tool again"
    printf '# a comment the tool reads past\n' >> "$COMPOSITION"
    configure composed "$COMPOSITION" "$TMP/build" || fail "the edited composition did not configure"
    grep -q 'kickos_compose(probe_system): emitted ' "$TMP/build.configure.log" \
        || fail "a changed composition did not run the host tool again"
    echo "PASS: the host tool ran on the first configure and on a changed input, not on an unchanged one"
    exit 0
fi

PROBE_ARM=composed
EXPECT=""
ALSO=""
EXTRA=""
case "$ARM" in
    none)
        PROBE_ARM=none
        EXPECT='required symbol [^ ]*kickos_link_one_system_target. not defined' ;;
    two)
        PROBE_ARM=two
        # Two copies of the default, which names no driver: two systems that differ on drivers
        # would also collide on the init's two driver paths.
        cp "$DEFAULT" "$COMPOSITION"
        EXPECT='multiple definition of [^ ]*kickos_link_one_system_target' ;;
    entry)
        edited '^( *entry: *)kickos_main' '\1kickos_absent_entry'
        # ld names the symbol as C spells it, with no ABI prefix (rx-elf's `_` included).
        EXPECT="undefined reference to [\`']kickos_absent_entry'" ;;
    stack)
        PROBE_ARM=tls
        edited '^( *stack: *)[0-9]+' "\\1$(manifest_number min_stack)"
        EXPECT="stack cannot hold the linked image's thread-local block" ;;
    arena)
        # S is the largest power of two that the chip file's arena base is aligned to and that
        # fits twice in that arena: admission places a stack of S, aligned to S, at base + S, and
        # it ends at or below the arena's end. The control image's linked arena starts past base
        # and ends below base + 2S, which is checked here, so the same stack lands at base + S or
        # later and ends past it.
        CHIP="$PACKAGE/$(sed -n 's/^  chip: *\(platform\/.*\)$/\1/p' "$MANIFEST" | head -n 1)"
        # The memory entry marked `arena: true`, one line whether written flow or block style.
        LINE="$(awk '/^memory:/ { m = 1; next } /^[^ #]/ { m = 0 } !m { next }
                     /^  [a-z]/ { if (e ~ /arena: *true/) { print e; exit } e = "" }
                     { e = e " " $0 } END { if (e ~ /arena: *true/) print e }' "$CHIP" | head -n 1)"
        BASE="$(printf '%s' "$LINE" | sed -n 's/.*base: *\(0x[0-9a-fA-F]*\|[0-9]*\).*/\1/p')"
        SIZE="$(printf '%s' "$LINE" | sed -n 's/.*size: *\(0x[0-9a-fA-F]*\|[0-9]*\).*/\1/p')"
        [ -n "$BASE" ] && [ -n "$SIZE" ] || fail "$CHIP states no arena base and size"
        BASE=$(printf '%d' "$BASE")
        SIZE=$(printf '%d' "$SIZE")
        [ "$(sed -n 's/^ *window_rule: *//p' "$MANIFEST")" = pow2 ] \
            || fail "the arena arm places its stack by the pow2 rule, which this build's manifest does not state"
        S=1
        while [ $((S * 4)) -le "$SIZE" ] && [ $((BASE % (S * 2))) -eq 0 ]; do
            S=$((S * 2))
        done
        cp "$DEFAULT" "$COMPOSITION"
        configure composed "$COMPOSITION" "$TMP/control" || fail "the control did not configure"
        "$CMAKE" --build "$TMP/control" >"$TMP/control.log" 2>&1 \
            || fail "the default composition did not link, so the arena arm has no control"
        NM="$(sed -n 's/^CMAKE_NM:FILEPATH=//p' "$TMP/control/CMakeCache.txt")"
        symbol() { # <name>: its value in the control image
            _sv="$("$NM" "$TMP/control/probe" | sed -n "s/^\([0-9a-fA-F]*\) . $1\$/\1/p")"
            [ -n "$_sv" ] || fail "the control image defines no $1"
            printf '%d' "0x$_sv"
        }
        RAM_START=$(symbol __kickos_ram_start)
        RAM_END=$(symbol __kickos_ram_end)
        [ "$RAM_START" -gt "$BASE" ] && [ "$RAM_END" -lt $((BASE + 2 * S)) ] \
            || fail "the control's arena [$RAM_START, $RAM_END) does not start past $BASE and end below \
$((BASE + 2 * S)), so a stack of $S need not fail on this board"
        edited '^( *stack: *)[0-9]+' "\\1$S"
        EXPECT="the arena cannot hold task .main.'s stack" ;;
    heap)
        edited '^heap: *[0-9]+' 'heap: 1048576'
        EXPECT="1048576-byte heap of $COMPOSITION, its .heap., is more than" ;;
    noheap)
        PROBE_ARM=noheap
        cp "$DEFAULT" "$COMPOSITION"
        EXPECT="KICKOS_USER_HEAP_SIZE is not the [0-9]+-byte heap of $COMPOSITION, which" ;;
    nocc)
        PROBE_ARM=nocc
        cp "$DEFAULT" "$COMPOSITION"
        EXPECT='kickos_compose\(probe_system\): the project does not enable C' ;;
    refused)
        TOP="$(sed -n 's/^ *priority: *\[[0-9]*, *\([0-9]*\)\] *$/\1/p' "$MANIFEST" | head -n 1)"
        [ -n "$TOP" ] || fail "$MANIFEST states no priority range"
        edited '^( *priority: *)[0-9]+' "\\1$((TOP + 1))"
        EXPECT='composition.yaml:[0-9]+: scheduling.priority: '
        ALSO='kickos_compose\(probe_system\): the host tool refused' ;;
    notool)
        # A uv that runs nothing: the tool never reads the composition.
        cp "$DEFAULT" "$COMPOSITION"
        EXTRA=-DKICKOS_UV=/bin/false
        EXPECT='kickos_compose\(probe_system\): could not run the host tool'
        ALSO='uv and Python >=' ;;
    *)
        fail "$USAGE" ;;
esac

if [ -n "$EXTRA" ]; then
    configure "$PROBE_ARM" "$COMPOSITION" "$TMP/build" "$EXTRA"
else
    configure "$PROBE_ARM" "$COMPOSITION" "$TMP/build"
fi
CONFIGURED=$?
if [ "$ARM" = refused ] || [ "$ARM" = notool ] || [ "$ARM" = nocc ]; then
    [ "$CONFIGURED" -ne 0 ] || fail "the $ARM arm configured"
    grep -qE "$EXPECT" "$TMP/build.configure.log" || {
        sed -n '/CMake Error/,$p' "$TMP/build.configure.log" | sed -n '1,20p' >&2
        fail "the configure failed without the tool's '$EXPECT' line (see above)"
    }
    if [ -n "$ALSO" ] && ! grep -qE "$ALSO" "$TMP/build.configure.log"; then
        sed -n '/CMake Error/,$p' "$TMP/build.configure.log" | sed -n '1,20p' >&2
        fail "the configure failed without '$ALSO' (see above)"
    fi
    echo "PASS: the $ARM arm failed the configure:"
    grep -m 1 -E "$EXPECT" "$TMP/build.configure.log"
    exit 0
fi
[ "$CONFIGURED" -eq 0 ] || {
    sed -n '/CMake Error/,$p' "$TMP/build.configure.log" | sed -n '1,20p' >&2
    fail "the $ARM arm's probe did not configure, so its link witnesses nothing (see above)"
}
if "$CMAKE" --build "$TMP/build" >"$TMP/build.log" 2>&1; then
    fail "the $ARM arm's image linked"
fi
# ld wraps no line, so the message is matched on the line it wrote, and ninja's echo of the failed
# command, which names the required symbol too, matches none of these.
grep -qE "$EXPECT" "$TMP/build.log" || {
    sed -n '1,30p' "$TMP/build.log" >&2
    fail "the $ARM arm's link failed without '$EXPECT' (see above)"
}
# Each system target names KickOS::init's objects, which the link takes once, so only the
# symbols each system target emits collide: its table's and its USB console mark's, and on a
# build carrying one its gate assignment's.
if [ "$ARM" = two ] && grep 'multiple definition' "$TMP/build.log" \
        | grep -qv 'definition of [^ ]*\(kickos_link_one_system_target\|kickos_table\|kickos_usb_device_console\|kickos_gate_rows\|kickos_gate_row_count\).'; then
    grep 'multiple definition' "$TMP/build.log" | sed -n '1,5p' >&2
    fail "the link took a system target's shared objects twice"
fi
if [ "$ARM" = none ] && grep -q 'referenced in expression' "$TMP/build.log"; then
    grep -m 1 'referenced in expression' "$TMP/build.log" >&2
    fail "the link stopped on a symbol of the chip script before it named its required one"
fi
echo "PASS: the $ARM arm's link failed naming its rule:"
grep -m 1 -E "$EXPECT" "$TMP/build.log"
