#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Stamp each line a capture's reader appends to <log> with the host time of its arrival, into
# <log>.times as `<seconds>\t<line>`: the clock outside the board a timing judge reads.
#
#   stamp_lines.py <log> <watched-pid>
#
# <log>.times exists once it is ready. Runs until SIGTERM, or until <watched-pid> is gone or a
# zombie, and writes a last line that has no newline at exit.
#
# A stamp is the first poll that saw the line's newline: never earlier than its arrival, later by
# at most the gap since the poll before, and every line completed within one poll shares it.
# A log that shrinks, or whose stamped bytes change, is a new stream from its first byte. A
# rewrite within one poll that reproduces the stamped bytes at the head and just ahead of the
# read point reads as the same stream, so the reproduced lines keep their earlier stamps.
import os
import signal
import sys
import time

POLL_S = 0.002
WINDOW = 4096


def gone(pid):
    try:
        os.kill(pid, 0)
    except (ProcessLookupError, PermissionError):
        return True
    try:
        with open("/proc/%d/stat" % pid, "rb") as f:
            stat = f.read()
    except OSError:
        return False
    end = stat.rfind(b")")
    return stat[end + 2:end + 3] == b"Z"


def main():
    log, watched = sys.argv[1], int(sys.argv[2])
    stop = []
    signal.signal(signal.SIGTERM, lambda *_: stop.append(True))
    t0 = time.monotonic()
    fd = None
    offset = 0
    head = b""
    tail = b""
    pending = b""
    pending_at = 0.0
    with open(log + ".times", "wb") as times:

        def stamp(at, line):
            times.write(b"%.6f\t" % at + line.rstrip(b"\r") + b"\n")

        while True:
            last = bool(stop)
            try:
                ino = os.stat(log).st_ino
            except FileNotFoundError:
                ino = None
            if ino is not None and (fd is None or os.fstat(fd).st_ino != ino):
                try:
                    opened = os.open(log, os.O_RDONLY)
                except FileNotFoundError:
                    opened = None
                if opened is not None:
                    if fd is not None:
                        os.close(fd)
                    fd = opened
                    offset, head, tail, pending = 0, b"", b"", b""
            if fd is not None:
                size = os.fstat(fd).st_size
                now = time.monotonic() - t0
                if size < offset or (offset > 0 and (
                        os.pread(fd, len(head), 0) != head
                        or os.pread(fd, len(tail), offset - len(tail)) != tail)):
                    offset, head, tail, pending = 0, b"", b"", b""
                    continue
                if size > offset:
                    data = os.pread(fd, size - offset, offset)
                    offset += len(data)
                    head += data[:WINDOW - len(head)]
                    tail = (tail + data)[-WINDOW:]
                    pending += data
                    pending_at = now
                    while b"\n" in pending:
                        line, _, pending = pending.partition(b"\n")
                        stamp(now, line)
                    times.flush()
            if last:
                if pending:
                    stamp(pending_at, pending)
                times.flush()
                return
            if gone(watched):
                stop.append(True)
                continue
            time.sleep(POLL_S)


if __name__ == "__main__":
    main()
