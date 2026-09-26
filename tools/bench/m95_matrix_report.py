#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
"""Reduce run_m95_matrix.sh captures to a checked, machine-readable TSV."""

import re
import sys
from pathlib import Path


RATE = re.compile(r"^ipc-pairs: len=(\d+) cores=(\d+) calls=(\d+) rate=(\d+)/s$", re.M)
WINDOW = re.compile(
    r"^ipc-pairs: len=(\d+) wall_ms=(\d+) span_ms=(\d+) overlap_ms=(\d+)$", re.M
)
PHASE = re.compile(r"^    (CALL_COPY|REPLY_COPY)\s+(\d+)/\d+ .*? n=(\d+)$", re.M)
WAIT = re.compile(r"^  lock-wait: .*?  (\d+)/(\d+)/(\d+) ns  .*?n=(\d+)\)$", re.M)
LABEL = re.compile(r"^(fast1|fast2|fast4|slow1|slow2|slow4|slow8|mixed12)-r(\d+)$")
PIN = re.compile(r"^vCPU (\d+) -> host CPU (\d+); tid \d+$", re.M)
CPUSET = {
    "fast1": "0-1", "fast2": "0-1", "fast4": "0-3",
    "slow1": "4-5", "slow2": "4-5", "slow4": "4-7",
    "slow8": "4-11", "mixed12": "0-11",
}


def extract(run: Path) -> list[tuple[str, ...]]:
    match = LABEL.fullmatch(run.name)
    if match is None:
        raise ValueError(f"unexpected run directory: {run}")
    label, rep = match.groups()
    if run.parent.name == "runs-pinned":
        cores = int(label.removeprefix("fast").removeprefix("slow").removeprefix("mixed"))
        vcpus = max(2, cores)
        lo, hi = map(int, CPUSET[label].split("-"))
        expected = [(i, lo + i) for i in range(vcpus)]
        pins = [(int(i), int(cpu)) for i, cpu in PIN.findall((run / "pin.txt").read_text())]
        if pins != expected or len(pins) != len(set(pins)) or lo + vcpus - 1 > hi:
            raise ValueError(f"bad vCPU pin map: {run}")
    log = (run / "run.txt").read_text()
    if "ipc-pairs: done\n" not in log or "KICKOS-EXIT status 0\n" not in log:
        raise ValueError(f"incomplete capture: {run}")
    rates = RATE.findall(log)
    windows = WINDOW.findall(log)
    if len(rates) != 2 or len(windows) != 2:
        raise ValueError(f"expected two payload reports: {run}")
    blocks = re.split(r"^ipc-pairs: report len=(\d+) cores=(\d+)$", log, flags=re.M)
    if len(blocks) != 7:
        raise ValueError(f"expected two phase reports: {run}")
    by_len = {}
    for pos in (1, 4):
        length, cores, body = blocks[pos : pos + 3]
        phases = {name: (int(cycles), int(count)) for name, cycles, count in PHASE.findall(body)}
        if set(phases) != {"CALL_COPY", "REPLY_COPY"}:
            raise ValueError(f"missing copy phase: {run}, payload {length}")
        wait = WAIT.search(body)
        wait_values = wait.groups() if wait is not None else ("0", "0", "0", "0")
        by_len[int(length)] = (int(cores), phases, wait_values)
    rows = []
    for rate, window in zip(rates, windows):
        length, cores, calls, per_second = map(int, rate)
        window_len, wall, span, overlap = map(int, window)
        if length != window_len or length not in by_len or cores != by_len[length][0]:
            raise ValueError(f"mismatched report: {run}")
        if calls != cores * 50000 or overlap <= 0 or overlap > span or span > wall:
            raise ValueError(f"bad count or active window: {run}")
        phases, wait = by_len[length][1:]
        if phases["CALL_COPY"][1] != calls or phases["REPLY_COPY"][1] != calls:
            raise ValueError(f"phase population differs from calls: {run}")
        rows.append(tuple(map(str, (
            label, CPUSET[label], rep, cores, length, calls, wall, span, overlap,
            per_second, *wait, phases["CALL_COPY"][0], phases["REPLY_COPY"][0],
        ))))
    return rows


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: m95_matrix_report.py <runs-dir>")
    root = Path(sys.argv[1])
    runs = sorted(p for p in root.iterdir() if p.is_dir() and LABEL.fullmatch(p.name))
    if not runs:
        raise SystemExit(f"no run directories in {root}")
    print("label\thost_cpu_set\trep\tkernel_cores\tpayload_bytes\tcalls\twall_ms\t"
          "span_ms\toverlap_ms\trate_per_s\tlock_wait_p50_ns\tlock_wait_p99_ns\t"
          "lock_wait_max_ns\tlock_wait_n\tcall_copy_avg_cycles\treply_copy_avg_cycles")
    for run in runs:
        for row in extract(run):
            print("\t".join(row))


if __name__ == "__main__":
    main()
