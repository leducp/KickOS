#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
"""Check and tabulate the pinned M9.5 local/cross-core IPC mixtures."""

import re
import sys
from pathlib import Path


CASES = ((4, 1, "0-3"), (12, 1, "0-11"), (12, 6, "0-11"),
         (12, 12, "0-11"))
ROUNDS = 50000


def one(pattern: str, data: str, path: Path) -> tuple[str, ...]:
    found = re.findall(pattern, data, re.MULTILINE)
    if len(found) != 1:
        raise ValueError(f"{path}: expected one {pattern!r}, found {len(found)}")
    value = found[0]
    return value if isinstance(value, tuple) else (value,)


def main() -> None:
    if len(sys.argv) != 2:
        raise SystemExit("usage: m95_mixed_report.py <build/m95>")
    root = Path(sys.argv[1])
    print("cores\thost_cpu_set\tremote_pairs\tmode\trep\tpayload_bytes\t"
          "calls\twall_ms\toverlap_ms\trate_per_s\tlocal_rate_per_s\t"
          "remote_rate_per_s")
    for cores, remote, cpuset in CASES:
        for mode in ("base", "local"):
            for rep in range(1, 4):
                run = root / f"mixed{cores}" / f"{mode}-remote{remote}-r{rep}"
                pin_path = run / "pin.txt"
                log_path = run / "run.txt"
                pins = pin_path.read_text()
                log = log_path.read_text(errors="replace")
                expected = [(str(i), str(i)) for i in range(cores)]
                seen = re.findall(r"^vCPU (\d+) -> host CPU (\d+); tid \d+$",
                                  pins, re.MULTILINE)
                if seen != expected:
                    raise ValueError(f"{pin_path}: incorrect physical-core pin map")
                one(r"^ipc-pairs: done$", log, log_path)
                one(r"^KICKOS-EXIT status 0$", log, log_path)
                active = re.findall(r"^ipc-pairs: active=(\d+) guest=(\d+) remote=(\d+)$",
                                    log, re.MULTILINE)
                if active != [(str(cores), str(cores), str(remote))] * 2:
                    raise ValueError(f"{log_path}: incorrect workload configuration")
                for length in (8, 256):
                    for core in range(cores):
                        one(rf"^ipc-pairs: len={length} core={core} calls={ROUNDS} "
                            rf"active_ms=\d+$", log, log_path)
                    calls, rate = map(int, one(
                        rf"^ipc-pairs: len={length} cores={cores} calls=(\d+) "
                        rf"rate=(\d+)/s$", log, log_path))
                    wall, overlap = map(int, one(
                        rf"^ipc-pairs: len={length} wall_ms=(\d+) span_ms=\d+ "
                        rf"overlap_ms=(\d+)$", log, log_path))
                    class_rates = []
                    for group, expected_calls in ((0, (cores - remote) * ROUNDS),
                                                  (1, remote * ROUNDS)):
                        class_calls, class_rate = map(int, one(
                            rf"^ipc-pairs: len={length} remote={group} calls=(\d+) "
                            rf"rate=(\d+)/s$", log, log_path))
                        if class_calls != expected_calls:
                            raise ValueError(f"{log_path}: wrong class count")
                        class_rates.append(class_rate)
                    if calls != cores * ROUNDS or wall <= 0 or overlap <= 0:
                        raise ValueError(f"{log_path}: incomplete or nonoverlapping run")
                    print(f"{cores}\t{cpuset}\t{remote}\t{mode}\t{rep}\t{length}\t"
                          f"{calls}\t{wall}\t{overlap}\t{rate}\t"
                          f"{class_rates[0]}\t{class_rates[1]}")


if __name__ == "__main__":
    main()
