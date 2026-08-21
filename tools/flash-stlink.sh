#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# flash-<tool> backend: flash an STM32 via st-flash (ST-Link). Flash alias 0x08000000.
# Usage: [STLINK_UNDER_RESET=1] tools/flash-stlink.sh <board> [app]
set -euo pipefail
FL_ROOT=$(cd "$(dirname "$0")/.." && pwd); . "$FL_ROOT/tools/flash-common.sh"
flash_resolve "$@"
have st-flash || die "st-flash not on PATH (install stlink-tools)"

# With two ST-Link probes on the bus, st-flash writes to whichever it enumerates first.
# Resolve the probe by serial from the rig, and refuse rather than pick when more than one
# is present: picking means a successful write to the wrong board, then a console read of
# the right one.
. "$FL_ROOT/tools/bench/rig.sh"
rig_find "$FL_ROOT" || true
FL_SNKEY="RIG_STLINK_$(printf '%s' "$FL_BOARD" | tr 'a-z-' 'A-Z_')"
eval "FL_SNVAL=\${$FL_SNKEY:-}"
FL_SN=()
if [ -n "$FL_SNVAL" ]; then
    FL_SN=(--serial "$FL_SNVAL")
elif have st-info; then
    FL_NPROBE=$(st-info --probe 2>/dev/null | sed -n 's/^Found \([0-9][0-9]*\) stlink.*/\1/p')
    if [ "${FL_NPROBE:-1}" -gt 1 ]; then
        die "$FL_NPROBE ST-Link probes are present and $FL_SNKEY is unset in ${RIG_CONF:-<no rig.conf>}: st-flash would take whichever enumerates first (see tools/bench/rig.conf.example)"
    fi
fi
# A RUNNING KickOS image parks the idle thread in WFI and SWD cannot halt a live
# core, so re-flashing a board that is already running needs --connect-under-reset,
# which in turn needs NRST reaching the probe. Default on for the onboard-debugger boards,
# where NRST is wired by construction; a bare 4-pin SWD header carries the pin but not the
# signal, so elsewhere it takes an extra wire and STLINK_UNDER_RESET to match.
case "$FL_BOARD" in
    f411disco|f302nucleo) FL_UR=(--connect-under-reset) ;;
    *)                    FL_UR=() ;;
esac
[ "${STLINK_UNDER_RESET:-}" = "1" ] && FL_UR=(--connect-under-reset)
[ "${STLINK_UNDER_RESET:-}" = "0" ] && FL_UR=()
# NEVER add --reset to a --connect-under-reset write: it leaves the core under halting debug
# with DEMCR.VC_HARDERR armed, so the first HardFault halts the CPU at the handler's first
# instruction and the fault reporter is silent while the board looks locked up. Measured on
# f302nucleo 2026-08-13, DFSR.VCATCH set and DHCSR.S_LOCKUP clear. Releasing NRST already
# starts the image, so --reset buys nothing here.
#
# The bench capture path issues no reset of its own, so this branch's write IS the boot the
# capture reads. Adding a reset here would cut it off mid-line and start a second one.
if [ ${#FL_UR[@]} -gt 0 ]; then
    run st-flash "${FL_SN[@]}" "${FL_UR[@]}" write "$FL_BIN" 0x08000000
else
    run st-flash "${FL_SN[@]}" --reset write "$FL_BIN" 0x08000000
fi
