#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A capture of usbcdcwit (user/apps/common/usbcdcwit) over the board's own USB CDC console: the
# driver accepted every byte pushed past a full ring, with no error. Nothing listens on that
# transport before the image boots, so the kernel banner never reaches it; the app's own commit
# line opens the boot that is read.
#
#   KOS_CAPTURE=<log> check_usbcdcwit.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

if ! judging_capture; then
    jfail no-capture "no KOS_CAPTURE: usbcdcwit is judged from a capture only"
fi
[ -s "$KOS_CAPTURE" ] || jfail no-capture "no capture at $KOS_CAPTURE"
_at="$(tr -d '\r' < "$KOS_CAPTURE" | awk '/^\[usbcdcwit\] commit / { at = NR } END { print at }')"
[ -n "$_at" ] || jfail commit "the capture carries no usbcdcwit commit line, so no boot in it can be read"
OUT="$(tr -d '\r' < "$KOS_CAPTURE" | sed -n "${_at},\$p")"

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
