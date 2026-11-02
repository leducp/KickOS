#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of a bus slot witness (<kickos/apps/spi_slots.h>): two clients of one packaged
# SPI service, each handed the endpoint badged with its own slot. The holder opens and transfers on
# slot 0, the intruder opens its slot 1 and is refused slot 0 on a CONFIG and on a transfer, and no
# panic. The clients run in their own order, so only the intruder's verdict is read as the last.
#
#   KOS_CAPTURE=<log> check_spi_slots.sh <board-build> <kickos-source> <cmake> <tag>

set -u
. "$(dirname "$0")/../lib/gate.sh"

TAG="${4:-}"
case "$TAG" in
    xmcslots|k64slots)
        ;;
    *)
        fail "usage: KOS_CAPTURE=<log> check_spi_slots.sh <build> <source> <cmake> <xmcslots|k64slots>"
        ;;
esac
judge_capture "$TAG"

if has_e "^\[$TAG\] .*FAIL"; then
    jfail error "the witness reported a failure"
fi
jno_panic "a panic in the slot witness"
for _case in 'holder opens its slot 0' 'holder transfers on its slot 0' 'intruder opens its slot 1'; do
    jafter case 1 "[$TAG] $_case: PASS (rc=" "case $_case" part
done
jafter refused 1 "[$TAG] intruder is refused the holder's slot 0: PASS (rc=-13)" \
    "the intruder's CONFIG of slot 0 refused -KOS_EACCES"
jafter refused-xfer "$AT" "[$TAG] intruder is refused a transfer on slot 0: PASS (rc=-13)" \
    "the intruder's transfer on slot 0 refused -KOS_EACCES"
jafter verdict "$AT" "[$TAG] PASS" "the intruder's verdict"
echo "PASS: $TAG refused the second client the first one's slot"
