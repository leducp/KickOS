#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of rxdrv (user/apps/rx72m/rxdrv): P80 muxed and the console pin's mux
# refused, kos_periph_enable reaching the chip for the window's holder and refused to a thread
# holding none, LED6's pad tracking every drive, then the windowless thread killed for its write
# to the ungranted MPC.
#
#   KOS_CAPTURE=<log> check_rxdrv.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

judge_capture rxdrv

if has_e '^\[rxdrv\] (ERROR|FAIL|.*DID NOT FAULT)'; then
    jfail error "rxdrv reported a failure"
fi
_at="$(printf '%s\n' "$OUT" | awk '$0 == "[rxdrv] pinmux P80 -> general I/O rc 0" { print NR; exit }')"
[ -n "$_at" ] || jfail mux "no successful mux of P80"
jafter console-pin "$_at" '[rxdrv] pinmux PB1/TXD6 refused (-KOS_EBUSY): console pin is kernel-owned' "console pin refusal"
jafter holder "$AT" '[rxdrv] PASS periph_enable holder rc -22 (want -22)' "holder's periph_enable"
jafter blink-start "$AT" '[rxdrv] blinking LED6 (P80) via the port window' "blink start"
jafter blink "$AT" '[rxdrv] PASS (pad tracked the drive on every cycle)' "blink verdict"
jafter non-holder "$AT" '[rxdrv] PASS periph_enable non-holder rc -1 (want -1)' "non-holder's refusal"
jafter announce "$AT" '[rxdrv] poking UNGRANTED MPC PB1PFS @ 0x0008C199 (expect MPU FAULT)' "announce of the write"
require_killed_at rxpoke 0x0008C199
echo "PASS: rxdrv drove LED6 through its window and its windowless thread was killed for the MPC write"
