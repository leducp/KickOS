#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Judges a pizero2350-amp2 capture of the ampping partition and its ACCESSCTRL witness
# (docs/design-m10-fleet.md, section 9.5): node 0's readback of UART0's register, node 0's read
# of its own UART0, and the four rounds across.
#
#   KOS_CAPTURE=<log> check_pizero_amp_gate.sh <board-build> <kickos-source> <cmake>
#
# A finding is `FAIL: <token>: <prose>`, the token a planted row of
# tests/static/check_app_judges.sh names.
#
# Only a capture is judged: no emulator runs this partition. Both kernels print a banner per
# boot, so the boot judged opens at the last banner ahead of the last readback line, which
# node 0 prints before it launches node 1.

set -u
. "$(dirname "$0")/../lib/gate.sh"

USAGE="usage: KOS_CAPTURE=<log> check_pizero_amp_gate.sh <board-build> <kickos-source> <cmake>"
: "${1:?$USAGE}" "${2:?$USAGE}" "${3:?$USAGE}"
if [ -z "${KOS_CAPTURE:-}" ]; then
  jfail no-capture "no KOS_CAPTURE: this partition is judged from a silicon capture only"
fi
[ -s "$KOS_CAPTURE" ] || jfail no-capture "no capture at $KOS_CAPTURE"

BANNER_RE='^   KickOS [^ ]+  -  microkernel RTOS$|^K [0-9]'
READBACK_RE='^# accessctrl: 0xa0 = 0x[0-9a-f]+$'

_at="$(tr -d '\r' < "$KOS_CAPTURE" | awk -v banner="$BANNER_RE" -v readback="$READBACK_RE" '
  $0 ~ banner { b = NR }
  $0 ~ readback { at = b }
  END { print at }')"
if [ -z "$_at" ]; then
  jfail readback "$KOS_CAPTURE holds no '# accessctrl: 0xa0' line after a banner"
fi
OUT="$(tr -d '\r' < "$KOS_CAPTURE" | sed -n "${_at},\$p")"
echo "== judging the boot at line $_at of $KOS_CAPTURE =="

rc=0

# The one line matching <ere>, or a finding.
one() { # <ere> <token> <what>
  _n="$(printf '%s\n' "$OUT" | grep -cE "$1")"
  if [ "$_n" -ne 1 ]; then
    bad "$2: $3: $_n line(s) match '$1', not one"
    return 1
  fi
  return 0
}

_banners="$(printf '%s\n' "$OUT" | grep -cE "$BANNER_RE")"
if [ "$_banners" -gt 2 ]; then
  bad "banners: $_banners banners in the judged boot, which two kernels print two of"
fi

# Node 0 granted UART0, which runs on core 0: DBG, CORE0, SP and SU set; CORE1, NSP and NSU
# clear; DMA whatever reset left (RP2350 datasheet, Table 952).
if one "$READBACK_RE" readback "UART0's ACCESSCTRL readback"; then
  _img="$(printf '%s\n' "$OUT" | sed -n 's/^# accessctrl: 0xa0 = 0x\([0-9a-f]*\)$/\1/p')"
  if [ "${#_img}" -gt 8 ]; then
    bad "readback: UART0's ACCESSCTRL readback 0x$_img is wider than a register"
  elif [ $((0x$_img & ~0x40)) -ne $((0x9C)) ]; then
    bad "readback: UART0's ACCESSCTRL reads 0x$_img: want DBG, CORE0, SP and SU alone beside DMA (0x9c or 0xdc)"
  fi
fi

one "^ampping: gate: node 0 read 0x11 from its uart0$" gate "node 0's read of its uart0"

_faults="$(printf '%s\n' "$OUT" | grep -c '=== THREAD FAULT ===')"
if [ "$_faults" -ne 0 ]; then
  bad "fault: $_faults thread fault(s) in the judged boot, where none is expected"
fi

one '^ampping: 2 of 2 node app\(s\) alive on the port the partition names, own row 1$' \
    alive "both node apps alive"
one '^ampping: node 0 calls node 1 port 3$' call "node 0's call line"
for _n in 1 2 3 4; do
  one "^  ping $_n -> pong $((_n + 1)) from node 1 \\(4 byte\\(s\\)\\)\$" round "round $_n"
done
if one '^ampping: node 1 answered 4 round\(s\), its own record says [0-9]+$' served "node 1's record"; then
  _served="$(printf '%s\n' "$OUT" | sed -n 's/^ampping: node 1 answered 4 round(s), its own record says \([0-9]*\)$/\1/p')"
  if [ "$_served" -lt 4 ]; then
    bad "served: node 1's own record says $_served, not four"
  fi
fi
one '^ampping: node 0 done, 4 round\(s\) across the partition$' done "node 0 done"

if has_e "$KOS_PANIC_RE"; then
  bad "panic: a panic or fault banner in the judged boot"
fi

if [ "$rc" -ne 0 ]; then
  exit 1
fi
echo "PASS: ACCESSCTRL reads back UART0 as node 0's core's alone; node 0 read 0x11; four rounds across"
