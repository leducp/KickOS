#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# flash-<tool> backend: flash an Espressif board (esp32 / esp32c6) via esptool.
# Usage: tools/flash-esptool.sh <board> [app]   (app default: hello)
set -euo pipefail
FL_ROOT=$(cd "$(dirname "$0")/.." && pwd); . "$FL_ROOT/tools/flash-common.sh"
flash_resolve "$@"
esptool=esptool; have esptool || esptool=esptool.py
have "$esptool" || die "esptool not on PATH -- activate the esp-idf env"
[ -e "$FL_APPBIN" ] || die "no $FL_APPBIN (the esptool image) -- rebuild with the esp-idf env active"
off=0x0; [ "$FL_CHIP" = esp32 ] && off=0x1000     # C-series ROM-boots @0; esp32 @0x1000
args=(--chip "$FL_CHIP")
if p=$(pick_port); then
    args+=(--port "$p")
elif [ -n "${FLASH_PORT:-}" ]; then
    die "FLASH_PORT='$FLASH_PORT' does not exist -- refusing to guess another board's port"
else
    say "no serial device -- letting esptool auto-detect"
fi
run "$esptool" "${args[@]}" write_flash "$off" "$FL_APPBIN"
