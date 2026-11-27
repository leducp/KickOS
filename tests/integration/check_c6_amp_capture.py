#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
"""Verdict for the unattended ESP32-C6 HP/LP AMP capture. A refusal is `FAIL: <token>: <prose>`,
the token a planted row of tests/static/check_app_judges.sh names.

    check_c6_amp_capture.py <capture.log> <node 0 base> <node 1 base>

The two addresses are the build's (user/apps/common/ampping/CMakeLists.txt).
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


def main() -> None:
    require(len(sys.argv) == 4, "usage",
            "usage: check_c6_amp_capture.py <capture.log> <node 0 base> <node 1 base>")
    log = Path(sys.argv[1]).read_text().replace("\r", "")
    node0, node1 = (int(v, 16) for v in sys.argv[2:4])
    # Node 0's slice of HP SRAM, below the LP slice (ampping/systems/esp32c6-wroom/amp2/gate.c
    # holds the word).
    node0_own = (node0, node1)
    panic = PANIC.read_text().strip()
    require(panic != "", "usage", f"{PANIC} is empty, so no panic would be refused")
    banner = re.compile(panic)
    one("load", rf"^load:{node0:#010x},len:0x[0-9a-f]+$", log)
    one("load", rf"^load:{node1:#010x},len:0x[0-9a-f]+$", log)
    one("lp-rtc", r"^# c6amp: LP RTC ([0-9]+) Hz$", log)
    hp_clk, lp_clk = (int(v) for v in one(
        "clock", r"^ampping: gate: node 0 runs at ([0-9]+) Hz, node 1 at ([0-9]+) Hz$", log))
    require(hp_clk == 160000000, "clock",
            f"HP clock {hp_clk} Hz is not the 160 MHz the boot's PLL bring-up selects")
    require(lp_clk == 20000000, "clock", f"LP clock {lp_clk} Hz is not the LP core's 20 MHz")
    one("relay", r"^ampping: node 1 serves port 3$", log)
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
    served = int(one("served", r"^ampping: node 1 answered 4 round\(s\), its own record says ([0-9]+)$", log)[0])
    require(served >= 4, "served", f"LP served record has {served}, needs four")
    one("done", r"^ampping: node 0 done, 4 round\(s\) across the partition$", log)
    panics = [line for line in log.splitlines() if banner.search(line)]
    require(not panics, "panic", f"a panic or fault banner in the capture: {panics[0] if panics else ''}")
    require("=== THREAD FAULT ===" not in log, "fault", "a thread fault in the capture, where none is expected")
    print(f"PASS: ESP32-C6 HP/LP AMP, four far replies, LP served record {served}, "
          f"LP timg1 {held:#x}, node 0's timg0 and node 0's word at {word:#x} denied, "
          f"the LP's announcement whole through the HP relay")


if __name__ == "__main__":
    main()
