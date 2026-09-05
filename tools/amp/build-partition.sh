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
# tests/static/check_amp_two_elf.sh refuses a pair that did not.

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

ELFS="$NODE0_ELF"
node=1
for target in "$@"; do
    bdir="$WORK/node$node"
    echo "== partition: configuring node $node ($target) =="
    "$CMAKE" -S "$SRC" -B "$bdir" -G "$GEN" \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DKICKOS_BOARD="$BOARD" -DKICKOS_CONFIG_VARIANT="$VARIANT" \
        -DKICKOS_AMP_NODE_ID="$node" >/dev/null \
        || { echo "build-partition.sh: node $node configure failed" >&2; exit 1; }
    echo "== partition: building node $node ($target) =="
    "$CMAKE" --build "$bdir" --target "$target" >/dev/null \
        || { echo "build-partition.sh: node $node build failed" >&2; exit 1; }
    # Located rather than spelled: an app's output path is the build's business.
    elf="$(find "$bdir" -type f -name "$target" -perm -u+x | head -1)"
    [ -n "$elf" ] || { echo "build-partition.sh: node $node built no $target" >&2; exit 1; }
    ELFS="$ELFS $elf"
    node=$((node + 1))
done

# shellcheck disable=SC2086
"$(dirname "$0")/merge-partition.sh" "$OBJCOPY" "$LD" "$READELF" "$OBJDUMP" "$OUT" $ELFS
