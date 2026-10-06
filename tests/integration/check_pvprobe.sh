#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of pvprobe (user/apps/xmc4800-relax/pvprobe): the seam installing pattern B in
# the three PV registers, the U,PV control landing a direct unprivileged store while each PV
# register drops one, the seam installing pattern A, the seam's mask and possession refusals, then
# the task killed for its read of the ungranted SCU register. The values are the app's constants.
#
#   KOS_CAPTURE=<log> check_pvprobe.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

judge_capture pvprobe

if has_e '^\[pvprobe\] (ERROR|.*MPU not enforcing)'; then
    jfail error "pvprobe reported a failure"
fi
if has_e '^\[pvprobe\] unpriv [A-Z]*\[PV\]: post=0x[0-9a-f]* LANDED'; then
    jfail pv-landed "a PV register took a direct unprivileged store, so PV write is not enforced"
fi
jafter probe-up 1 '[pvprobe] unprivileged probe up (granted U0C1 window 0x200)' "probe up"
jafter seam-b "$AT" '[pvprobe] baseline through the seam: pattern B' "pattern B's announce"
for _w in 'FDR: rc=0 wrote=0x2aa read=0x2aa exact' 'BRG: rc=0 wrote=0x2aa0000 read=0x2aa0000 exact' \
          'CCR: rc=0 wrote=0x4000 read=0x4000 exact'; do
    jafter seam-b "$AT" "[pvprobe] seam $_w" "pattern B's seam write $_w"
done
jafter sctr-landed "$AT" '[pvprobe] unpriv SCTR[U,PV control]: post=0x7070101 LANDED (post == written)' \
    "the U,PV control landing"
for _w in 'FDR[PV]: post=0x2aa' 'BRG[PV]: post=0x2aa0000' 'CCR[PV]: post=0x4000'; do
    jafter pv-dropped "$AT" "[pvprobe] unpriv $_w DROPPED (post == pre)" "the drop of $_w"
done
jafter seam-a "$AT" '[pvprobe] pattern A through the seam (expect exact)' "pattern A's announce"
for _w in 'FDR: rc=0 wrote=0x155 read=0x155 exact' 'BRG: rc=0 wrote=0x1550000 read=0x1550000 exact' \
          'CCR: rc=0 wrote=0xc001 read=0xc001 exact'; do
    jafter seam-a "$AT" "[pvprobe] seam $_w" "pattern A's seam write $_w"
done
jafter mask "$AT" '[pvprobe] mask refusal: CCR|TBIEN rc=-22 (want -22), pre=0xc001 post=0xc001 unchanged' \
    "the mask refusal leaving CCR unchanged"
jafter refusals "$AT" \
    '[pvprobe] refusals: off-allowlist rc=-22 (want -22), unheld-window rc=-1 (want -1)' \
    "the off-allowlist and unheld-window refusals"
jafter announce "$AT" '[pvprobe] poking UNGRANTED SCU @ 0x50004648 (expect MPU FAULT)' "announce of the read"
require_killed_at pvprobe 0x50004648
echo "PASS: pvprobe showed PV-only writes drop, the seam lands them, and was killed for the SCU read"
