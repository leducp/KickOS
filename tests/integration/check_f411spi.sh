#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of f411spi (user/apps/f411disco/f411spi): the four words echoed through the
# jumper on the SPI1 line, the loopback verdict, then the task killed for its read of GPIOB.
#
#   KOS_CAPTURE=<log> check_f411spi.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

judge_capture f411spi

if has_e '^\[f411spi\] (ERROR|TXE timeout|.*FAIL|.*DID NOT FAULT)'; then
    jfail error "f411spi reported a failure"
fi
jafter loopback-start 1 '[f411spi] starting loopback (blocking on the SPI1 line)' "loopback start"
_from="$AT"
for _w in '0: tx=0xa5 rx=0xa5 PASS' '1: tx=0x3c rx=0x3c PASS' '2: tx=0x0 rx=0x0 PASS' \
          '3: tx=0xff rx=0xff PASS'; do
    jafter word "$_from" "[f411spi] word $_w" "echo of word $_w"
    _from="$AT"
done
jafter loopback "$_from" '[f411spi] loopback PASS (all words echoed equal)' "loopback verdict"
jafter announce "$AT" '[f411spi] poking UNGRANTED GPIOB @ 0x40020400 (expect MPU FAULT)' "announce of the read"
require_killed_at f411spi 0x40020400
echo "PASS: f411spi echoed four words on its line and was killed for the ungranted read"
