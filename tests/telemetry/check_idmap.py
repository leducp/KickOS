#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate 3: trace-metadata de-drift. Runs the gen_idmap host program (which prints
# the AUTHORITATIVE arch/syscall numbers straight from the C++ enums) and asserts
# tools/kicktrace.py's ARCH_NAME and SYSCALL_NAME dicts key EXACTLY those numbers --
# no missing entry (a decoder that lags abi.h, e.g. stops at syscall 20 while the
# ABI reaches 35) and no extra entry (a stale number the C side no longer defines).
# The number is the wire/ABI contract; the string labels are a human mirror not
# compared here (the C headers carry no strings). See the TODO in kicktrace.py.

import subprocess
import sys
import importlib.util

sys.dont_write_bytecode = True  # don't drop a __pycache__ beside tools/kicktrace.py

if len(sys.argv) < 3:
    sys.stderr.write("usage: check_idmap.py <gen_idmap> <kicktrace.py>\n")
    sys.exit(2)

gen = sys.argv[1]
kicktrace_path = sys.argv[2]

# Authoritative numbers from the C++ source of truth.
out = subprocess.check_output([gen], text=True)
c_arch = set()
c_syscall = set()
for line in out.splitlines():
    parts = line.split()
    if len(parts) != 2:
        continue
    if parts[0] == "arch":
        c_arch.add(int(parts[1]))
    elif parts[0] == "syscall":
        c_syscall.add(int(parts[1]))

# Load kicktrace.py as a module (its main() is __main__-guarded, so importing it
# has no side effect beyond defining the dicts).
spec = importlib.util.spec_from_file_location("kicktrace", kicktrace_path)
kt = importlib.util.module_from_spec(spec)
spec.loader.exec_module(kt)

py_arch = set(kt.ARCH_NAME.keys())
py_syscall = set(kt.SYSCALL_NAME.keys())

problems = []
if not c_arch:
    problems.append("gen_idmap emitted no arch ids")
if not c_syscall:
    problems.append("gen_idmap emitted no syscall numbers")


def diff(label, c_set, py_set):
    missing = sorted(c_set - py_set)   # in C source of truth, absent from kicktrace
    extra = sorted(py_set - c_set)     # in kicktrace, no longer in the C source
    if missing:
        problems.append("%s: kicktrace MISSING %s" % (label, missing))
    if extra:
        problems.append("%s: kicktrace has EXTRA (stale) %s" % (label, extra))


diff("ARCH_NAME", c_arch, py_arch)
diff("SYSCALL_NAME", c_syscall, py_syscall)

# Every mapped label must be a non-empty string (a bare {} or None would decode
# nothing useful).
for nr, name in kt.SYSCALL_NAME.items():
    if not isinstance(name, str) or not name:
        problems.append("SYSCALL_NAME[%r] is not a non-empty string" % nr)
for nr, name in kt.ARCH_NAME.items():
    if not isinstance(name, str) or not name:
        problems.append("ARCH_NAME[%r] is not a non-empty string" % nr)

if problems:
    print("IDMAP DRIFT:")
    for p in problems:
        print("  - " + p)
    sys.exit(1)

print("IDMAP OK: %d arch ids + %d syscall numbers match kicktrace.py exactly"
      % (len(c_arch), len(c_syscall)))
sys.exit(0)
