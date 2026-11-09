#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# flash-<tool> backend: flash an RP2040 via picotool (board must be in BOOTSEL).
# Usage: tools/flash-picotool.sh <board> [app]
# PICOTOOL_SN=<USB serial> selects the BOOTSEL device; with none, more than one RP device on the bus
# is refused, since picotool cannot choose between them.
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
SEL=()
if [ -n "${PICOTOOL_SN:-}" ]; then
    # picotool's --ser matches an RP2040 against its flash id, not the USB serial the bus shows.
    for dev in /sys/bus/usb/devices/*; do
        if [ "$(cat "$dev/idVendor" 2>/dev/null)" = 2e8a ] && [ "$(cat "$dev/serial" 2>/dev/null)" = "$PICOTOOL_SN" ]; then
            SEL=(--bus "$(cat "$dev/busnum")" --address "$(cat "$dev/devnum")")
            break
        fi
    done
    [ ${#SEL[@]} -gt 0 ] || [ -n "${DRY_RUN:-}" ] || die "no RP device with USB serial $PICOTOOL_SN is on the bus"
elif [ -z "${DRY_RUN:-}" ]; then
    found=$(picotool info 2>&1 || true)
    if printf '%s\n' "$found" | grep -q '^Multiple RP-series devices'; then
        die "more than one RP device is in BOOTSEL and no PICOTOOL_SN names $FL_BOARD's: $(printf '%s\n' "$found" | grep -E '^RP[0-9]+ device at' | paste -sd ';' -)"
    fi
fi
for range in ${FLASH_ERASE_RANGES:-}; do
    run picotool erase -r "${range%%:*}" "${range#*:}" "${SEL[@]}"
done
# -t elf: the KickOS image has no .elf extension, so force the type; picotool
# refuses to guess format from an extensionless name. -x: reboot into the app.
run picotool load -x -t elf "$FL_ELF" "${SEL[@]}"
