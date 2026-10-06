#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# tools/bench/stamp_lines.py, started and stopped through tools/bench/stamper.sh as the capture
# runs it, over logs this script writes: each line stamped once and in order, a log rewritten under
# it read as a new stream, a last line with no newline kept, its exit once the watched process is
# gone, a zombie or another user's, and a start refused when the stamper is not running.

set -u
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root
scratch_dir
rc=0
command -v python3 > /dev/null || fail "python3 not found"
command -v ps > /dev/null || fail "ps not found"
. tools/bench/stamper.sh
HERE="$PWD/tools/bench"

# <file> <text>: waits a bounded while until <file> holds a stamped line reading <text>.
stamped() {
    local n=0
    until cut -f2- "$1" 2>/dev/null | grep -qxF "$2"; do
        n=$((n + 1))
        if [ "$n" -ge 1000 ]; then
            bad "no stamp of '$2' in $1"
            return 1
        fi
        sleep 0.01
    done
}

# A process that lives until this script ends, its pid in $!; <code> runs first.
follower() {
    python3 -c 'import os, sys, time
exec(sys.argv[2])
while True:
    time.sleep(0.05)
    try:
        os.kill(int(sys.argv[1]), 0)
    except OSError:
        break' "$$" "${1:-pass}" &
}

# <pid>: waits a bounded while for <pid> to end; returns 1 if it outlives that.
ends() {
    local n=0
    while stamper_alive "$1"; do
        n=$((n + 1))
        [ "$n" -lt 500 ] || return 1
        sleep 0.01
    done
}

WATCH=$$

# Lines appended across polls, a rewrite under a stopped stamper that outgrows the stamped bytes,
# and a last line with no newline.
LOG="$TMP/grow.log"
: > "$LOG"
stamper_start "$HERE" "$LOG" "$WATCH" || fail "$STAMPER_WHY"
printf 'one\r\n' >> "$LOG"
stamped "$LOG.times" one
printf 'two\n' >> "$LOG"
stamped "$LOG.times" two
printf 'thr' >> "$LOG"
kill -STOP "$STAMPER"
printf 'new-one\nnew-two\nnew-three, longer than the stream it replaces\n' > "$LOG"
kill -CONT "$STAMPER"
stamped "$LOG.times" 'new-three, longer than the stream it replaces'
kill -STOP "$STAMPER"
printf 'same-1\nsame-2\nsame-3, the length of the stream it replaces...\n' > "$LOG"
kill -CONT "$STAMPER"
stamped "$LOG.times" 'same-3, the length of the stream it replaces...'
printf 'tail with no newline' >> "$LOG"
stamper_stop || bad "the stamper fails a clean stop: $STAMPER_WHY"
want='one
two
new-one
new-two
new-three, longer than the stream it replaces
same-1
same-2
same-3, the length of the stream it replaces...
tail with no newline'
got="$(cut -f2- "$LOG.times")"
[ "$got" = "$want" ] || bad "the stamps over a grown and rewritten log read:
$got"
if ! cut -f1 "$LOG.times" | awk 'NR > 1 && $1 < prev { exit 1 } { prev = $1 }'; then
    bad "the stamps go back in time: $(cut -f1 "$LOG.times" | paste -sd ' ' -)"
fi
t1="$(awk -F '\t' '$2 == "one" { print $1 }' "$LOG.times")"
t2="$(awk -F '\t' '$2 == "two" { print $1 }' "$LOG.times")"
awk -v a="$t1" -v b="$t2" 'BEGIN { exit !(b > a) }' \
    || bad "two lines that arrived in separate polls share the stamp $t1"

# A truncation through stamper_truncate keeps no stamp of the bytes it removed.
LOG="$TMP/cut.log"
: > "$LOG"
stamper_start "$HERE" "$LOG" "$WATCH" || fail "$STAMPER_WHY"
printf 'before the cut\n' >> "$LOG"
stamped "$LOG.times" 'before the cut'
stamper_truncate "$HERE" "$LOG" "$WATCH" || bad "a truncation is refused: $STAMPER_WHY"
printf 'after the cut\n' >> "$LOG"
stamped "$LOG.times" 'after the cut'
stamper_stop || bad "the stamper fails a clean stop after a truncation: $STAMPER_WHY"
got="$(cut -f2- "$LOG.times")"
[ "$got" = "after the cut" ] || bad "the stamps after a truncation read:
$got"

# The watched process ending, as a capture's shell does.
LOG="$TMP/end.log"
: > "$LOG"
follower
short=$!
stamper_start "$HERE" "$LOG" "$short" || fail "$STAMPER_WHY"
printf 'last words' >> "$LOG"
kill "$short"
wait "$short" 2>/dev/null
ends "$STAMPER" || bad "the stamper outlives the process it watches"
wait "$STAMPER" 2>/dev/null || bad "the stamper fails when the process it watches ends"
STAMPER=""
[ "$(cut -f2- "$LOG.times")" = "last words" ] \
    || bad "the stamper's own exit drops the last line: $(cat "$LOG.times")"

# A watched process that is a zombie.
LOG="$TMP/zombie.log"
: > "$LOG"
follower 'pid = os.fork()
if pid == 0:
    os._exit(0)
print(pid, flush=True)' > "$TMP/zombie.pid"
parent=$!
until [ -s "$TMP/zombie.pid" ]; do
    sleep 0.01
done
zombie="$(cat "$TMP/zombie.pid")"
until [ "$(ps -o stat= -p "$zombie" | cut -c1)" = Z ]; do
    sleep 0.01
done
python3 tools/bench/stamp_lines.py "$LOG" "$zombie" &
undead=$!
ends "$undead" || bad "the stamper outlives a watched process that is a zombie"
kill "$undead" 2>/dev/null
wait "$undead" 2>/dev/null
kill "$parent"
wait "$parent" 2>/dev/null

# A watched pid another user's process holds.
if [ "$(id -u)" -ne 0 ]; then
    LOG="$TMP/other.log"
    : > "$LOG"
    python3 tools/bench/stamp_lines.py "$LOG" 1 2> "$TMP/other.err" &
    other=$!
    ends "$other" || bad "the stamper outlives a watched pid another user holds"
    wait "$other" || bad "the stamper fails on a watched pid another user holds: $(tail -n 1 "$TMP/other.err")"
else
    echo "NOTE: running as root, so no watched pid can belong to another user; that case is not run"
fi

# A start whose stamper is not running is refused, and leaves no earlier stamps behind.
mkdir -p "$TMP/dead"
printf 'import sys\nsys.exit(1)\n' > "$TMP/dead/stamp_lines.py"
LOG="$TMP/dead.log"
: > "$LOG"
printf '1.000000\tstale\n' > "$LOG.times"
if stamper_start "$TMP/dead" "$LOG" "$WATCH"; then
    bad "a start whose stamper exits at once is accepted"
    stamper_stop
fi
[ -e "$LOG.times" ] && bad "an earlier capture's stamps survive a refused start"

# A stamper that fails after it is ready, mid-capture or at its stop, is refused by the stop.
mkdir -p "$TMP/late"
printf '%s\n' 'import os, signal, sys, time' \
    'open(sys.argv[1] + ".times", "w").close()' \
    'signal.signal(signal.SIGTERM, lambda *_: sys.exit(3))' \
    'while os.path.getsize(sys.argv[1]) == 0:' \
    '    time.sleep(0.01)' \
    'sys.exit(1)' > "$TMP/late/stamp_lines.py"
LOG="$TMP/late.log"
: > "$LOG"
stamper_start "$TMP/late" "$LOG" "$WATCH" || bad "a start whose stamper is running is refused: $STAMPER_WHY"
if stamper_stop; then
    bad "a stamper that fails at its stop is accepted"
fi
: > "$LOG"
stamper_start "$TMP/late" "$LOG" "$WATCH" || bad "a start whose stamper is running is refused: $STAMPER_WHY"
late=$STAMPER
printf 'a line\n' >> "$LOG"
ends "$late" || bad "the planted stamper did not end"
if stamper_stop; then
    bad "a stamper that failed during the capture is accepted"
fi

# The capture truncates its log under no running stamper: through stamper_truncate, or before
# stamper_start.
awk '/stamper_start / { started = 1 }
     started && /(^|[^>])>[ \t]*"?\$LOG"?([^.A-Za-z_]|$)/ { print FILENAME ":" FNR ": " $0 }' \
    tools/bench/bench-capture.sh > "$TMP/cuts"
while read -r cut; do
    bad "$cut truncates the log under a running stamper"
done < "$TMP/cuts"
grep -q 'stamper_start ' tools/bench/bench-capture.sh || bad "bench-capture.sh starts no stamper"

[ "$rc" -eq 0 ] || exit 1
echo "PASS: the stamper stamps each line once, follows a rewritten log and keeps its last line"
