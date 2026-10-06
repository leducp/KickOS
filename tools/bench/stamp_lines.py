#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Stamp each line a capture's reader appends to <log> with the host time of its arrival, into
# <log>.times as `<seconds>\t<line>`: the clock outside the board a timing judge reads.
#
#   stamp_lines.py <log> <watched-pid>
#
# Runs until SIGTERM, or until <watched-pid> is gone. A log that shrinks was truncated by its
# capture, so its later bytes are a new stream.
import os
import signal
import sys
import time

POLL_S = 0.002


def main():
    log, watched = sys.argv[1], int(sys.argv[2])
    stop = []
    signal.signal(signal.SIGTERM, lambda *_: stop.append(True))
    t0 = time.monotonic()
    offset = 0
    pending = b""
    with open(log + ".times", "wb") as times:
        while True:
            last = bool(stop)
            try:
                size = os.stat(log).st_size
            except FileNotFoundError:
                size = 0
            if size < offset:
                offset = 0
                pending = b""
            if size > offset:
                now = time.monotonic() - t0
                with open(log, "rb") as f:
                    f.seek(offset)
                    data = f.read(size - offset)
                offset += len(data)
                pending += data
                while b"\n" in pending:
                    line, _, pending = pending.partition(b"\n")
                    times.write(b"%.6f\t" % now + line.rstrip(b"\r") + b"\n")
                times.flush()
            if last:
                return
            try:
                os.kill(watched, 0)
            except ProcessLookupError:
                return
            time.sleep(POLL_S)


if __name__ == "__main__":
    main()
