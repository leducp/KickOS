#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate 4: QEMU structural run. Boots a self-terminating telemetry image on a
# QEMU Cortex-M4 (mps2-an386, semihosting trace clock); at exit the mps2 backend
# writes the ch1 ring to kicktrace.bin via semihosting. This is the ONLY automated
# coverage of the PendSV-tail switch-hook ASM. Decode + structurally assert.
#
# If QEMU is absent, exit 77 (CTest SKIP) rather than a false pass.

import subprocess
import sys
import os
import tempfile
import shutil

if len(sys.argv) < 3:
    sys.stderr.write("usage: check_qemu.py <image.elf> <kicktrace.py>\n")
    sys.exit(2)

image = os.path.abspath(sys.argv[1])
kicktrace = os.path.abspath(sys.argv[2])
qemu = os.environ.get("QEMU", "qemu-system-arm")
machine = os.environ.get("QEMU_MACHINE", "mps2-an386")

if shutil.which(qemu) is None:
    print("SKIP: %s not found" % qemu)
    sys.exit(77)

with tempfile.TemporaryDirectory() as d:
    # QEMU writes kicktrace.bin relative to its CWD via semihosting SYS_OPEN.
    out = subprocess.run(
        [qemu, "-M", machine, "-nographic", "-semihosting", "-kernel", image],
        cwd=d, capture_output=True, text=True, timeout=30)
    sys.stdout.write(out.stdout)
    sys.stderr.write(out.stderr)
    trace = os.path.join(d, "kicktrace.bin")
    if not os.path.exists(trace):
        print("FAIL: no kicktrace.bin produced by the QEMU run")
        sys.exit(1)
    print("captured %d bytes of telemetry from QEMU" % os.path.getsize(trace))
    rc = subprocess.run([sys.executable, kicktrace, trace, "--assert-structural"])
    subprocess.run([sys.executable, kicktrace, trace, "--summary"])
    sys.exit(rc.returncode)
