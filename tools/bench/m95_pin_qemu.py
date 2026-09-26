#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
"""Start paused QEMU, pin each KVM vCPU to one host CPU, then run it."""

import json
import os
import re
import socket
import subprocess
import sys
import time
from pathlib import Path


def cpu_list(spec: str) -> list[int]:
    result = []
    for part in spec.split(","):
        bounds = part.split("-")
        if len(bounds) == 1:
            result.append(int(bounds[0]))
        elif len(bounds) == 2:
            lo, hi = map(int, bounds)
            if lo > hi:
                raise ValueError(f"reversed CPU range: {part}")
            result.extend(range(lo, hi + 1))
        else:
            raise ValueError(f"invalid CPU range: {part}")
    if not result or len(result) != len(set(result)):
        raise ValueError("CPU set is empty or contains duplicates")
    return result


def qmp_command(stream, request: str) -> None:
    stream.write(json.dumps({"execute": request}).encode() + b"\n")
    stream.flush()
    while True:
        line = stream.readline()
        if not line:
            raise RuntimeError(f"QMP closed while waiting for {request}")
        answer = json.loads(line)
        if "error" in answer:
            raise RuntimeError(f"QMP {request}: {answer['error']}")
        if "return" in answer:
            return


def thread_ticks(pid: int, tid: int) -> int:
    # comm is parenthesized and may contain spaces; fields after it start at #3.
    stat = Path(f"/proc/{pid}/task/{tid}/stat").read_text()
    fields = stat[stat.rfind(")") + 2 :].split()
    return int(fields[11]) + int(fields[12])  # utime + stime


def run(out: Path, cpus: list[int], vcpus: int, command: list[str]) -> int:
    if len(cpus) < vcpus:
        raise ValueError("one distinct host CPU is required per QEMU vCPU")
    qmp_path = out / "qmp.sock"
    qmp_path.unlink(missing_ok=True)
    with (out / "run.log").open("wb") as log:
        proc = subprocess.Popen(
            command + ["-S", "-qmp", f"unix:{qmp_path},server=on,wait=off"],
            stdout=log,
            stderr=subprocess.STDOUT,
            start_new_session=True,
        )
        try:
            deadline = time.monotonic() + 15
            threads: dict[int, int] = {}
            while time.monotonic() < deadline and len(threads) != vcpus:
                if proc.poll() is not None:
                    raise RuntimeError(f"QEMU exited before vCPU pinning: {proc.returncode}")
                for task in Path(f"/proc/{proc.pid}/task").iterdir():
                    tid = int(task.name)
                    os.sched_setaffinity(tid, cpus)
                    match = re.fullmatch(r"CPU (\d+)/KVM", (task / "comm").read_text().strip())
                    if match:
                        threads[int(match.group(1))] = tid
                if len(threads) != vcpus:
                    time.sleep(0.01)
            if set(threads) != set(range(vcpus)):
                raise RuntimeError(f"found KVM vCPU threads {sorted(threads)}, expected 0..{vcpus - 1}")
            for guest, host in enumerate(cpus[:vcpus]):
                os.sched_setaffinity(threads[guest], {host})
            with (out / "pin.txt").open("w") as pin:
                for guest, host in enumerate(cpus[:vcpus]):
                    pin.write(f"vCPU {guest} -> host CPU {host}; tid {threads[guest]}\n")
            first_ticks = {guest: thread_ticks(proc.pid, tid) for guest, tid in threads.items()}
            last_ticks = first_ticks.copy()
            while not qmp_path.exists() and time.monotonic() < deadline:
                time.sleep(0.01)
            with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
                sock.settimeout(max(0.1, deadline - time.monotonic()))
                sock.connect(str(qmp_path))
                stream = sock.makefile("rwb", buffering=0)
                if not stream.readline():
                    raise RuntimeError("no QMP greeting")
                qmp_command(stream, "qmp_capabilities")
                qmp_command(stream, "cont")
            run_limit = float(os.environ.get("KICKOS_M95_RUN_LIMIT_S", "120"))
            if run_limit <= 0:
                raise ValueError("KICKOS_M95_RUN_LIMIT_S must be positive")
            started = time.monotonic()
            while proc.poll() is None and time.monotonic() - started < run_limit:
                for guest, tid in threads.items():
                    try:
                        last_ticks[guest] = thread_ticks(proc.pid, tid)
                    except (FileNotFoundError, ProcessLookupError):
                        # /proc may vanish while the guest exits cleanly.
                        pass
                time.sleep(0.01)
            elapsed = time.monotonic() - started
            clock_ticks = os.sysconf("SC_CLK_TCK")
            with (out / "cpu.txt").open("w") as cpu:
                cpu.write(f"QEMU running wall time {elapsed:.3f} s (includes firmware)\n")
                for guest in range(vcpus):
                    seconds = (last_ticks[guest] - first_ticks[guest]) / clock_ticks
                    cpu.write(f"vCPU {guest} host CPU {cpus[guest]} CPU time {seconds:.3f} s\n")
            if proc.poll() is None:
                return 124
            return proc.returncode
        finally:
            if proc.poll() is None:
                try:
                    proc.kill()
                except ProcessLookupError:
                    # The guest may exit between poll and kill after a clean run.
                    pass
                proc.wait()
            qmp_path.unlink(missing_ok=True)


def main() -> int:
    if len(sys.argv) < 5:
        raise SystemExit("usage: m95_pin_qemu.py <output-dir> <host-cpu-set> <vcpus> <qemu> [args]")
    out = Path(sys.argv[1])
    cpus = cpu_list(sys.argv[2])
    vcpus = int(sys.argv[3])
    return run(out, cpus, vcpus, sys.argv[4:])


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        sys.exit(2)
