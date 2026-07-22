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
# -t elf: the KickOS image has no .elf extension, so force the type -- picotool
# refuses to guess format from an extensionless name. -x: reboot into the app.
run picotool load -x -t elf "$FL_ELF"
