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
# A RUNNING KickOS image parks the idle thread in WFI and SWD cannot halt a live
# core, so re-flashing a board that is already running needs --connect-under-reset,
# which in turn needs NRST reaching the probe. Default on for the onboard-debugger
# boards, where it is wired by construction. Off elsewhere: a bare 4-pin SWD header does
# not carry NRST even though the board has the pin, so it needs an extra wire.
# STLINK_UNDER_RESET=1 forces it on (use once NRST is wired), =0 forces it off.
case "$FL_BOARD" in
    f411disco|f302nucleo) FL_UR=(--connect-under-reset) ;;
    *)                    FL_UR=() ;;
esac
[ "${STLINK_UNDER_RESET:-}" = "1" ] && FL_UR=(--connect-under-reset)
[ "${STLINK_UNDER_RESET:-}" = "0" ] && FL_UR=()
run st-flash "${FL_UR[@]}" --reset write "$FL_BIN" 0x08000000
