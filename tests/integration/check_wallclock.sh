#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of wallclock (user/apps/common/wallclock): the kernel's requested sleep lasted
# that long on the capture host's clock, within one percent, by the arrival stamps the capture
# route writes beside the log (<log>.times, `<seconds>\t<line>`), and nothing panicked.
#
#   KOS_CAPTURE=<log> check_wallclock.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

judge_capture wallclock

jno_panic "a panic in wallclock"
jafter mark-0 1 '[wallclock] mark 0, sleeping ' "first mark" part
jafter mark-1 "$AT" '[wallclock] mark 1, the kernel clock advanced ' "second mark" part
jafter done "$AT" '[wallclock] done' "end line"

REQ="$(printf '%s\n' "$OUT" | sed -n 's/^\[wallclock\] mark 0, sleeping \([0-9][0-9]*\) ns$/\1/p' | tail -n 1)"
[ -n "$REQ" ] || jfail mark-0 "the first mark names no sleep length"
KERN="$(printf '%s\n' "$OUT" | sed -n 's/^\[wallclock\] mark 1, the kernel clock advanced \([0-9][0-9]*\) ns$/\1/p' | tail -n 1)"
[ -n "$KERN" ] || jfail mark-1 "the second mark names no kernel time"
if [ "$KERN" -lt "$REQ" ]; then
    jfail sleep-short "the kernel clock advanced $KERN ns over a sleep of $REQ ns"
fi

TIMES="$KOS_CAPTURE.times"
[ -s "$TIMES" ] || jfail no-times "no arrival stamps at $TIMES, so no clock outside the chip times the sleep"
HOST="$(tr -d '\r' < "$TIMES" | awk -F '\t' '
    index($2, "[wallclock] mark 0, sleeping ") == 1 { t0 = $1; t1 = "" }
    index($2, "[wallclock] mark 1, the kernel clock advanced ") == 1 && t0 != "" && t1 == "" { t1 = $1 }
    END { if (t0 != "" && t1 != "") { printf "%.0f\n", (t1 - t0) * 1000000000 } }')"
[ -n "$HOST" ] || jfail no-times "the arrival stamps carry no mark 0 followed by a mark 1"

DEV="$(awk -v h="$HOST" -v r="$REQ" 'BEGIN { d = h - r; if (d < 0) { d = -d } printf "%.0f\n", d }')"
if [ "$DEV" -gt "$((REQ / 100))" ]; then
    jfail host-time "a sleep of $REQ ns took $HOST ns on the capture host, off by more than one percent"
fi
echo "PASS: a sleep of $REQ ns took $HOST ns on the capture host and $KERN ns on the kernel clock"
