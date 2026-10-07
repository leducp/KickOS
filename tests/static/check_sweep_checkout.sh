#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The main checkout the gate sweeps read their GTest prefix from (tools/sweep-common.sh), over
# planted repositories: from the main tree, from a linked worktree, from a bare repository's
# worktree and from no repository.

set -u
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root
scratch_dir
rc=0
. tools/sweep-common.sh

for sweep in tools/sweep_host_gates.sh tools/sweep_image_gates.sh; do
    # shellcheck disable=SC2016
    grep -qxF 'MAIN="$(main_checkout "$ROOT")"' "$sweep" \
        || bad "$sweep does not take its main checkout from main_checkout"
done

GIT_CONFIG_NOSYSTEM=1
GIT_CONFIG_GLOBAL=/dev/null
export GIT_CONFIG_NOSYSTEM GIT_CONFIG_GLOBAL
REAL_GIT="$(command -v git)" || fail "no git on PATH"
g() {
    "$REAL_GIT" -c user.name=planted -c user.email=planted@invalid -c init.defaultBranch=main "$@" \
        > /dev/null 2>&1 || fail "planting failed: git $*"
}
g init -q "$TMP/main"
g -C "$TMP/main" commit -q --allow-empty -m planted
g -C "$TMP/main" worktree add -q "$TMP/linked"
g clone -q --bare "$TMP/main" "$TMP/bare.git"
g -C "$TMP/bare.git" worktree add -q "$TMP/barewt"
mkdir -p "$TMP/plain"
P="$(cd "$TMP" && pwd -P)"

# <tree> <expected>
expect() {
    got="$(main_checkout "$1")"
    [ "$got" = "$2" ] || bad "the main checkout of $1 is [$got], not $2"
}
expect "$P/main" "$P/main"
expect "$P/linked" "$P/main"
expect "$P/barewt" "$P/barewt"
expect "$P/plain" "$P/plain"

[ "$rc" -eq 0 ] || exit 1
echo "PASS: the gate sweeps find the main checkout from every tree layout"
