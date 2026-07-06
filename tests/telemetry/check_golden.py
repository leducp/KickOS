#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate 1: golden-vector encode->decode round-trip. Runs the gen_golden host
# program (which drives the PURE C++ encoders and prints the expected canonical
# decode), decodes the emitted bytes with kicktrace.py, and asserts they match
# exactly. A byte-level regression in either the encoders or the decoder fails.

import subprocess
import sys
import tempfile
import os

if len(sys.argv) < 3:
    sys.stderr.write("usage: check_golden.py <gen_golden> <kicktrace.py>\n")
    sys.exit(2)

gen = sys.argv[1]
kicktrace = sys.argv[2]

with tempfile.TemporaryDirectory() as d:
    binpath = os.path.join(d, "golden.bin")
    expected = subprocess.check_output([gen, binpath], text=True)
    actual = subprocess.check_output(
        [sys.executable, kicktrace, binpath, "--csv"], text=True)

exp = expected.strip().splitlines()
act = actual.strip().splitlines()
if exp == act:
    print("GOLDEN OK: %d records round-tripped exactly" % len(exp))
    sys.exit(0)

print("GOLDEN MISMATCH")
n = max(len(exp), len(act))
for i in range(n):
    e = exp[i] if i < len(exp) else "<missing>"
    a = act[i] if i < len(act) else "<missing>"
    mark = "  " if e == a else ">>"
    print("%s expected: %s" % (mark, e))
    print("%s actual  : %s" % (mark, a))
sys.exit(1)
