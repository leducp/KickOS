#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Sourced by bench-capture.sh, never executed. Runs stamp_lines.py beside a capture.

STAMPER=""
STAMPER_WHY=""

# <pid>: the process exists and is not a zombie.
stamper_alive() {
  local state
  state=$(ps -o stat= -p "$1" 2>/dev/null) || return 1
  [ -n "$state" ] && [ "${state#Z}" = "$state" ]
}

# stamper_start <dir> <log> <watched-pid>: <dir>/stamp_lines.py over <log>, with any earlier
# <log>.times removed first. Returns 1, with STAMPER_WHY set, when the stamper is not alive once
# ready. Never call it inside $(...): the stamper must be this shell's child.
stamper_start() {
  rm -f "$2.times"
  python3 "$1/stamp_lines.py" "$2" "$3" &
  STAMPER=$!
  while [ ! -e "$2.times" ] && stamper_alive "$STAMPER"; do
    sleep 0.01
  done
  if ! stamper_alive "$STAMPER"; then
    wait "$STAMPER" 2>/dev/null
    STAMPER=""
    STAMPER_WHY="stamp_lines.py is not running, so no line of this capture can be timed"
    return 1
  fi
}

# stamper_stop: ends the stamper, its last line written. Returns 1, with STAMPER_WHY set, when it
# had already failed.
stamper_stop() {
  local rc
  [ -n "$STAMPER" ] || return 0
  kill -TERM "$STAMPER" 2>/dev/null
  wait "$STAMPER" 2>/dev/null
  rc=$?
  STAMPER=""
  if [ "$rc" -ne 0 ]; then
    STAMPER_WHY="stamp_lines.py failed during the capture (status $rc), so its stamps stop short"
    return 1
  fi
}

# stamper_truncate <dir> <log> <watched-pid>: empties <log> with no stamper running, then starts
# one over what follows.
stamper_truncate() {
  stamper_stop || return 1
  if ! : > "$2"; then
    STAMPER_WHY="cannot truncate $2"
    return 1
  fi
  stamper_start "$1" "$2" "$3"
}
