#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate 2: ring wrap / full-drop record-atomicity + drop accounting. Runs the
# flood app (overflows the ch1 ring), then asserts:
#   * the flushed file is record-atomic (whole records, contiguous seq),
#   * drops actually occurred (dropped > 0),
#   * accounting reconciles: decoded records + dropped == attempted
#     (attempted/dropped are printed by kickos_trace_report_counters at exit).
#
# Clean-tail invariant (why contiguous seq holds even under overflow): the sim
# host drains the ring ONLY at shutdown, so once the ring fills, EVERY subsequent
# record is dropped whole -- drops form one contiguous tail, never an interior
# gap. The file therefore holds seq 0..K with no internal holes; --assert-atomic
# checks exactly that (whole records + no interior gap). A mid-stream gap here
# would mean a torn/corrupt write, i.e. a real bug.

import subprocess
import sys
import tempfile
import os
import re

if len(sys.argv) < 3:
    sys.stderr.write("usage: check_flood.py <app> <kicktrace.py>\n")
    sys.exit(2)

app = sys.argv[1]
kicktrace = sys.argv[2]

with tempfile.TemporaryDirectory() as d:
    trace = os.path.join(d, "trace.bin")
    env = dict(os.environ, KICKOS_TRACE_FILE=trace)
    out = subprocess.run([app], env=env, capture_output=True, text=True, timeout=60)
    sys.stdout.write(out.stdout)
    if out.returncode != 0:
        print("app exited nonzero: %d" % out.returncode)
        sys.exit(1)

    m = re.search(r"\[ktrace\] attempted=(\d+) dropped=(\d+)", out.stdout)
    if not m:
        print("FAIL: no [ktrace] counters line on the console")
        sys.exit(1)
    attempted = int(m.group(1))
    dropped = int(m.group(2))
    print("counters: attempted=%d dropped=%d" % (attempted, dropped))

    atomic = subprocess.run([sys.executable, kicktrace, trace, "--assert-atomic"],
                            capture_output=True, text=True)
    sys.stdout.write(atomic.stdout)
    if atomic.returncode != 0:
        sys.exit(1)
    dm = re.search(r"decoded=(\d+)", atomic.stdout)
    decoded = int(dm.group(1)) if dm else -1

    problems = []
    if dropped <= 0:
        problems.append("expected drops (ring overflow), but dropped=0")
    if decoded + dropped != attempted:
        problems.append("accounting: decoded(%d) + dropped(%d) = %d != attempted(%d)"
                        % (decoded, dropped, decoded + dropped, attempted))
    if problems:
        print("FLOOD FAIL:")
        for p in problems:
            print("  - " + p)
        sys.exit(1)
    print("FLOOD OK: decoded=%d + dropped=%d == attempted=%d, record-atomic"
          % (decoded, dropped, attempted))
    sys.exit(0)
