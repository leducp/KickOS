#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# flash-<tool> backend: flash a PJRC Teensy over its HalfKay bootloader with
# teensy_loader_cli. Usage: tools/flash-teensy.sh <board> [app]   (app default: hello)
#
# Loads the .hex (HalfKay takes Intel HEX; its addresses are embedded, so no load
# base). The Teensy 4.1 exposes no SWD header, so HalfKay is the flash path.
#
# The board must be IN HalfKay: tap the on-board button. teensy_loader_cli's own
# -s/-r auto-entry cannot do it for us -- a soft reboot needs the Teensyduino USB
# serial stack (KickOS's console is LPUART6, not USB) and a hard reboot needs the
# rebootor hardware on the RESET line. So pass -w and wait for the button instead.
set -euo pipefail
FL_ROOT=$(cd "$(dirname "$0")/.." && pwd); . "$FL_ROOT/tools/flash-common.sh"
flash_resolve "$@"
have teensy_loader_cli || die "teensy_loader_cli not on PATH (PJRC loader_cli)"
[ -e "$FL_HEX" ] || die "no $FL_HEX (build it: cmake --build build/$FL_BOARD --target $FL_APP)"

# --mcu selector: `teensy_loader_cli --list-mcus` names them. TEENSY41 is the board
# selector (the chip-level imxrt1062 does not distinguish the 4.0/4.1 flash size);
# an older loader_cli that only lists imxrt1062 wants that instead.
case "$FL_BOARD" in
    teensy41) mcu=TEENSY41 ;;
    *) die "flash-teensy: no --mcu selector for board '$FL_BOARD' (add a row)" ;;
esac

say "$FL_BOARD [$mcu] <- $FL_HEX (HalfKay; tap the on-board button to enter it)"
run teensy_loader_cli --mcu="$mcu" -w -v "$FL_HEX"
