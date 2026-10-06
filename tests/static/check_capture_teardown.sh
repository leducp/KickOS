#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# tools/bench/bench-capture.sh, run over stub flash tools and a pseudo-terminal console, leaves no
# console reader and no stamper behind a refusal: a stamper failing at the capture window's end
# under the wrapped reader, one failing at the f411disco cut under its bare reader, and a failed
# load under the wrapped reader.

set -u
. "$(dirname "$0")/../lib/gate.sh"

require_repo_root
scratch_dir
rc=0
command -v python3 > /dev/null || fail "python3 not found"
command -v pgrep > /dev/null || fail "pgrep not found"

python3 -c 'import os, pty, sys, time
m, s = pty.openpty()
print(os.ttyname(s), flush=True)
os.close(s)
time.sleep(600)' > "$TMP/pts" &
KOS_CHILD_PID=$!
n=0
until [ -s "$TMP/pts" ]; do
    n=$((n + 1))
    [ "$n" -lt 500 ] || fail "no pseudo-terminal was opened"
    sleep 0.01
done
ln -s "$(cat "$TMP/pts")" "$TMP/tty"

mkdir -p "$TMP/root/tools" "$TMP/root/boards" "$TMP/bin" "$TMP/late"
cp -R boards/picopi boards/f411disco "$TMP/root/boards/"
printf '#!/bin/sh\nexit 0\n' > "$TMP/root/tools/flash.sh"
printf '#!/bin/sh\nexit "${STUB_LOAD_RC:-0}"\n' > "$TMP/root/tools/flash-picotool.sh"
printf '#!/bin/sh\nexit 0\n' > "$TMP/bin/picotool"
printf '#!/bin/sh\nexit 0\n' > "$TMP/bin/st-flash"
chmod +x "$TMP/root/tools/flash.sh" "$TMP/root/tools/flash-picotool.sh" "$TMP/bin/picotool" \
    "$TMP/bin/st-flash"
printf 'RIG_CONSOLE_PICOPI=%s\nRIG_CONSOLE_F411DISCO=%s\n' "$TMP/tty" "$TMP/tty" > "$TMP/rig.conf"
: > "$TMP/image"
: > "$TMP/image.bin"

# The capture's scripts beside a stamper that is ready, then fails when it is stopped.
cp tools/bench/*.sh tools/bench/*.py "$TMP/late/"
printf '%s\n' 'import signal, sys, time' \
    'open(sys.argv[1] + ".times", "w").close()' \
    'signal.signal(signal.SIGTERM, lambda *_: sys.exit(3))' \
    'while True:' \
    '    time.sleep(0.01)' > "$TMP/late/stamp_lines.py"

# <pattern>: waits a bounded while for every process whose command line holds <pattern> to end;
# returns 1, ending them, if one outlives that.
gone() {
    local n=0
    while pgrep -f "$1" > /dev/null; do
        n=$((n + 1))
        if [ "$n" -ge 500 ]; then
            pkill -f "$1"
            return 1
        fi
        sleep 0.01
    done
}

# <name> <board> <capture script> <word in the refusal> [VAR=value]...: the capture refuses, saying
# <word>, and leaves no reader of the console and no stamper running.
refuses() {
    local name=$1 board=$2 script=$3 word=$4
    shift 4
    if env "$@" ROOT="$TMP/root" KICKOS_RIG="$TMP/rig.conf" PATH="$TMP/bin:$PATH" CAP_SECS=1 \
           EXPECT_COMMIT=0123abcd EXPECT_ARCH=armv6m BENCH_HOST= PEER_ERASE= \
           bash "$script" "$board" planted "$TMP/image" "$TMP/$name.log" > "$TMP/$name.out" 2>&1; then
        bad "$name: the capture is accepted"
    elif ! grep -qF -e "$word" "$TMP/$name.out"; then
        bad "$name: the capture refuses, but not for '$word': $(grep REFUSING "$TMP/$name.out")"
    fi
    gone "$TMP/tty" || bad "$name: a reader of the console outlives the refusal"
    gone "$TMP/$name.log" || bad "$name: the stamper outlives the refusal"
}

refuses window-end picopi "$TMP/late/bench-capture.sh" 'stamp_lines.py failed during the capture'
refuses cut f411disco "$TMP/late/bench-capture.sh" 'stamp_lines.py failed during the capture'
refuses load picopi "$PWD/tools/bench/bench-capture.sh" 'picotool could not flash' STUB_LOAD_RC=1

[ "$rc" -eq 0 ] || exit 1
echo "PASS: a refused capture leaves no console reader and no stamper running"
