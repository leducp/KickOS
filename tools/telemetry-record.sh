#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Record the KickOS binary telemetry (RTT channel 1) to a file. Needs a
# KICKOS_TELEMETRY=rtt image flashed + running, and JLinkRTTLogger (SEGGER J-Link
# pack) on PATH. JLinkRTTLogger opens its OWN J-Link connection -- do NOT run
# rtt-server.sh at the same time (one probe). Ctrl-C stops the capture.
#
# Usage: tools/telemetry-record.sh <out.bin> [board]   (board: k64f (default) | xmc4800)
# Afterwards, decode with the printed kicktrace command (ns via the board's clock).
# For a LIVE scroll instead of a file, skip this and pipe straight into --follow:
#   mkfifo /tmp/rtt1; JLinkRTTLogger ... -RTTChannel 1 /tmp/rtt1 &
#   python3 tools/kicktrace.py --follow /tmp/rtt1
set -eu

out=${1:?usage: telemetry-record.sh <out.bin> [k64f|xmc4800]}
board=${2:-k64f}
# dev = J-Link device; hz = trace-clock (DWT/core = SystemCoreClock) for tick->ns.
case "$board" in
    k64f|frdmk64f)         dev=MK64FN1M0xxx12; hz=120000000 ;;  # PLL 120 MHz (FEI 20.97 MHz if the PLL bring-up is skipped)
    xmc4800|xmc4800-relax) dev=XMC4800-2048;   hz=120000000 ;;  # PLL 120 MHz
    *) echo "telemetry-record: unknown board '$board' (known: k64f, xmc4800)" >&2; exit 1 ;;
esac
command -v JLinkRTTLogger >/dev/null 2>&1 || {
    echo "telemetry-record: JLinkRTTLogger not on PATH (install the SEGGER J-Link pack)" >&2; exit 1; }

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
echo "telemetry-record: $board [$dev] channel 1 -> $out   (Ctrl-C to stop)"
echo "telemetry-record: then decode (ns via the ${hz} Hz trace clock):"
echo "    python3 $here/kicktrace.py $out --summary --clock-hz $hz"
echo "    python3 $here/kicktrace.py $out --csv     --clock-hz $hz"
echo

# exec so JLinkRTTLogger owns the terminal; Ctrl-C ends it and leaves <out> complete.
exec JLinkRTTLogger -device "$dev" -if SWD -speed 4000 -RTTChannel 1 "$out"
