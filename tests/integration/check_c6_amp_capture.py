#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
"""Verdict for the unattended ESP32-C6 HP/LP AMP capture."""

import re
import sys
from pathlib import Path


def require(ok: bool, why: str) -> None:
    if not ok:
        raise SystemExit(f"FAIL: ESP32-C6 AMP capture: {why}")


def one(pattern: str, log: str) -> tuple[str, ...]:
    rows = re.findall(pattern, log, re.MULTILINE)
    require(len(rows) == 1, f"expected one row matching {pattern!r}, found {len(rows)}")
    row = rows[0]
    return row if isinstance(row, tuple) else (row,)


def attempts_per_round(log: str) -> dict[int, int]:
    # ampping prints a retried round's attempt line BEFORE that round's ping line.
    attempts: dict[int, int] = {}
    pending = 1
    for line in log.splitlines():
        retry = re.fullmatch(r"  \(node 1 answered on attempt ([0-9]+)\)", line)
        if retry:
            require(pending == 1, "two attempt lines with no ping line between them")
            pending = int(retry.group(1))
            require(pending >= 2, f"attempt line names attempt {pending}")
            continue
        ping = re.fullmatch(r"  ping ([0-9]+) -> pong [0-9]+ from node 1 \(4 byte\(s\)\)", line)
        if ping:
            attempts[int(ping.group(1))] = pending
            pending = 1
    require(pending == 1, "an attempt line is not followed by its round's ping line")
    require(sorted(attempts) == [1, 2, 3, 4], f"rounds seen {sorted(attempts)}, expected 1 to 4")
    return attempts


def main() -> None:
    require(len(sys.argv) == 2, "usage: check_c6_amp_capture.py <capture.log>")
    log = Path(sys.argv[1]).read_text().replace("\r", "")
    one(r"^load:0x40800000,len:0x[0-9a-f]+$", log)
    one(r"^load:0x4083c000,len:0x[0-9a-f]+$", log)
    one(r"^# c6amp: LP RTC ([0-9]+) Hz$", log)
    vector = int(one(r"^# ampdiag: peer node=1 vectors=0x([0-9a-f]+) tag=0 clk=20000000 self clk=160000000$", log)[0], 16)
    require(0x4083c000 <= (vector & ~3) < 0x40878000 and (vector & 3) == 1,
            f"LP mtvec {vector:#x} is outside its HP-SRAM image or not vectored")
    one(r"^ampping: 2 of 2 node app\(s\) alive on the port the partition names, own row 1$", log)
    own, held, foreign = (int(v, 16) for v in one(
        r"^ampping: gate: node 0 read 0x([0-9a-f]+) from its timg0, "
        r"node 1 read 0x([0-9a-f]+) from its timg1 and 0x([0-9a-f]+) from node 0's timg0$", log))
    require(own != 0, "node 0 read zero from the timg0 it holds")
    require(held != 0, "the LP read zero from the timg1 it holds")
    require(foreign == 0, f"the LP read {foreign:#x} from node 0's timg0, which the APM must deny it")
    one(r"^ampping: node 0 calls node 1 port 3$", log)
    for n in range(1, 5):
        one(rf"^  ping {n} -> pong {n + 1} from node 1 \(4 byte\(s\)\)$", log)
    attempts = attempts_per_round(log)
    print("rounds: " + ", ".join(f"{n} on attempt {attempts[n]}" for n in sorted(attempts)))
    retried = [str(n) for n in sorted(attempts) if attempts[n] > 1]
    if retried:
        print("retried rounds: " + ", ".join(retried))
    else:
        print("retried rounds: none")
    served = int(one(r"^ampping: node 1 answered 4 round\(s\), its own record says ([0-9]+)$", log)[0])
    require(served >= 4, f"LP served record has {served}, needs four")
    one(r"^ampping: node 0 done, 4 round\(s\) across the partition$", log)
    one(r"^ampping: the doorbell has no seat, so no raise of it can be deferred \(rc -38\)$", log)
    for node in range(2):
        bells, drains = map(int, one(rf"^# ampdiag: node={node} core={node} bells=([0-9]+) drains=([0-9]+)$", log))
        require(bells > 0 and drains > 0, f"node {node} did not service its doorbell")
    require("Kernel panic" not in log and "FAULT" not in log, "panic or fault in capture")
    print(f"PASS: ESP32-C6 HP/LP AMP, four far replies, LP served record {served}, "
          f"LP timg1 {held:#x} and node 0's timg0 denied")


if __name__ == "__main__":
    main()
