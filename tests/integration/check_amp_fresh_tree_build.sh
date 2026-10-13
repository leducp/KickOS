#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The partition builds from a fresh tree: node 0's preset configured again, every target and the
# merged artefact built from it.
#
# usage: check_amp_fresh_tree_build.sh <src-dir> <cmake> <preset>

set -u
set -f
. "$(dirname "$0")/../lib/gate.sh"

if [ "$#" -ne 3 ]; then
    echo "usage: $0 <src-dir> <cmake> <preset>" >&2
    exit 2
fi
SRC="$1"
CMAKE="$2"
PRESET="$3"

[ -d "$SRC" ] || fail "source dir does not exist: $SRC"
command -v "$CMAKE" >/dev/null 2>&1 || [ -x "$CMAKE" ] || fail "cmake not executable: $CMAKE"

KOS_TRASH_DIR="$(mktemp -d "/var/tmp/kickos-amp-fresh-$PRESET.XXXXXX")" \
    || fail "mktemp -d under /var/tmp failed"
TMP="$KOS_TRASH_DIR"
kos_trap

"$CMAKE" -S "$SRC" -B "$TMP/b" --preset "$PRESET" > "$TMP/cfg.log" 2>&1 \
    || { tail -n 40 "$TMP/cfg.log" >&2; fail "$PRESET does not configure in a fresh tree"; }
for target in all amp_partition; do
    if ! "$CMAKE" --build "$TMP/b" --target "$target" > "$TMP/$target.log" 2>&1; then
        grep -E 'error|FAILED' "$TMP/$target.log" | sed -n '1,20p' >&2
        fail "$PRESET does not build $target in a fresh tree"
    fi
done

echo "PASS: $PRESET builds every target and its partition in a fresh tree"
