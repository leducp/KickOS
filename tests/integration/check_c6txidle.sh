#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of c6txidle (user/apps/esp32c6-wroom/c6txidle): the TAIL line whole up to its
# sentinel's last byte though the TX pad was taken from the UART the moment the console flush
# returned, the transmitter idle at the encoding the flush waits for, the wait neither shorter than
# the last frame nor run to its spin bound, and no panic.
#
#   KOS_CAPTURE=<log> check_c6txidle.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

judge_capture c6txidle

if has '^\[c6txidle\] ERROR'; then
    jfail error "c6txidle could not arrange the witness"
fi
if has '^\[c6txidle\] FAIL'; then
    jfail verdict "$(printf '%s\n' "$OUT" | grep -m1 '^\[c6txidle\] FAIL')"
fi
jno_panic "a panic in c6txidle"
jafter tail 1 '[c6txidle] TAIL 0123456789abcdef0123456789abcdef <<<TXIDLE-END>>>' \
    "TAIL line ending in its whole sentinel"
jafter timing "$AT" '[c6txidle] frame ' "flush timing record" part
jafter state "$AT" '[c6txidle] ST_UTX_OUT ' "transmitter state record" part
jafter verdict "$AT" '[c6txidle] PASS the flush returned once the last frame had left' "verdict"
echo "PASS: c6txidle's tail survived a cut at the flush's return, so the flush waited for it"
