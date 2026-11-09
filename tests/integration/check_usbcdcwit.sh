#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A capture of usbcdcwit (user/apps/common/usbcdcwit) over the board's own USB CDC console: the
# driver accepted every byte pushed past a full ring, with no error, and the host received each
# of them in order. The kernel banner never reaches that console, and the identity rows its
# driver opens the host's stream with (kickos/sys/banner_identity.h) open what is read.
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
# Byte i of the letter stream is 'a' + i % 26, so a byte lost or repeated anywhere between the
# driver's ring and this log moves every letter after it.
_bad="$(printf '%s\n' "$OUT" | awk -v n="$1" '
    /^\[usbcdcwit\] start/ { on = 1; s = ""; next }
    on && /^\[usbcdcwit\] accepted=/ { done = 1; exit }
    on { s = s $0 }
    END {
        if (!done) { print "no letter stream between the start and accepted lines"; exit }
        a = "abcdefghijklmnopqrstuvwxyz"
        for (i = 0; i < length(s); i++) {
            want = substr(a, i % 26 + 1, 1)
            if (substr(s, i + 1, 1) != want) {
                print "stream byte " i " is \"" substr(s, i + 1, 1) "\" where \"" want "\" was sent"
                exit
            }
        }
        if (length(s) != n) { print length(s) " stream bytes arrived of the " n " accepted" }
    }')"
if [ -n "$_bad" ]; then
    jfail stream "the host did not receive the stream the driver accepted: $_bad"
fi
# The bench's reader writes nothing to the device, so a byte the device received is the host's
# tty echoing what arrived before its reader set it raw, or another program on the port.
_rx="$(printf '%s\n' "$OUT" | sed -n 's/^\[usbcdcwit\] tx=.* rx=\([0-9]*\)$/\1/p' | head -n1)"
if [ -z "$_rx" ]; then
    jfail rx "usbcdcwit printed no rx count"
elif [ "$_rx" -ne 0 ]; then
    jfail rx "the device received $_rx byte(s) from the host: $(printf '%s\n' "$OUT" | grep '^\[usbcdcwit\] rx bytes:')"
fi
if ! has '^\[usbcdcwit\] PASS (sustained output past a full ring)'; then
    jfail verdict "usbcdcwit reached no PASS"
fi
echo "PASS: the USB CDC console took every byte past a full ring"
exit 0
