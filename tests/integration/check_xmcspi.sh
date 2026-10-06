#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of xmcspi (user/apps/xmc4800-relax/xmcspi): the three PV registers written
# through the seam, four words echoed on the line, the loopback verdict, then the task killed for
# its read of the ungranted SCU register.
#
#   KOS_CAPTURE=<log> check_xmcspi.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

judge_capture xmcspi

if has_e '^\[xmcspi\] (ERROR|.*DISCARDED/REFUSED|.*FAIL|.*DID NOT FAULT)'; then
    jfail error "xmcspi reported a failure"
fi
for _r in FDR BRG CCR; do
    jafter seam 1 "[xmcspi] seam $_r:" "seam write of $_r" part
done
jafter loopback-start "$AT" '[xmcspi] starting SSC loopback (blocking on the USIC0 line)' "loopback start"
_from="$AT"
for _w in '0: tx=0xa5 rx=0xa5 PASS' '1: tx=0x3c rx=0x3c PASS' '2: tx=0x0 rx=0x0 PASS' \
          '3: tx=0xff rx=0xff PASS'; do
    jafter word "$_from" "[xmcspi] word $_w" "echo of word $_w"
    _from="$AT"
done
jafter loopback "$_from" '[xmcspi] loopback PASS (all words echoed equal)' "loopback verdict"
jafter announce "$AT" '[xmcspi] poking UNGRANTED SCU @ 0x50004648 (expect MPU FAULT)' "announce of the read"
require_killed_at xmcspi 0x50004648
echo "PASS: xmcspi echoed four words on its line and was killed for the ungranted SCU read"
