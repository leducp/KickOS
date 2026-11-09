#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Judges a pizero2350-amp2 capture of the ampping partition and its ACCESSCTRL witness
# (docs/design-m10-fleet.md, section 9.5): node 0's readback of UART0's register, node 0's read
# of its own UART0, and the four rounds across.
#
# A build configured with KICKOS_WITNESS_NODE (both kernels saying `witness node`) is the node
# witness's, and its capture owes two more things after the rounds: node 1 raised UART0's line on
# its own core and its own kernel took it as one spurious line, then node 1's kernel read UART0
# and ended in the bus fault ACCESSCTRL gives core 1, at that address, which the MPU did not
# refuse. That capture is taken as the plain one, the knob added:
#
#   EXTRA_CMAKE="-DKICKOS_WITNESS_NODE=ON -DKICKOS_WITNESS_NODE_SCRATCH=check_pizero_amp_gate.sh" \
#       AMP_PARTITION=1 APP=ampping_n0 VARIANT=amp2-n0 tools/bench/bench.sh pizero2350
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
WITNESS=0
if grep -q '^KICKOS_WITNESS_NODE:BOOL=ON$' "$1/CMakeCache.txt" 2>/dev/null; then
  WITNESS=1
fi

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

# The line number of the one line matching <ere>, empty where there is not exactly one.
line_of() { # <ere>
  printf '%s\n' "$OUT" | grep -nE "$1" | awk -F: 'END { if (NR == 1) print $1 }'
}

_wit="$(printf '%s\n' "$OUT" | grep -c '^   witness node$')"
_panics="$(printf '%s\n' "$OUT" | grep -cE "$KOS_PANIC_RE")"
if [ "$WITNESS" -eq 0 ]; then
  if [ "$_wit" -ne 0 ]; then
    bad "witness: $_wit 'witness node' banner line(s) from a build without KICKOS_WITNESS_NODE"
  fi
  if [ "$_panics" -ne 0 ]; then
    bad "panic: a panic or fault banner in the judged boot"
  fi
else
  if [ "$_wit" -ne 2 ]; then
    bad "witness: $_wit 'witness node' banner line(s), where both kernels print one"
  fi
  one '^ampping: witness: node 1 raised line 33 on its own core, its kernel counted 1 spurious$' \
      stray "node 1's stray line, taken once by its own kernel" || true
  if printf '%s\n' "$OUT" | grep -q '^ampping: witness: ERROR'; then
    bad "refused: node 1's kernel read node 0's UART0 and ran on"
  fi
  _read="$(line_of '^\[node\] node 1 on core 1: ring 0 reads 0x40070fe0$')"
  _fault="$(line_of '^=== BUS FAULT ===$')"
  if [ -z "$_read" ]; then
    bad "read: no one announcement of node 1's kernel reading UART0's UARTPERIPHID0"
  elif [ -z "$_fault" ] || [ "$_panics" -ne 1 ] || [ "$_fault" -lt "$_read" ]; then
    bad "refused: $_panics panic or fault banner(s), where node 1's read owes one bus fault after it"
  else
    _after="$(printf '%s\n' "$OUT" | sed -n "${_fault},\$p")"
    _bfar="$(printf '%s\n' "$_after" | grep -c '^  BFAR=0x40070fe0$')"
    _cfsr="$(printf '%s\n' "$_after" | sed -n 's/^  CFSR=0x\([0-9a-f]*\) HFSR=0x[0-9a-f]*$/\1/p' | head -n 1)"
    if [ "$_bfar" -ne 1 ]; then
      bad "bfar: the bus fault does not name UART0's UARTPERIPHID0, 0x40070fe0"
    fi
    # PRECISERR and BFARVALID, and no MemManage bit: the bus refused the read, not the MPU.
    if [ -z "$_cfsr" ] || [ "${#_cfsr}" -gt 8 ] || [ $((0x$_cfsr & 0x8200)) -ne $((0x8200)) ] \
       || [ $((0x$_cfsr & 0xff)) -ne 0 ]; then
      bad "cfsr: the bus fault's CFSR '${_cfsr}' is not a precise bus error alone"
    fi
  fi
fi

if [ "$rc" -ne 0 ]; then
  exit 1
fi
if [ "$WITNESS" -eq 1 ]; then
  echo "PASS: ACCESSCTRL reads back UART0 as node 0's core's alone; node 0 read 0x11; four rounds across; node 1's stray line stayed its own, and ACCESSCTRL refused node 1's kernel UART0"
  exit 0
fi
echo "PASS: ACCESSCTRL reads back UART0 as node 0's core's alone; node 0 read 0x11; four rounds across"
