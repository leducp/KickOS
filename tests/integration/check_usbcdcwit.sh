#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A capture of usbcdcwit (user/apps/common/usbcdcwit) over the board's own USB CDC console: the
# driver accepted every byte pushed past a full ring, with no error. The kernel banner never
# reaches that console, and the identity rows its driver opens the host's stream with
# (kickos/sys/banner_identity.h) open what is read.
#
#   KOS_CAPTURE=<log> check_usbcdcwit.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

judge_capture usbcdcwit

jno_panic "a panic on usbcdcwit's path"
if has '^\[usbcdcwit\] FAIL'; then
    jfail verdict "usbcdcwit reported FAIL"
fi
_line="$(printf '%s\n' "$OUT" | grep '^\[usbcdcwit\] accepted=' | head -n1)"
if [ -z "$_line" ]; then
    jfail accepted "usbcdcwit printed no accepted count"
fi
_sent="$(printf '%s\n' "$_line" | sed -n 's/^\[usbcdcwit\] accepted=\([0-9]*\) of \([0-9]*\) err=\([-0-9]*\) .*/\1 \2 \3/p')"
set -- $_sent
if [ "$#" -ne 3 ] || [ "$1" -ne "$2" ] || [ "$3" -ne 0 ]; then
    jfail accepted "the driver did not accept every byte: $_line"
fi
if ! has '^\[usbcdcwit\] PASS (sustained output past a full ring)'; then
    jfail verdict "usbcdcwit reached no PASS"
fi
echo "PASS: the USB CDC console took every byte past a full ring"
exit 0
