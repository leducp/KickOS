#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
"""Verdict for the unattended ESP32-C6 HP/LP AMP capture. A refusal is `FAIL: <token>: <prose>`,
the token a planted row of tests/static/check_app_judges.sh names.

    check_c6_amp_capture.py <capture.log> <node 0 base> <node 1 base> <node 1 end>

The three addresses are the build's (user/apps/common/ampping/CMakeLists.txt).
"""

import re
import sys
from pathlib import Path

PANIC = Path(__file__).resolve().parent.parent / "lib" / "panic.ere"
GATE_WORD = 0x600DF00D


def require(ok: bool, token: str, why: str) -> None:
    if not ok:
        raise SystemExit(f"FAIL: {token}: ESP32-C6 AMP capture: {why}")


def one(token: str, pattern: str, log: str) -> tuple[str, ...]:
    rows = re.findall(pattern, log, re.MULTILINE)
    require(len(rows) == 1, token, f"expected one row matching {pattern!r}, found {len(rows)}")
    row = rows[0]
    return row if isinstance(row, tuple) else (row,)


def attempts_per_round(log: str) -> dict[int, int]:
    # ampping prints a retried round's attempt line BEFORE that round's ping line.
    attempts: dict[int, int] = {}
    pending = 1
    for line in log.splitlines():
        retry = re.fullmatch(r"  \(node 1 answered on attempt ([0-9]+)\)", line)
        if retry:
            require(pending == 1, "attempt", "two attempt lines with no ping line between them")
            pending = int(retry.group(1))
            require(pending >= 2, "attempt", f"attempt line names attempt {pending}")
            continue
        ping = re.fullmatch(r"  ping ([0-9]+) -> pong [0-9]+ from node 1 \(4 byte\(s\)\)", line)
        if ping:
            attempts[int(ping.group(1))] = pending
            pending = 1
    require(pending == 1, "attempt", "an attempt line is not followed by its round's ping line")
    require(sorted(attempts) == [1, 2, 3, 4], "round", f"rounds seen {sorted(attempts)}, expected 1 to 4")
    return attempts


def main() -> None:
    require(len(sys.argv) == 5, "usage",
            "usage: check_c6_amp_capture.py <capture.log> <node 0 base> <node 1 base> <node 1 end>")
    log = Path(sys.argv[1]).read_text().replace("\r", "")
    node0, node1, node1_end = (int(v, 16) for v in sys.argv[2:5])
    # Node 0's slice of HP SRAM, below the LP slice (ampping/systems/esp32c6-wroom/amp2/gate.c
    # holds the word).
    node0_own = (node0, node1)
    panic = PANIC.read_text().strip()
    require(panic != "", "usage", f"{PANIC} is empty, so no panic would be refused")
    banner = re.compile(panic)
    one("load", rf"^load:{node0:#010x},len:0x[0-9a-f]+$", log)
    one("load", rf"^load:{node1:#010x},len:0x[0-9a-f]+$", log)
    one("lp-rtc", r"^# c6amp: LP RTC ([0-9]+) Hz$", log)
    vector, self_clk = one("vectors", r"^# ampdiag: peer node=1 vectors=0x([0-9a-f]+) tag=0 clk=20000000 "
                           r"self clk=([0-9]+)$", log)
    vector = int(vector, 16)
    require(int(self_clk) == 160000000, "vectors",
            f"HP clock {self_clk} Hz is not the 160 MHz the boot's PLL bring-up selects")
    require(node1 <= (vector & ~3) < node1_end and (vector & 3) == 1, "vectors",
            f"LP mtvec {vector:#x} is outside its HP-SRAM image or not vectored")
    one("alive", r"^ampping: 2 of 2 node app\(s\) alive on the port the partition names, own row 1$", log)
    own, held, foreign = (int(v, 16) for v in one(
        "gate", r"^ampping: gate: node 0 read 0x([0-9a-f]+) from its timg0, "
        r"node 1 read 0x([0-9a-f]+) from its timg1 and 0x([0-9a-f]+) from node 0's timg0$", log))
    require(own != 0, "gate", "node 0 read zero from the timg0 it holds")
    require(held != 0, "gate", "the LP read zero from the timg1 it holds")
    require(foreign == 0, "gate", f"the LP read {foreign:#x} from node 0's timg0, which the APM must deny it")
    stored, hit, word, loaded, kept = (int(v, 16) for v in one(
        "memory", r"^ampping: gate: node 1 stored 0x([0-9a-f]+) to 0x([0-9a-f]+), node 0's word at 0x([0-9a-f]+), "
        r"and loaded 0x([0-9a-f]+); node 0 reads 0x([0-9a-f]+) there$", log))
    require(hit == word, "memory", f"the LP stored to {hit:#x}, not node 0's word at {word:#x}")
    require(node0_own[0] <= word < node0_own[1], "memory",
            f"node 0's word at {word:#x} is outside node 0's own HP SRAM [{node0_own[0]:#x}, {node0_own[1]:#x})")
    # An APM denial reads 0 and drops the write (ESP32-C6 TRM v1.2 section 16.5).
    require(loaded == 0, "memory", f"the LP loaded {loaded:#x} from node 0's word, which the APM must deny it")
    require(kept == GATE_WORD and kept != stored, "memory",
            f"node 0's word reads {kept:#x} after the LP's store of {stored:#x}, not the {GATE_WORD:#x} it holds")
    one("call", r"^ampping: node 0 calls node 1 port 3$", log)
    for n in range(1, 5):
        one("round", rf"^  ping {n} -> pong {n + 1} from node 1 \(4 byte\(s\)\)$", log)
    attempts = attempts_per_round(log)
    print("rounds: " + ", ".join(f"{n} on attempt {attempts[n]}" for n in sorted(attempts)))
    retried = [str(n) for n in sorted(attempts) if attempts[n] > 1]
    if retried:
        print("retried rounds: " + ", ".join(retried))
    else:
        print("retried rounds: none")
    served = int(one("served", r"^ampping: node 1 answered 4 round\(s\), its own record says ([0-9]+)$", log)[0])
    require(served >= 4, "served", f"LP served record has {served}, needs four")
    one("done", r"^ampping: node 0 done, 4 round\(s\) across the partition$", log)
    panics = [line for line in log.splitlines() if banner.search(line)]
    require(not panics, "panic", f"a panic or fault banner in the capture: {panics[0] if panics else ''}")
    require("=== THREAD FAULT ===" not in log, "fault", "a thread fault in the capture, where none is expected")
    print(f"PASS: ESP32-C6 HP/LP AMP, four far replies, LP served record {served}, "
          f"LP timg1 {held:#x}, node 0's timg0 and node 0's word at {word:#x} denied")


if __name__ == "__main__":
    main()
