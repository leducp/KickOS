#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# flash-<tool> backend: flash an STM32 via st-flash (ST-Link). Flash alias 0x08000000.
# Usage: tools/flash-stlink.sh <board> [app]
set -euo pipefail
FL_ROOT=$(cd "$(dirname "$0")/.." && pwd); . "$FL_ROOT/tools/flash-common.sh"
flash_resolve "$@"
have st-flash || die "st-flash not on PATH (install stlink-tools)"
run st-flash --reset write "$FL_BIN" 0x08000000
