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
run picotool load -x "$FL_ELF"   # -x: reboot into the app after loading
