#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Flash a KickOS image to a SEGGER J-Link target (incl. an OpenSDA reflashed with
# J-Link-OpenSDA firmware). Needs the J-Link Software Pack on PATH (JLinkExe).
#
# Usage: tools/flash-jlink.sh <image> [board]
#   <image>  a KickOS build artifact:
#              .elf / .hex  -> load addresses are self-contained (loadfile) -- preferred
#              .bin         -> flashed at the board's flash base (table below)
#   [board]  target key selecting the J-Link device string + the .bin flash base.
#            Default: k64f. The flash base differs per silicon -- Kinetis is at
#            0x00000000, the XMC cached-flash alias is at 0x08000000 -- so a raw
#            .bin MUST go to the right one; this table takes care of it.
#
# Add a board: one row in the case below (device string per docs/flashing.md).
set -eu

img=${1:?usage: flash-jlink.sh <image.elf|.hex|.bin> [k64f|xmc4800]}
board=${2:-k64f}
[ -f "$img" ] || { echo "flash-jlink: no such image: $img" >&2; exit 1; }

case "$board" in
    k64f|frdmk64f)         dev=MK64FN1M0xxx12; base=0x00000000 ;; # Kinetis: flash @ 0
    xmc4800|xmc4800-relax) dev=XMC4800-2048;   base=0x08000000 ;; # XMC: cached-flash alias
    *) echo "flash-jlink: unknown board '$board' (known: k64f, xmc4800)" >&2; exit 1 ;;
esac

# A .bin carries no address, so it needs the per-board base; a .hex/.elf (or the
# extension-less linked ELF) carries its own load addresses -> loadfile, no base.
script=$(mktemp)
trap 'rm -f "$script"' EXIT
case "$img" in
    *.bin) printf 'loadbin %s %s\nr\ng\nq\n' "$img" "$base" > "$script"
           echo "flash-jlink: $img -> $board [$dev] @ $base" ;;
    *)     printf 'loadfile %s\nr\ng\nq\n' "$img" > "$script"
           echo "flash-jlink: $img -> $board [$dev] (addresses from the file)" ;;
esac

exec JLinkExe -device "$dev" -if SWD -speed 4000 -autoconnect 1 -CommanderScript "$script"
