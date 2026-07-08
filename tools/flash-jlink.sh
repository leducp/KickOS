#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# flash-<tool> backend: flash via a SEGGER J-Link (incl. an OpenSDA reflashed with
# J-Link-OB firmware). Needs the J-Link Software Pack on PATH (JLinkExe).
# Usage: tools/flash-jlink.sh <board> [app]   (app default: hello)
#
# Loads the .hex (its addresses are embedded -> `loadfile`, no per-board base). The
# J-Link -device string MUST match the exact silicon: the k64f/xmc rows are
# HW-verified; the others are the expected part for each KickOS board -- adjust if
# your specific chip variant differs. Add a board: one row in the case below.
set -euo pipefail
FL_ROOT=$(cd "$(dirname "$0")/.." && pwd); . "$FL_ROOT/tools/flash-common.sh"
flash_resolve "$@"
have JLinkExe || die "JLinkExe not on PATH (install SEGGER's J-Link Software Pack)"

case "$FL_BOARD" in
    frdmk64f)             dev=MK64FN1M0xxx12 ;;  # Kinetis (verified)
    xmc4800-relax)        dev=XMC4800-2048   ;;  # XMC (verified)
    bluepill|bluepill-c8) dev=STM32F103C8    ;;
    blackpill)            dev=STM32F411CE    ;;
    f411disco)            dev=STM32F411VE    ;;
    f302nucleo)           dev=STM32F302R8    ;;
    microbit)             dev=nRF51822_xxAA  ;;
    rx72m)                dev=R5F572MNDxBD   ;;
    *) die "flash-jlink: no J-Link -device string for board '$FL_BOARD' (add a row)" ;;
esac

script=$(mktemp); trap 'rm -f "$script"' EXIT
printf 'loadfile %s\nr\ng\nq\n' "$FL_HEX" > "$script"   # r=reset g=go q=quit
say "$FL_BOARD [$dev] <- $FL_HEX (loadfile; addresses embedded)"
run JLinkExe -device "$dev" -if SWD -speed 4000 -autoconnect 1 -CommanderScript "$script"
