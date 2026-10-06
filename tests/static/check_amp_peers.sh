#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# An own-image AMP node's image captured alone runs alone: tools/bench/amp_peers.sh derives the
# other nodes' flash text windows from planted configurations, tools/flash-picotool.sh erases
# each ahead of the load over a stub picotool, and every picotool load the capture runs takes them.

set -u
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root
scratch_dir
rc=0
. tools/bench/amp_peers.sh

# <name> <own> <nodes> <id> <base> <share>: a planted kickos_config.cmake.
config() {
    {
        [ -z "$2" ] || printf 'set(KICKOS_AMP_OWN_IMAGE %s)\n' "$2"
        printf 'set(KICKOS_AMP_NODES %s)\nset(KICKOS_AMP_NODE_ID %s)\n' "$3" "$4"
        printf 'set(KICKOS_AMP_TEXT_BASE %s)\nset(KICKOS_AMP_TEXT_SHARE %s)\n' "$5" "$6"
    } > "$TMP/$1.cmake"
}

# <name> <want>: amp_peer_text over the planted <name> prints <want>.
peers() {
    local got
    got=$(amp_peer_text "$TMP/$1.cmake" | paste -sd ' ' -)
    [ "$got" = "$2" ] || bad "the $1 configuration's peer windows read [$got], not [$2]"
}

config n0 1 2 0 268435456 1048576
peers n0 '0x10100000:0x10200000'
config n1 1 2 1 268435456 1048576
peers n1 '0x10000000:0x10100000'
config n1of3 1 3 1 268435456 1048576
peers n1of3 '0x10000000:0x10100000 0x10200000:0x10300000'
config ram 1 2 0 0 0
peers ram ''
config none 0 0 0 0 0
peers none ''
config unstated '' 2 0 268435456 1048576
if amp_peer_text "$TMP/unstated.cmake" > "$TMP/why"; then
    bad "a configuration stating no AMP posture is read as one"
fi
if amp_peer_text "$TMP/absent.cmake" > "$TMP/why"; then
    bad "an absent configuration is read as one"
fi

mkdir -p "$TMP/bin"
cat > "$TMP/bin/picotool" <<'STUB'
#!/bin/sh
echo "$*" >> "$STUB_LOG"
STUB
chmod +x "$TMP/bin/picotool"
: > "$TMP/image"
STUB_LOG="$TMP/calls" PATH="$TMP/bin:$PATH" FLASH_ERASE_RANGES='0x10100000:0x10200000 0x10300000:0x10400000' \
    FLASH_IMAGE="$TMP/image" bash tools/flash-picotool.sh pizero2350 planted > "$TMP/flash.out" 2>&1 \
    || bad "flash-picotool.sh fails over the stub: $(tail -n 1 "$TMP/flash.out")"
want="erase -r 0x10100000 0x10200000
erase -r 0x10300000 0x10400000
load -x -t elf $TMP/image"
[ "$(cat "$TMP/calls" 2>/dev/null)" = "$want" ] \
    || bad "flash-picotool.sh runs [$(cat "$TMP/calls" 2>/dev/null)], not the erases ahead of the load"

loads=$(grep -c 'tools/flash-picotool.sh' tools/bench/bench-capture.sh)
[ "$loads" -gt 0 ] || bad "bench-capture.sh runs no picotool load, so nothing here is read"
grep -n 'tools/flash-picotool.sh' tools/bench/bench-capture.sh \
    | grep -v 'FLASH_ERASE_RANGES="${PEER_ERASE:-}"' > "$TMP/bare"
while read -r line; do
    bad "bench-capture.sh:$line loads without the peer windows erased"
done < "$TMP/bare"

[ "$rc" -eq 0 ] || exit 1
echo "PASS: an own-image AMP node captured alone has the other nodes' flash windows erased first"
