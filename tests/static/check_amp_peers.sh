#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# An own-image AMP node's image captured alone runs alone: tools/bench/amp_peers.sh derives the
# other nodes' flash text windows from planted configurations, tools/flash-picotool.sh erases
# each ahead of the load over a stub picotool, and every picotool load the capture runs takes them.
# A picotool with no erase (1.x) and a window that is not whole flash sectors are refused before
# any erase or load, and a board whose load is not picotool's refuses windows to erase.

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
if [ "$1" = version ]; then
    echo "$STUB_VERSION"
    exit 0
fi
echo "$*" >> "$STUB_LOG"
STUB
chmod +x "$TMP/bin/picotool"
: > "$TMP/image"
# <name> <picotool version line> <windows>: flash-picotool.sh over the stub, its calls in
# $TMP/<name>.calls and its words in $TMP/<name>.out.
load() {
    : > "$TMP/$1.calls"
    STUB_LOG="$TMP/$1.calls" STUB_VERSION="$2" PATH="$TMP/bin:$PATH" FLASH_ERASE_RANGES="$3" \
        FLASH_IMAGE="$TMP/image" bash tools/flash-picotool.sh pizero2350 planted > "$TMP/$1.out" 2>&1
}
V2='picotool v2.1.1 (Linux, GNU-14.2.0, Release)'
load erased "$V2" '0x10100000:0x10200000 0x10300000:0x10400000' \
    || bad "flash-picotool.sh fails over the stub: $(tail -n 1 "$TMP/erased.out")"
want="erase -r 0x10100000 0x10200000
erase -r 0x10300000 0x10400000
load -x -t elf $TMP/image"
[ "$(cat "$TMP/erased.calls")" = "$want" ] \
    || bad "flash-picotool.sh runs [$(cat "$TMP/erased.calls")], not the erases ahead of the load"
# <name> <picotool version line> <windows> <word>: refused before any erase or load, saying <word>.
unloaded() {
    if load "$1" "$2" "$3"; then
        bad "flash-picotool.sh takes the $1 load"
    elif ! grep -qF -e "$4" "$TMP/$1.out"; then
        bad "flash-picotool.sh refuses the $1 load, but not for '$4': $(tail -n 1 "$TMP/$1.out")"
    fi
    [ ! -s "$TMP/$1.calls" ] || bad "the refused $1 load still runs [$(cat "$TMP/$1.calls")]"
}
unloaded picotool1 'picotool v1.1.2 (Linux, GNU-12.2.0, Release)' '0x10100000:0x10200000' \
    'needs picotool 2.x'
unloaded noversion 'ERROR: Unknown command: version' '0x10100000:0x10200000' 'needs picotool 2.x'
unloaded unaligned "$V2" '0x10100000:0x10200000 0x10300800:0x10400000' 'whole 4096-byte flash sectors'
unloaded unalignedend "$V2" '0x10100000:0x10100800' 'whole 4096-byte flash sectors'
unloaded empty "$V2" '0x10100000:0x10100000' 'whole 4096-byte flash sectors'
unloaded decimal "$V2" '268435456:269484032' 'not <from>:<to> in hex'
load plain 'picotool v1.1.2 (Linux, GNU-12.2.0, Release)' '' \
    || bad "a load with no window to erase is refused over picotool 1.x: $(tail -n 1 "$TMP/plain.out")"

# A route whose load erases nothing refuses windows to erase.
printf 'RIG_CONSOLE_F302NUCLEO=%s\n' "$TMP/no-such-console" > "$TMP/rig.conf"
if PEER_ERASE='0x10100000:0x10200000' KICKOS_RIG="$TMP/rig.conf" BENCH_HOST= \
       bash tools/bench/bench-capture.sh f302nucleo planted "$TMP/image" "$TMP/st.log" \
       > "$TMP/st.out" 2>&1; then
    bad "an f302nucleo capture with peer windows to erase is accepted"
elif ! grep -qF "and f302nucleo's load erases none" "$TMP/st.out"; then
    bad "an f302nucleo capture with peer windows to erase is refused, but not for them: $(grep REFUSING "$TMP/st.out")"
fi

loads=$(grep -c 'tools/flash-picotool.sh' tools/bench/bench-capture.sh)
[ "$loads" -gt 0 ] || bad "bench-capture.sh runs no picotool load, so nothing here is read"
# Matched as the call site spells it: a reformatted call fails here until this is updated.
grep -n 'tools/flash-picotool.sh' tools/bench/bench-capture.sh \
    | grep -v 'FLASH_ERASE_RANGES="${PEER_ERASE:-}"' > "$TMP/bare"
while read -r line; do
    bad "bench-capture.sh:$line loads without the peer windows erased"
done < "$TMP/bare"

[ "$rc" -eq 0 ] || exit 1
echo "PASS: an own-image AMP node captured alone has the other nodes' flash windows erased first"
