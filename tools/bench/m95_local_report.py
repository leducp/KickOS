#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
"""Verify and tabulate the pinned M9.5 owner-local IPC captures."""

import argparse
import re
from pathlib import Path


CASES = ((2, "0-1"), (4, "0-3"), (8, "4-11"), (12, "0-11"))
PAYLOADS = (8, 256)
ROUNDS = 50000


def exact(pattern: str, data: str, path: Path) -> re.Match[str]:
    found = list(re.finditer(pattern, data, re.MULTILINE))
    if len(found) != 1:
        raise ValueError(f"{path}: expected exactly one match for {pattern!r}; found {len(found)}")
    return found[0]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=Path, help="build/m95 directory")
    parser.add_argument("--reps", type=int, default=3)
    args = parser.parse_args()
    if args.reps < 1:
        parser.error("--reps must be positive")

    print("cores\thost_cpu_set\trep\tpayload_bytes\tcalls\twall_ms\toverlap_ms\trate_per_s")
    for cores, cpuset in CASES:
        first, last = map(int, cpuset.split("-"))
        expected_cpus = list(range(first, last + 1))
        for rep in range(1, args.reps + 1):
            run = args.build / f"local{cores}" / f"phase-r{rep}"
            pin_path = run / "pin.txt"
            log_path = run / "run.txt"
            pins = pin_path.read_text()
            log = log_path.read_text(errors="replace")
            seen = re.findall(r"^vCPU (\d+) -> host CPU (\d+); tid \d+$", pins, re.MULTILINE)
            expected = [(str(i), str(cpu)) for i, cpu in enumerate(expected_cpus)]
            if seen != expected:
                raise ValueError(f"{pin_path}: expected {expected}, found {seen}")
            exact(r"^ipc-pairs: done$", log, log_path)
            exact(r"^KICKOS-EXIT status 0$", log, log_path)
            for size in PAYLOADS:
                for core in range(cores):
                    exact(rf"^ipc-pairs: len={size} core={core} calls={ROUNDS} active_ms=\d+$",
                          log, log_path)
                result = exact(
                    rf"^ipc-pairs: len={size} cores={cores} calls=(\d+) rate=(\d+)/s$",
                    log, log_path)
                window = exact(
                    rf"^ipc-pairs: len={size} wall_ms=(\d+) span_ms=\d+ overlap_ms=(\d+)$",
                    log, log_path)
                calls, rate = map(int, result.groups())
                wall, overlap = map(int, window.groups())
                if calls != ROUNDS * cores or wall <= 0 or overlap <= 0:
                    raise ValueError(f"{log_path}: incomplete or non-overlapping {size}-byte run")
                print(f"{cores}\t{cpuset}\t{rep}\t{size}\t{calls}\t{wall}\t{overlap}\t{rate}")


if __name__ == "__main__":
    main()
