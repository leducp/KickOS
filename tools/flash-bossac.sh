#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# flash-<tool> backend: flash a SAM3X (Arduino Due) via bossac over the serial port.
# Usage: tools/flash-bossac.sh <board> [app]   (double-tap RESET for the SAM-BA ROM)
set -euo pipefail
FL_ROOT=$(cd "$(dirname "$0")/.." && pwd); . "$FL_ROOT/tools/flash-common.sh"
flash_resolve "$@"
have bossac || die "bossac not on PATH"
p=$(pick_port) || die "no serial port -- set FLASH_PORT=/dev/ttyACMx (double-tap RESET for SAM-BA)"
# -b sets GPNVM1 (boot from flash) -- WITHOUT it the ROM keeps booting the SAM-BA
# monitor, not the app (silent "dead" board). NOTE: the SAM3X latches its boot mode
# at NRST/power-on, NOT on a soft reset, so bossac's -R may not be enough -- press
# the board's RESET button (or power-cycle) after flashing if the app doesn't start.
run bossac -p "${p#/dev/}" -e -w -v -b -R "$FL_BIN"
