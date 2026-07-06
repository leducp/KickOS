#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Host a SEGGER RTT server for an already-flashed KickOS board so JLinkRTTClient
# (channel 0 = console) can attach. Uses JLinkExe: it connects (halting the core
# to attach), `g` resumes it, and keeping stdin open leaves JLinkExe connected +
# hosting the RTT telnet server on localhost:19021. No GDB needed.
#
# One J-Link probe = one connection at a time, so the loop is SEQUENTIAL:
#   1. tools/flash-jlink.sh <image> [board]   # JLinkExe flashes + quits (frees the probe)
#   2. tools/rtt-server.sh [board]            # this (leave running; Ctrl-C to stop)
#   3. JLinkRTTClient                          # another terminal: channel 0 (console)
#
# The image must be built KICKOS_CONSOLE=rtt|both (else the _SEGGER_RTT block is
# gc'd and there is nothing to serve). For the binary TELEMETRY (channel 1) use
# JLinkRTTLogger -RTTChannel 1 instead (its own connection -- not alongside this).
#
# Usage: tools/rtt-server.sh [board]   (board: k64f (default) | xmc4800)
set -eu

board=${1:-k64f}
case "$board" in
    k64f|frdmk64f)         dev=MK64FN1M0xxx12 ;;
    xmc4800|xmc4800-relax) dev=XMC4800-2048 ;;
    *) echo "rtt-server: unknown board '$board' (known: k64f, xmc4800)" >&2; exit 1 ;;
esac

echo "rtt-server: $board [$dev] -- RTT on localhost:19021 (Ctrl-C to stop)"
echo "rtt-server: then attach the console in another terminal:  JLinkRTTClient"

# `g` resumes the core after the attach-time halt; `cat` holds stdin open so
# JLinkExe stays connected and keeps hosting the RTT server (further J-Link
# Commander commands can be typed here too).
{ printf 'g\n'; cat; } | JLinkExe -device "$dev" -if SWD -speed 4000 -autoconnect 1
