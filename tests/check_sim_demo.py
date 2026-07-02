#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for the KickOS M0 sim demo. Runs the hello ELF and asserts that every
# M0 verification bullet is observable in the output, in the required order.

import subprocess
import sys


def fail(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def main():
    if len(sys.argv) < 2:
        fail("usage: check_sim_demo.py <hello-elf>")
    elf = sys.argv[1]

    try:
        proc = subprocess.run([elf], capture_output=True, text=True, timeout=25)
    except subprocess.TimeoutExpired:
        fail("demo timed out (likely a scheduler hang / missing switch)")

    out = proc.stdout
    print(out, end="")

    if proc.returncode != 0:
        fail(f"non-zero exit status {proc.returncode}")

    # Nothing should ever print these.
    for bad in ("ERROR", "PANIC"):
        if bad in out:
            fail(f"forbidden marker present: {bad!r}")

    # 1. User SVC roundtrip returns the correct byte count.
    if "[svc] write returned 4" not in out:
        fail("SVC roundtrip did not return the correct result")

    # Ordered markers: each must appear after the previous one.
    ordered = [
        # 2. Two-thread FIFO ordering.
        "[fifo] A running",
        "[fifo] B running",
        # 3. Higher-priority thread preempts on ready (thread-ctx sem post).
        "[preempt] low before",
        "[preempt] high preempts",
        "[preempt] low after",
        # 4. Semaphore posted from an IRQ handler switches (IRQ ctx, tick off).
        "[irq] injecting",
        "[irq] woken by IRQ handler",
        "[irq] injector resumes",
        # 6. Sleep ordering via the tickless timer queue (short before long).
        "[sleep] 10ms woke",
        "[sleep] 40ms woke",
        # Privileged guard access survives a syscall (regression: trap epilogue
        # must restore the caller's MPU posture, not force PROT_NONE).
        "[mpu] privileged guard write ok",
        # Completion, then the reported MPU fault (unprivileged wild write).
        "DEMO COMPLETE",
        "MPU FAULT",
    ]
    pos = -1
    for marker in ordered:
        idx = out.find(marker, pos + 1)
        if idx < 0:
            fail(f"missing or out-of-order marker: {marker!r}")
        pos = idx

    # 5. RR interleaves equal-priority threads: peer B's first iteration must
    #    appear before A's second (a pure-FIFO scheduler would run A to
    #    completion first).
    for m in ("[rr] A 1", "[rr] B 1", "[rr] A 2", "[rr] B 2", "[rr] A 3", "[rr] B 3"):
        if m not in out:
            fail(f"missing RR marker: {m!r}")
    if not out.index("[rr] B 1") < out.index("[rr] A 2"):
        fail("RR did not round-robin (B's slice never interleaved A's)")

    print("PASS: all M0 verification bullets observed")
    sys.exit(0)


if __name__ == "__main__":
    main()
