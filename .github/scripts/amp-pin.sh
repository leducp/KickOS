#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Posture pin for an AMP preset, read from the RESOLVED partition.
#
# usage: amp-pin.sh <build-dir> <expected node index> <shared|own>
#
# Every AMP gate is registered by a CMake clause keyed on the posture, so a preset that lost it
# does not FAIL those gates, it stops registering them, and ctest passes on a run that covered
# the plain arm64 posture under another name.
#
# Non-circular by construction: the expected values are LITERALS the caller states, and the file
# read is the partition's own generated description rather than anything that derives with the
# posture.

set -eu

BUILD="${1:?usage: amp-pin.sh <build-dir> <node-index> <shared|own>}"
WANT_NODE="${2:?}"
WANT_POSTURE="${3:?}"

PORTS="$BUILD/generated/include/kickos/config/amp_ports.h"
[ -f "$PORTS" ] || { echo "amp-pin: $PORTS was not generated: this build has no partition"; exit 1; }

read_def() { # <macro>
    sed -n "s/^#define $1 \\(.*\\)\$/\\1/p" "$PORTS" | tail -1
}

COUNT="$(read_def KICKOS_AMP_PORT_COUNT)"
SELF="$(read_def KICKOS_AMP_SELF_NODE)"

if [ -z "$COUNT" ] || [ "$((COUNT))" -eq 0 ]; then
    echo "amp-pin: $BUILD states 0 partition crossing(s): every AMP gate de-registers here and"
    echo "         this step would re-run the plain arm64 posture under another name"
    exit 1
fi
if [ "$((SELF))" -ne "$((WANT_NODE))" ]; then
    echo "amp-pin: $BUILD is built as node $SELF and this step needs node $WANT_NODE."
    echo "         A partition whose peer is a second node ZERO links, boots, and passes every"
    echo "         arm that runs on node 0 (docs/design-multicore.md N6c)."
    exit 1
fi

CFG="$BUILD/generated/.config"
[ -f "$CFG" ] || { echo "amp-pin: no resolved Kconfig at $CFG"; exit 1; }
OWN="$(sed -n 's/^CONFIG_KICKOS_AMP_OWN_IMAGE=\(.*\)$/\1/p' "$CFG" | tail -1)"
case "$WANT_POSTURE" in
    own)
        [ "$OWN" = 1 ] || { echo "amp-pin: $BUILD is not the own-image posture (OWN_IMAGE='$OWN')"; exit 1; }
        ;;
    shared)
        [ "$OWN" = 0 ] || { echo "amp-pin: $BUILD is not the shared-image posture (OWN_IMAGE='$OWN')"; exit 1; }
        ;;
    *)
        echo "amp-pin: unknown posture '$WANT_POSTURE'"; exit 1 ;;
esac

echo "amp-pin: $BUILD is node $SELF of a $WANT_POSTURE-image partition naming $COUNT crossing(s)"
