#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Run a telemetry sim app (which flushes the ch1 ring to $KICKOS_TRACE_FILE at
# shutdown), then decode + structurally assert the trace. Used by CI gate 3.

import subprocess
import sys
import tempfile
import os

if len(sys.argv) < 3:
    sys.stderr.write("usage: check_run.py <app> <kicktrace.py>\n")
    sys.exit(2)

app = sys.argv[1]
kicktrace = sys.argv[2]

with tempfile.TemporaryDirectory() as d:
    trace = os.path.join(d, "trace.bin")
    env = dict(os.environ, KICKOS_TRACE_FILE=trace)
    out = subprocess.run([app], env=env, capture_output=True, text=True, timeout=30)
    sys.stdout.write(out.stdout)
    sys.stderr.write(out.stderr)
    if out.returncode != 0:
        print("app exited nonzero: %d" % out.returncode)
        sys.exit(1)
    if not os.path.exists(trace):
        print("no trace file produced")
        sys.exit(1)
    rc = subprocess.run([sys.executable, kicktrace, trace, "--assert-structural"])
    # Also print a human summary for the CI log.
    subprocess.run([sys.executable, kicktrace, trace, "--summary"])
    sys.exit(rc.returncode)
