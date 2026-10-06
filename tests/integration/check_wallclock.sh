#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# A silicon capture of wallclock (user/apps/common/wallclock): between its second and third marks
# the sleep the app declares took that long, within one percent, on the kernel clock and on the
# capture host's clock alike, by the arrival stamps the capture route writes beside the log
# (<log>.times, `<seconds>\t<line>`, whose mark lines must be this log's), and nothing panicked.
# The first mark is never needed. In the app's source every clock read that opens an interval is
# followed by the sleep and the read that closes it, so no print falls inside a timed interval.
#
#   KOS_CAPTURE=<log> check_wallclock.sh <board-build> <kickos-source> <cmake>

set -u
. "$(dirname "$0")/../lib/gate.sh"

SRC="${2:?usage: check_wallclock.sh <board-build> <kickos-source> <cmake>}/user/apps/common/wallclock/main.cc"
REQ="$(sed -n 's/^ *constexpr uint64_t SLEEP_NS = \([0-9][0-9]*\)ull;$/\1/p' "$SRC")"
[ -n "$REQ" ] || fail "$SRC declares no SLEEP_NS"
TOL=$((REQ / 100))
# C a clock read, S a sleep, P a print, in source order with comments gone.
SHAPE="$(sed 's|//.*||' "$SRC" | awk '
    /clock_now\(/ { printf "C" }
    /sleep_ns\(/ { printf "S" }
    /print\(|printf\(|puts\(/ { printf "P" }')"
case "$SHAPE" in
    *CSC*) ;;
    *) jfail interval "$SRC times no sleep between two clock reads (reads [$SHAPE])" ;;
esac
case "$(printf '%s' "$SHAPE" | sed 's/CSC//g')" in
    *C*|*S*) jfail interval "$SRC has a clock read or a sleep outside a read, sleep, read run, so a timed interval holds more than its sleep (reads [$SHAPE])" ;;
esac

judge_capture wallclock

jno_panic "a panic in wallclock"
jafter mark-1 1 '[wallclock] mark 1, the kernel clock advanced ' "second mark" part
M1="$(printf '%s\n' "$OUT" | sed -n "${AT}p")"
jafter mark-2 "$AT" '[wallclock] mark 2, the kernel clock advanced ' "third mark" part
M2="$(printf '%s\n' "$OUT" | sed -n "${AT}p")"
jafter done "$AT" '[wallclock] done' "end line"

SLEPT="$(printf '%s\n' "$M1" | sed -n 's/^\[wallclock\] mark 1, the kernel clock advanced [0-9][0-9]* ns, sleeping \([0-9][0-9]*\) ns$/\1/p')"
[ "$SLEPT" = "$REQ" ] || jfail declared "the second mark sleeps '$SLEPT' ns, not the $REQ ns the app declares"
M0="$(printf '%s\n' "$OUT" | grep -a '^\[wallclock\] mark 0, ' | grep -avxF "[wallclock] mark 0, sleeping $REQ ns")"
[ -z "$M0" ] || jfail declared "the first mark reads '$M0', not a sleep of the $REQ ns the app declares"
KERN="$(printf '%s\n' "$M2" | sed -n 's/^\[wallclock\] mark 2, the kernel clock advanced \([0-9][0-9]*\) ns$/\1/p')"
[ -n "$KERN" ] || jfail mark-2 "the third mark names no kernel time"
if [ "$KERN" -lt "$REQ" ]; then
    jfail sleep-short "the kernel clock advanced $KERN ns over a sleep of $REQ ns"
fi
if [ "$((KERN - REQ))" -gt "$TOL" ]; then
    jfail kernel-long "the kernel clock advanced $KERN ns over a sleep of $REQ ns, over one percent more"
fi

TIMES="$KOS_CAPTURE.times"
[ -s "$TIMES" ] || jfail no-times "no arrival stamps at $TIMES, so no clock outside the chip times the sleep"
_marks='^\[wallclock\] mark [0-9]'
LOGMARKS="$(tr -d '\r' < "$KOS_CAPTURE" | grep -a "$_marks")"
STAMPMARKS="$(tr -d '\r' < "$TIMES" | cut -f2- | grep -a "$_marks")"
[ "$LOGMARKS" = "$STAMPMARKS" ] \
    || jfail stamps "the arrival stamps' mark lines are not this capture's: [$STAMPMARKS] against [$LOGMARKS]"
HOST="$(tr -d '\r' < "$TIMES" | KOS_M1="$M1" KOS_M2="$M2" awk -F '\t' '
    BEGIN { m1 = ENVIRON["KOS_M1"]; m2 = ENVIRON["KOS_M2"] }
    { line = substr($0, index($0, "\t") + 1) }
    t1 == "" && line == m1 { t1 = $1; next }
    t1 != "" && line == m2 { printf "%.0f\n", ($1 - t1) * 1000000000; exit }')"
[ -n "$HOST" ] || jfail no-times "the arrival stamps carry no second mark followed by a third"

DEV="$(awk -v h="$HOST" -v r="$REQ" 'BEGIN { d = h - r; if (d < 0) { d = -d } printf "%.0f\n", d }')"
if [ "$DEV" -gt "$TOL" ]; then
    jfail host-time "a sleep of $REQ ns took $HOST ns on the capture host, off by more than one percent"
fi
DEV="$(awk -v h="$HOST" -v k="$KERN" 'BEGIN { d = h - k; if (d < 0) { d = -d } printf "%.0f\n", d }')"
if [ "$DEV" -gt "$TOL" ]; then
    jfail kernel-host "the kernel clock advanced $KERN ns where the capture host took $HOST ns, more than one percent of the sleep apart"
fi
echo "PASS: a sleep of $REQ ns took $HOST ns on the capture host and $KERN ns on the kernel clock"
