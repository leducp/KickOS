#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
"""Read the M9.7 emulator exit rows with one strict reader.

The distribution pattern also matches the M8.12 row format quoted in
archived `M8.12_meas.md`, lines 238-239.

Input files are <preset>-runN.body, the unedited captured console bodies. The
phase report has several workload windows: the last report of each distribution
sets the exit row, including an empty report that clears an earlier sample.
The first untagged CALL/REPLY rate is the fast path.
"""

import argparse
from pathlib import Path
import re
from statistics import median


PRESETS = (
    "qemu-riscv-bench",
    "qemu-arm64-bench",
    "qemu-riscv64-bench",
    "qemu-x86_64-bench",
    "qemu-arm64-benchsmp",
    "qemu-riscv64-benchsmp",
)
ROWS = ("switch", "lock-hold", "rt8", "rt256", "lock-wait", "doorbell",
        "e2e-local", "e2e-cross")
DIST = re.compile(
    r"^  (switch|lock-hold|lock-wait|doorbell|e2e-local|e2e-cross):"
    r"\s+(\d+)/(\d+)/(\d+)\s+(cyc|ns)"
    r"(?:\s+\d+/\d+/\d+ ns)?\s+\(p50/(?:p99|max)/max, n=(\d+)\)"
)
ROUND_TRIP = re.compile(r"^  call/reply: (8|256) B\s+(\d+) ns/round-trip")


def read_capture(path: Path) -> dict[str, int]:
    values: dict[str, int] = {}
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        match = DIST.match(line)
        if match:
            row = match.group(1)
            if int(match.group(6)) == 0:
                values.pop(row, None)
            else:
                values[row] = int(match.group(2))
            continue
        match = ROUND_TRIP.match(line)
        if match and "[" not in line:
            values.setdefault("rt" + match.group(1), int(match.group(2)))
    required = {"switch", "lock-hold", "rt8", "rt256", "e2e-local"}
    if "smp" in path.name:
        required.update(("lock-wait", "doorbell"))
    missing = sorted(required - values.keys())
    if missing:
        raise ValueError(f"{path}: missing sampled row(s): {', '.join(missing)}")
    return values


def capture_set(directory: Path, preset: str, runs: int) -> list[Path]:
    found = set(directory.glob(preset + "-run*.body"))
    expected = [directory / f"{preset}-run{i}.body" for i in range(1, runs + 1)]
    if found != set(expected):
        raise ValueError(f"{preset}: expected run1..run{runs}.body; found "
                         + ", ".join(path.name for path in sorted(found)))
    return expected


def cells(directory: Path, preset: str, runs: int) -> str:
    captures = [read_capture(path) for path in capture_set(directory, preset, runs)]
    result = []
    for row in ROWS:
        samples = [cap[row] for cap in captures if row in cap]
        if not samples:
            result.append(row + "=-")
        elif len(samples) != runs:
            raise ValueError(f"{preset}: {row} sampled in {len(samples)}/{runs} captures")
        else:
            mid = int(median(samples))
            low, high = min(samples), max(samples)
            result.append(f"{row}={mid}" + (f" [{low}-{high}]" if low != high else ""))
    return f"{preset:<22s} n={runs}  " + "  ".join(result)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="directory of captured .body files")
    parser.add_argument("--runs", type=int, default=5, help="required runs per preset")
    args = parser.parse_args()
    if args.runs < 1:
        parser.error("--runs must be positive")
    try:
        output = [cells(args.directory, preset, args.runs) for preset in PRESETS]
    except ValueError as exc:
        parser.error(str(exc))
    print("\n".join(output))


if __name__ == "__main__":
    main()
