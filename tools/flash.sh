#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Magic flasher: the thin front-end over the flash-<tool>.sh backends. It resolves
# the board -> chip (from board.cmake), then dispatches to the right backend script
# -- the first one whose tool is on PATH, or one you force with FLASH_TOOL. Every
# backend is also directly runnable on its own (tools/flash-jlink.sh <board>, ...).
#
# The ONLY assumption is that the flasher is on PATH (no hardcoded install dirs).
# A chip may have several valid backends (e.g. an STM32 -> stlink OR jlink); the
# first present wins, unless FLASH_TOOL picks one -- e.g. use your own J-Link on a
# Blue Pill instead of an ST-Link:  FLASH_TOOL=jlink tools/flash.sh bluepill
#
# Usage:
#   tools/flash.sh <board> [app]     # app defaults to hello
#   tools/flash.sh --list            # every board + the backend it would use
# Env knobs (also honored by the backends):
#   FLASH_TOOL=esptool|stlink|jlink|picotool|pyocd|bossac|rfp    force a backend
#   FLASH_PORT=/dev/ttyACM0     force the serial port     DRY_RUN=1  print, don't run
set -euo pipefail
FL_ROOT=$(cd "$(dirname "$0")/.." && pwd); . "$FL_ROOT/tools/flash-common.sh"

# candidates_for <chip> -> ordered backend keys valid for that chip (empty = QEMU/host)
candidates_for() {
    case "$1" in
        esp32*)                        echo "esptool" ;;
        stm32f103|stm32f302|stm32f411) echo "stlink jlink" ;;
        rp2040)                        echo "picotool" ;;
        nrf51)                         echo "pyocd jlink" ;;
        sam3x8e)                       echo "bossac" ;;
        mk64f)                         echo "jlink pyocd" ;;
        xmc4800)                       echo "jlink" ;;
        rx72m)                         echo "rfp" ;;
        *)                             echo "" ;;
    esac
}

# tool_bin <backend-key> -> the PATH binary backing it, or empty if unavailable
tool_bin() {
    case "$1" in
        esptool)  if have esptool; then echo esptool; elif have esptool.py; then echo esptool.py; fi ;;
        stlink)   have st-flash  && echo st-flash  || true ;;
        jlink)    have JLinkExe  && echo JLinkExe  || true ;;
        picotool) have picotool  && echo picotool  || true ;;
        pyocd)    have pyocd     && echo pyocd     || true ;;
        bossac)   have bossac    && echo bossac    || true ;;
        rfp)      have rfp-cli    && echo rfp-cli   || true ;;
    esac
}

# --- --list: show the plan for every board -------------------------------------
if [ "${1:-}" = "--list" ]; then
    printf '%-16s %-10s %-9s %s\n' BOARD CHIP ARCH 'BACKEND (priority; first on PATH)'
    for d in "$FL_ROOT"/boards/*/; do
        b=$(basename "$d"); c=$(_bf "$b" KICKOS_CHIP); cand=$(candidates_for "$c")
        if [ -n "$cand" ]; then disp="${cand// / | }"     # join real backend keys
        else case "$c" in mps2|virt) disp="(QEMU -- not flashed)" ;; *) disp="(sim/host -- not flashed)" ;; esac; fi
        printf '%-16s %-10s %-9s %s\n' "$b" "$c" "$(_bf "$b" KICKOS_ARCH)" "$disp"
    done
    exit 0
fi

flash_resolve "$@"                    # sets FL_BOARD/FL_APP/FL_CHIP/... + guards QEMU/sim
cands=$(candidates_for "$FL_CHIP")
[ -n "$cands" ] || die "no flash recipe for chip '$FL_CHIP' (board '$FL_BOARD')"

# --- pick a backend ------------------------------------------------------------
pick=""
if [ -n "${FLASH_TOOL:-}" ]; then
    case " $cands " in
        *" $FLASH_TOOL "*) ;;
        *) die "FLASH_TOOL='$FLASH_TOOL' can't flash chip '$FL_CHIP' (candidates: $cands)" ;;
    esac
    [ -n "$(tool_bin "$FLASH_TOOL")" ] || die "FLASH_TOOL='$FLASH_TOOL' selected but its tool is not on PATH"
    pick=$FLASH_TOOL
else
    for m in $cands; do
        [ -n "$(tool_bin "$m")" ] && { pick=$m; break; }
    done
    [ -n "$pick" ] || die "no flasher on PATH for chip '$FL_CHIP' -- need one of: $cands
       (install it/add to PATH, or force with FLASH_TOOL=<name>)"
fi

say "$FL_BOARD (chip $FL_CHIP) <- $FL_APP  [backend: $pick]"
exec "$FL_ROOT/tools/flash-$pick.sh" "$FL_BOARD" "$FL_APP"   # backends inherit DRY_RUN/FLASH_PORT
