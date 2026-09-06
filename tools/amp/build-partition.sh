#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Build every PEER node of an AMP partition and merge the whole partition into one artefact,
# so a user flashes once (docs/design-multicore.md N6b).
#
# usage: build-partition.sh <cmake> <generator> <src> <work> <objcopy> <ld> <readelf> <objdump>
#                           <toolchain> <build-type> <out.elf> <board> <variant> <node0.elf>
#                           <peer-target>...
#
# The toolchain and the build type are PASSED, not assumed: a peer built from a different
# toolchain than node 0 is a partition whose halves no gate compares.
#
# The peer targets are given IN NODE ORDER starting at node 1. Node 0's own ELF is already
# built: this runs inside node 0's build, which is the node that releases the others at boot.
#
# Each peer is configured from the SAME source and the SAME variant, differing only in
# KICKOS_AMP_NODE_ID: the geometry and the port list are stated once in the shared defconfig,
# and every node derives its own share, link base and capabilities from its index (N6c, N6g).
# tests/static/check_amp_elf_agree.sh refuses a pair that did not.

set -eu

CMAKE="${1:?usage: build-partition.sh <cmake> <generator> <src> <work> ...}"
GEN="${2:?}"
SRC="${3:?}"
WORK="${4:?}"
OBJCOPY="${5:?}"
LD="${6:?}"
READELF="${7:?}"
OBJDUMP="${8:?}"
TOOLCHAIN="${9:?}"
BUILD_TYPE="${10:?}"
OUT="${11:?}"
BOARD="${12:?}"
VARIANT="${13:?}"
NODE0_ELF="${14:?}"
shift 14
[ "$#" -ge 1 ] || { echo "build-partition.sh: no peer target given" >&2; exit 1; }

[ -f "$NODE0_ELF" ] || { echo "build-partition.sh: no node 0 image at $NODE0_ELF" >&2; exit 1; }

# A peer is seeded from node 0's resolved configuration in TWO layers, neither covering the
# other.
#
# The resolved .config, through the seed file node 0's own configure writes: it carries every
# knob whatever its spelling, including one CMake forwards under its Kconfig name
# (SCHED_PERIODIC_TICK is one), which a sweep for KICKOS_* cache entries does not see.
#
# And node 0's KICKOS_* cache entries: a knob that is no Kconfig symbol at all reaches the build
# as a cache variable and appears in no .config. KICKOS_APPDATA_SIZE is one, stated in a preset
# and read by the linker script. Loaded as -D, so applied after the seed; genconfig.py refuses a
# configure where a name present in both disagrees.
#
# KICKOS_AMP_NODE_ID is left out of both, being the only thing a node may differ in.
NODE0_BUILD="$(dirname "$WORK")"
[ -f "$NODE0_BUILD/CMakeCache.txt" ] || {
    echo "build-partition.sh: no CMakeCache.txt at $NODE0_BUILD: this script takes the peer" >&2
    echo "  work directory to sit directly inside node 0's build tree, and it does not" >&2
    exit 1
}
# One KEY=VALUE per line, kept unflattened all the way to the argument list below: a cache
# value carrying whitespace, a build path among them, re-splits into several -D arguments and
# the peer silently takes its own default for whatever the split mangled.
INHERIT_LINES="$(sed -n -e '/^KICKOS_AMP_NODE_ID:/d' \
    -e 's/^\(KICKOS_[A-Z0-9_]*\):[A-Z]*=\(.*\)$/\1=\2/p' "$NODE0_BUILD/CMakeCache.txt")"

SEED="$NODE0_BUILD/generated/amp-peer-seed.cmake"
[ -f "$SEED" ] || {
    echo "build-partition.sh: node 0's build wrote no peer seed at $SEED. It is written by" >&2
    echo "  the own-image AMP block of the root CMakeLists.txt from the resolved .config, so" >&2
    echo "  either that build is not an own-image node 0 or its configure is stale." >&2
    exit 1
}

# The Kconfig fingerprint an image carries, as the two absolute symbols its link defines. Read
# from the ARTEFACT rather than from the tree that produced it: a stale or foreign image is
# exactly the case a build-tree comparison cannot see.
#
# It hashes the resolved .config alone, so it does NOT separate two images differing only in a
# KICKOS_* cache entry that is no Kconfig symbol. Those are carried by the cache sweep above,
# which builds every peer FROM node 0's entries; the fingerprint is what covers the half a
# re-configure can move under a peer that is already built.
image_fp() { # <elf>
    "$READELF" -sW "$1" 2>/dev/null | awk '
        $8 == "kickos_amp_config_fp_hi" { hi = $2 }
        $8 == "kickos_amp_config_fp_lo" { lo = $2 }
        END { if (hi != "" && lo != "") { print hi lo } }'
}

NODE0_FP="$(image_fp "$NODE0_ELF")"
[ -n "$NODE0_FP" ] || {
    echo "build-partition.sh: $NODE0_ELF carries no Kconfig fingerprint, so the" >&2
    echo "  agreement below would pass on every peer without comparing anything. The link" >&2
    echo "  defines kickos_amp_config_fp_hi and _lo on every own-image AMP node." >&2
    exit 1
}

# `set --` inside a function replaces the FUNCTION's positional parameters, so the peer targets
# the caller holds in "$@" are untouched by it. That is what lets one list be built as separate
# arguments while the other is still being iterated.
configure_node() { # <build dir> <node index>; the inherited cache entries arrive on stdin
    _bdir="$1"
    _node="$2"
    set --
    while IFS= read -r _kv; do
        [ -n "$_kv" ] || continue
        set -- "$@" "-D$_kv"
    done
    "$CMAKE" -S "$SRC" -B "$_bdir" -G "$GEN" -C "$SEED" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DKICKOS_BOARD="$BOARD" -DKICKOS_CONFIG_VARIANT="$VARIANT" \
        "$@" \
        -DKICKOS_AMP_NODE_ID="$_node" >/dev/null
}

merge_nodes() { # the node ELFs arrive on stdin, one path per line, in node order
    set -- "$OBJCOPY" "$LD" "$READELF" "$OBJDUMP" "$OUT"
    while IFS= read -r _elf; do
        [ -n "$_elf" ] || continue
        set -- "$@" "$_elf"
    done
    "$(dirname "$0")/merge-partition.sh" "$@"
}

ELF_LINES="$NODE0_ELF"
node=1
for target in "$@"; do
    bdir="$WORK/node$node"
    echo "== partition: configuring node $node ($target) =="
    configure_node "$bdir" "$node" <<EOF || { echo "build-partition.sh: node $node configure failed" >&2; exit 1; }
$INHERIT_LINES
EOF
    echo "== partition: building node $node ($target) =="
    "$CMAKE" --build "$bdir" --target "$target" >/dev/null \
        || { echo "build-partition.sh: node $node build failed" >&2; exit 1; }
    # Located rather than spelled: an app's output path is the build's business.
    elf="$(find "$bdir" -type f -name "$target" -perm -u+x | head -1)"
    [ -n "$elf" ] || { echo "build-partition.sh: node $node built no $target" >&2; exit 1; }
    peer_fp="$(image_fp "$elf")"
    if [ "$peer_fp" != "$NODE0_FP" ]; then
        echo "build-partition.sh: node $node was built from a DIFFERENT Kconfig state than" >&2
        echo "  node 0. Fingerprints over the resolved .config with the node index taken out:" >&2
        echo "    node 0: ${NODE0_FP:-none}" >&2
        echo "    node $node: ${peer_fp:-none}" >&2
        echo "  Nothing a link sees compares them and nothing at boot reports it, so this" >&2
        echo "  refuses here. Diff $NODE0_BUILD/generated/.config against" >&2
        echo "  $bdir/generated/.config for the knob that differs." >&2
        exit 1
    fi
    echo "== partition: node $node agrees with node 0 on Kconfig $peer_fp =="
    ELF_LINES="$ELF_LINES
$elf"
    node=$((node + 1))
done

merge_nodes <<EOF
$ELF_LINES
EOF
