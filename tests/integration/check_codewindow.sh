#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# codewindow_below and codewindow_past (user/apps/common/codewindow): the RV32 PMP code window
# is the TOR pair the script bounds. The window's first and last words read; the fetch of the
# word below it, the kernel's text, or of the word past it kills the thread at the address
# fetched, and nothing panicked. Boots the image, or reads a silicon capture with KOS_CAPTURE.
#
#   check_codewindow.sh <codewindow image> below|past
#   KOS_CAPTURE=<log> check_codewindow.sh <board-build> <kickos-source> <cmake> below|past

set -u
. "$(dirname "$0")/../lib/gate.sh"

_usage="usage: check_codewindow.sh <codewindow image> below|past"
if judging_capture; then
    side="${4:?$_usage}"
    judge_capture "codewindow_$side"
else
    elf="${1:?$_usage}"
    side="${2:?$_usage}"
    run_image "$elf"
fi

if has '^\[codewindow\] ERROR'; then
    cfail error "the fetch outside the window was permitted"
fi
if ! has '^\[codewindow\] first and last words read'; then
    cfail words "the window's own words were not readable"
fi
lo="$(printf '%s\n' "$OUT" | sed -n 's/^\[codewindow\] window 0x\([0-9a-fA-F]*\)\.\..*/\1/p' | head -n1)"
hi="$(printf '%s\n' "$OUT" | sed -n 's/^\[codewindow\] window 0x[0-9a-fA-F]*\.\.0x\([0-9a-fA-F]*\)$/\1/p' | head -n1)"
at="$(printf '%s\n' "$OUT" | sed -n 's/^\[codewindow\] fetching 0x\([0-9a-fA-F]*\)$/\1/p' | head -n1)"
if [ -z "$lo" ] || [ -z "$hi" ] || [ -z "$at" ]; then
    cfail announce "the app announced no window or no fetch"
fi
case "$side" in
    below) want="$((0x$lo - 4))" ;;
    past) want="$((0x$hi))" ;;
    *) fail "$_usage" ;;
esac
if [ "$((0x$at))" -ne "$want" ]; then
    cfail fetch "the app fetched 0x$at, not the word $side the window 0x$lo..0x$hi"
fi
require_killed_at main "0x$at"
echo "PASS: the fetch $side the window 0x$lo..0x$hi faulted at 0x$at"
exit 0
