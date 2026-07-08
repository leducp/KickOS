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
run bossac -p "${p#/dev/}" -e -w -v -R "$FL_BIN"
