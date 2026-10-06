#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# flash-<tool> backend: flash an RP2040 via picotool (board must be in BOOTSEL).
# Usage: tools/flash-picotool.sh <board> [app]
set -euo pipefail
FL_ROOT=$(cd "$(dirname "$0")/.." && pwd); . "$FL_ROOT/tools/flash-common.sh"
flash_resolve "$@"
have picotool || die "picotool not on PATH"
# FLASH_ERASE_RANGES: `<from>:<to>` words, each erased ahead of the load. picotool's erase arrived
# with 2.0, and it clears whole 4 KiB flash sectors, so every window is checked before any erase.
SECTOR=4096
if [ -n "${FLASH_ERASE_RANGES:-}" ]; then
    version=$(picotool version 2>&1 | head -n 1 || true)
    major=$(printf '%s\n' "$version" | sed -n 's/^picotool v\([0-9][0-9]*\)\..*/\1/p')
    if [ -z "$major" ] || [ "$major" -lt 2 ]; then
        die "picotool [$version] has no erase, which needs picotool 2.x: the AMP peers' flash windows cannot be cleared"
    fi
    for range in $FLASH_ERASE_RANGES; do
        if ! [[ $range =~ ^0x[0-9a-fA-F]+:0x[0-9a-fA-F]+$ ]]; then
            die "erase window '$range' is not <from>:<to> in hex"
        fi
        from=$(( ${range%%:*} ))
        to=$(( ${range#*:} ))
        if [ "$to" -le "$from" ] || [ $((from % SECTOR)) -ne 0 ] || [ $((to % SECTOR)) -ne 0 ]; then
            die "erase window $range is not a run of whole $SECTOR-byte flash sectors"
        fi
    done
fi
for range in ${FLASH_ERASE_RANGES:-}; do
    run picotool erase -r "${range%%:*}" "${range#*:}"
done
# -t elf: the KickOS image has no .elf extension, so force the type; picotool
# refuses to guess format from an extensionless name. -x: reboot into the app.
run picotool load -x -t elf "$FL_ELF"
