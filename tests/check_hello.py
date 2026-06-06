#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Smoke test for the hello ping-pong demo: it runs until interrupted, so let it
# play for a couple of seconds, send Ctrl+C, and assert it bounced the token and
# halted cleanly (exit 0).

import subprocess
import sys
import time
import signal


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def main():
    if len(sys.argv) < 2:
        fail("usage: check_hello.py <hello-elf>")
    elf = sys.argv[1]

    import os
    import fcntl

    proc = subprocess.Popen([elf], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    # Poll for a couple of exchanges rather than sleeping a fixed time (robust on
    # a loaded CI box), then Ctrl+C. Cap the wait.
    fd = proc.stdout.fileno()
    fcntl.fcntl(fd, fcntl.F_SETFL, fcntl.fcntl(fd, fcntl.F_GETFL) | os.O_NONBLOCK)
    seen = ""
    deadline = time.monotonic() + 10.0
    while "pong 2" not in seen and time.monotonic() < deadline:
        time.sleep(0.05)
        try:
            chunk = proc.stdout.read()
            if chunk:
                seen += chunk
        except (BlockingIOError, TypeError):
            pass
    proc.send_signal(signal.SIGINT)
    try:
        rest, _ = proc.communicate(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        fail("hello did not exit after Ctrl+C")
    out = seen + (rest or "")  # `seen` holds what we polled before the signal

    print(out, end="")
    for bad in ("PANIC", "FAULT", "ERROR"):
        if bad in out:
            fail(f"forbidden marker present: {bad!r}")
    if "ping 1" not in out or "pong 1" not in out:
        fail("ping-pong was not observed")
    if "[KickOS] halted." not in out:
        fail("no graceful halt message on Ctrl+C")
    if proc.returncode != 0:
        fail(f"non-zero exit after Ctrl+C: {proc.returncode}")

    print("PASS: hello ping-ponged and halted cleanly on Ctrl+C")


if __name__ == "__main__":
    main()
