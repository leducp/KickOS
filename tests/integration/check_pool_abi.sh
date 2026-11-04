#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The kernel archives a package ships hold no pool: each system's own link carries its pools'
# storage, sized by the manifest it was composed against (tests/lib/pool_abi). Every consumer
# here links the same installed archives.
#
#   check_pool_abi.sh <kickos-build> <kickos-source> <cmake> two|skew
#
#   two   the package's manifest, and a copy whose thread pool holds two slots fewer: both images
#         run, and the second spawns two threads fewer before the kernel refuses one; and the
#         package's manifest over a pools TU narrowed by two slots below its budgets: its storage
#         links two slots smaller and its spawns stop two sooner at a full pool, not at a budget
#   skew  a consumer whose pools TU reads another thread-slot ceiling than the archives were built
#         with, one whose descriptor swaps two members of one size, and one whose budget kinds swap
#         two values: each link fails naming the layout the kernel asks for, where the same consumer
#         against the package's headers links

set -u
. "$(dirname "$0")/../lib/gate.sh"

USAGE="usage: check_pool_abi.sh <kickos-build> <kickos-source> <cmake> two|skew"
KICKOS_BUILD="${1:?$USAGE}"
KICKOS_SRC="${2:?$USAGE}"
CMAKE="${3:?$USAGE}"
ARM="${4:?$USAGE}"
case "$ARM" in
    two) need_qemu_machine ;;
    skew) ;;
    *) fail "$USAGE" ;;
esac

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
COMPOSITION="$PACKAGE/boards/$BOARD/composition.yaml"
[ -f "$COMPOSITION" ] || fail "the package installs no default composition at $COMPOSITION"
ARCHIVE="$TMP/prefix/lib/libkickos_kernel.a"
[ -f "$ARCHIVE" ] || fail "the package installs no kernel archive at $ARCHIVE"

consumer() { # <build dir> [cmake argument]...: configure and build tests/lib/pool_abi
    _c_dir="$1"
    shift
    "$CMAKE" -S "$KICKOS_SRC/tests/lib/pool_abi" -B "$_c_dir" -G Ninja \
        -DCMAKE_MAKE_PROGRAM="$NINJA" -DCMAKE_TOOLCHAIN_FILE="$PACKAGE_TC" \
        -DCMAKE_PREFIX_PATH="$TMP/prefix" -DKICKOS_PROBE_COMPOSITION="$COMPOSITION" "$@" \
        >"$_c_dir.configure.log" 2>&1 || {
        sed -n '/CMake Error/,$p' "$_c_dir.configure.log" | sed -n '1,30p' >&2
        fail "the consumer in $_c_dir did not configure (see above)"
    }
    "$CMAKE" --build "$_c_dir" >"$_c_dir.build.log" 2>&1
}

# The thread pool a consumer's system was sized to: the first figure of its pools TU.
pool_threads() { # <build dir>
    _pt="$(sed -n 's/^KICKOS_SYSTEM_POOLS(\([0-9][0-9]*\),.*/\1/p' \
        "$1/kickos_compose/probe_system/pools.cc")"
    [ -n "$_pt" ] || fail "$1 emitted no pools TU naming its thread pool"
    printf '%s' "$_pt"
}

# The kernel archive a consumer's image linked must be the package's.
linked_archive() { # <build dir>
    grep -qF "$ARCHIVE" "$1/build.ninja" || fail "$1 linked no kernel archive of $ARCHIVE"
}

if [ "$ARM" = skew ]; then
    consumer "$TMP/control" || {
        sed -n '1,30p' "$TMP/control.build.log" >&2
        fail "the consumer did not link against the package's own pool header"
    }
    linked_archive "$TMP/control"
    EXPECT='PoolLayout<[0-9]+[ul]*>::tag'
    refused() { # <name> <what>: the consumer reading the headers under $TMP/<name> fails to link
        if consumer "$TMP/$1-build" -DKICKOS_POOL_ABI_SKEW="$TMP/$1"; then
            fail "a consumer whose pools TU read $2 linked"
        fi
        grep -q 'probe_system_kernel.dir/.*pools[.]cc' "$TMP/$1-build.build.log" \
            || fail "the $1 consumer never compiled its pools TU, so its failure witnesses nothing"
        grep -qE "$EXPECT" "$TMP/$1-build.build.log" || {
            sed -n '1,30p' "$TMP/$1-build.build.log" >&2
            fail "the $1 consumer's link failed without naming '$EXPECT' (see above)"
        }
        echo "PASS: a pools TU that read $2 is refused by the link:"
        grep -m 1 -E "$EXPECT" "$TMP/$1-build.build.log"
    }
    lines_changed() { # <package header> <skewed copy> <lines>
        [ "$(diff "$1" "$2" | grep -c '^>')" -eq "$3" ] || fail "the skewed copy of $1 changed not $3 line(s)"
    }

    mkdir -p "$TMP/ceiling/kickos/config" || fail "cannot create $TMP/ceiling"
    HEADER="$TMP/prefix/include/kickos/config/pool_abi.h"
    CEILING="$(sed -n 's/^#define KICKOS_THREAD_SLOT_CEILING \([0-9][0-9]*\)$/\1/p' "$HEADER")"
    [ -n "$CEILING" ] || fail "$HEADER defines no KICKOS_THREAD_SLOT_CEILING"
    sed "s/^#define KICKOS_THREAD_SLOT_CEILING $CEILING\$/#define KICKOS_THREAD_SLOT_CEILING $((CEILING - 1))/" \
        "$HEADER" > "$TMP/ceiling/kickos/config/pool_abi.h"
    lines_changed "$HEADER" "$TMP/ceiling/kickos/config/pool_abi.h" 1
    refused ceiling "another thread-slot ceiling"

    mkdir -p "$TMP/field/kickos" || fail "cannot create $TMP/field"
    HEADER="$TMP/prefix/include/kickos/pools.h"
    awk '/^        uint32_t cap_chunk_count;$/ { print "        uint32_t cap_root_width;"; next }
         /^        uint32_t cap_root_width;$/ { print "        uint32_t cap_chunk_count;"; next } { print }' \
        "$HEADER" > "$TMP/field/kickos/pools.h"
    lines_changed "$HEADER" "$TMP/field/kickos/pools.h" 1
    refused field "a descriptor whose cap_chunk_count and cap_root_width trade places"

    mkdir -p "$TMP/kind/kickos/sys" || fail "cannot create $TMP/kind"
    HEADER="$TMP/prefix/include/kickos/sys/budget.h"
    awk '/^ *X\(ENDPOINTS, / { held = $0; next } { print } /^ *X\(NOTIFICATIONS, / { print held }' \
        "$HEADER" > "$TMP/kind/kickos/sys/budget.h"
    lines_changed "$HEADER" "$TMP/kind/kickos/sys/budget.h" 1
    refused kind "budget kinds whose ENDPOINTS and NOTIFICATIONS trade values"
    exit 0
fi

# The default THREADS budget, the line under `budgets:`'s `threads:`.
THREADS="$(awk '$0 == "  threads:" { kind = 1; next } kind && $1 == "default:" { print $2; exit } { kind = 0 }' "$MANIFEST")"
[ -n "$THREADS" ] || fail "$MANIFEST states no default THREADS budget"
FEWER="$PACKAGE/manifest_fewer.yaml"
awk -v n="$((THREADS - 2))" '$0 == "  threads:" { kind = 1; print; next }
    kind && $1 == "default:" { print "    default: " n; kind = 0; next } { kind = 0; print }' "$MANIFEST" > "$FEWER"
[ "$(diff "$MANIFEST" "$FEWER" | grep -c '^>')" -eq 1 ] \
    || fail "the second manifest changed no single line of $MANIFEST"

consumer "$TMP/full" || { sed -n '1,30p' "$TMP/full.build.log" >&2; fail "the first consumer did not link"; }
consumer "$TMP/fewer" -DKICKOS_POOL_ABI_MANIFEST="$FEWER" || {
    sed -n '1,30p' "$TMP/fewer.build.log" >&2
    fail "the second consumer did not link"
}
linked_archive "$TMP/full"
linked_archive "$TMP/fewer"
FULL_POOL="$(pool_threads "$TMP/full")"
FEWER_POOL="$(pool_threads "$TMP/fewer")"
[ "$FULL_POOL" -eq $((FEWER_POOL + 2)) ] \
    || fail "the two systems' thread pools are $FULL_POOL and $FEWER_POOL slots, not two apart"

spawned() { # <image>: run it, and set SPAWNED and REFUSED from its line
    _image="$1"
    if [ -f "$1.efi" ]; then
        _image="$1.efi"
    fi
    run_image "$_image" >/dev/null
    assert_no_panic "a panic in $1"
    status_clause "$1 ended the system with status 0" 0
    _line="$(printf '%s\n' "$OUT" | sed -n 's/^pool-abi: \([0-9][0-9]*\) threads, then \(-[0-9][0-9]*\)$/\1 \2/p' | head -n 1)"
    [ -n "$_line" ] || fail "$1 printed no pool-abi line"
    SPAWNED="${_line% *}"
    REFUSED="${_line#* }"
}
spawned "$TMP/full/probe"
FULL_SPAWNED="$SPAWNED"
FULL_REFUSED="$REFUSED"
spawned "$TMP/fewer/probe"
[ "$REFUSED" -eq "$FULL_REFUSED" ] \
    || fail "the two images' last spawn was refused with $FULL_REFUSED and $REFUSED"
[ "$FULL_SPAWNED" -eq $((SPAWNED + 2)) ] \
    || fail "the images spawned $FULL_SPAWNED and $SPAWNED threads, not two apart"
echo "PASS: two systems of $FULL_POOL and $FEWER_POOL thread slots link the package's archives and run,"
echo "  spawning $FULL_SPAWNED and $SPAWNED threads before the refusal $REFUSED"

# The system's pools storage as its image links it: the byte size of the pools TU's object.
storage_bytes() { # <build dir>
    _nm="$(sed -n 's/^CMAKE_NM:FILEPATH=//p' "$1/CMakeCache.txt")"
    [ -n "$_nm" ] || fail "$1's cache names no CMAKE_NM"
    _elf="$1/probe"
    if [ -f "$1/probe.efi.elf" ]; then
        _elf="$1/probe.efi.elf"
    fi
    _sb="$("$_nm" -S -C "$_elf" | sed -n 's/^[0-9a-fA-F]* \([0-9a-fA-F]*\) [bBdD] kickos::(anonymous namespace)::system_pools$/\1/p')"
    [ -n "$_sb" ] || fail "$_elf links no pools storage of its pools TU"
    printf '%d' "0x$_sb"
}

consumer "$TMP/narrow" || { sed -n '1,30p' "$TMP/narrow.build.log" >&2; fail "the narrow consumer did not link"; }
POOLS_TU="$TMP/narrow/kickos_compose/probe_system/pools.cc"
sed "s/^KICKOS_SYSTEM_POOLS($FULL_POOL,/KICKOS_SYSTEM_POOLS($((FULL_POOL - 2)),/" "$POOLS_TU" > "$POOLS_TU.narrow" \
    && mv "$POOLS_TU.narrow" "$POOLS_TU" || fail "cannot narrow $POOLS_TU"
"$CMAKE" --build "$TMP/narrow" >"$TMP/narrow.rebuild.log" 2>&1 || {
    sed -n '1,30p' "$TMP/narrow.rebuild.log" >&2
    fail "the narrowed consumer did not link"
}
[ "$(pool_threads "$TMP/narrow")" -eq $((FULL_POOL - 2)) ] || fail "the rebuild put back the pools TU's thread pool"
linked_archive "$TMP/narrow"
FULL_BYTES="$(storage_bytes "$TMP/full")"
NARROW_BYTES="$(storage_bytes "$TMP/narrow")"
[ "$NARROW_BYTES" -lt "$FULL_BYTES" ] \
    || fail "the narrowed system's pools storage links at $NARROW_BYTES bytes, the full one's at $FULL_BYTES"
spawned "$TMP/narrow/probe"
[ "$FULL_REFUSED" -eq -11 ] || fail "the full system's last spawn was refused with $FULL_REFUSED, not its budget's -11"
[ "$REFUSED" -eq -12 ] \
    || fail "the narrowed system's last spawn was refused with $REFUSED, not a full pool's -12"
[ "$FULL_SPAWNED" -eq $((SPAWNED + 2)) ] \
    || fail "the full and narrowed images spawned $FULL_SPAWNED and $SPAWNED threads, not two apart"
echo "PASS: a pools TU two slots narrower than its budgets links $NARROW_BYTES storage bytes, not $FULL_BYTES,"
echo "  and its kernel stops at $SPAWNED threads with a full pool's $REFUSED"
