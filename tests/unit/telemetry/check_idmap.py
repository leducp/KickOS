#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate 3: trace-metadata de-drift, three-way. user/include/kickos/sys/abi.h is the
# AUTHORITY for the syscall set: every KOS_SYS_* enumerator it defines must appear, at
# the same number, in BOTH the gen_idmap C++ list and tools/kicktrace.py's SYSCALL_NAME
# (and neither side may carry a number abi.h no longer defines). Comparing only the two
# mirrors against each other was vacuous: both stopped at 35 while abi.h reached 37, and
# the gate stayed green. kicktrace's label must also be the enumerator suffix lowercased,
# which is what makes the strings checkable at all.
#
# ARCH_NAME stays a two-way check: its authority is kickos::trace::ArchId (a C++ enum in
# include/kickos/trace/record.h), reachable only through gen_idmap.

import re
import subprocess
import sys
import importlib.util

sys.dont_write_bytecode = True  # don't drop a __pycache__ beside tools/kicktrace.py

if len(sys.argv) < 4:
    sys.stderr.write("usage: check_idmap.py <gen_idmap> <kicktrace.py> <abi.h>\n")
    sys.exit(2)

gen = sys.argv[1]
kicktrace_path = sys.argv[2]
abi_path = sys.argv[3]

problems = []

# The authority: enum kos_syscall_nr in abi.h. Explicit `= N` values with trailing
# `// ...` comments; the last enumerator carries no comma.
abi_syscall = {}
abi_text = open(abi_path, encoding="ascii").read()
enum_match = re.search(r"enum\s+kos_syscall_nr\s*\{(.*?)\n\}", abi_text, re.S)
if enum_match is None:
    problems.append("%s: no `enum kos_syscall_nr { ... }` body found" % abi_path)
else:
    body = re.sub(r"//[^\n]*", "", enum_match.group(1))
    for token in body.split(","):
        token = " ".join(token.split())
        if not token:
            continue
        m = re.fullmatch(r"(KOS_SYS_[A-Z0-9_]+)\s*=\s*(\d+)", token)
        if m is None:
            problems.append("abi.h: enumerator %r is not `KOS_SYS_NAME = <decimal>`"
                            " (the parse needs an explicit number)" % token)
            continue
        abi_syscall[m.group(1)] = int(m.group(2))
if not abi_syscall and enum_match is not None:
    problems.append("abi.h: parsed 0 syscall enumerators")

# Numbers as the C++ side actually spells them (gen_idmap names each enumerator, so a
# renamed/removed one fails to compile there rather than drifting silently).
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

if not c_arch:
    problems.append("gen_idmap emitted no arch ids")
if not c_syscall:
    problems.append("gen_idmap emitted no syscall numbers")

for name in sorted(abi_syscall, key=lambda k: abi_syscall[k]):
    nr = abi_syscall[name]
    if nr not in c_syscall:
        problems.append("%s = %d in abi.h is MISSING from the gen_idmap.cc list"
                        " (tests/unit/telemetry/gen_idmap.cc)" % (name, nr))
    if nr not in py_syscall:
        problems.append("%s = %d in abi.h is MISSING from kicktrace.py SYSCALL_NAME"
                        % (name, nr))
        continue
    want = name[len("KOS_SYS_"):].lower()
    got = kt.SYSCALL_NAME[nr]
    if got != want:
        problems.append("SYSCALL_NAME[%d] is %r, expected %r (from %s)"
                        % (nr, got, want, name))

abi_numbers = set(abi_syscall.values())
for nr in sorted(c_syscall - abi_numbers):
    problems.append("syscall %d is STALE in the gen_idmap.cc list (no abi.h enumerator)"
                    % nr)
for nr in sorted(py_syscall - abi_numbers):
    problems.append("SYSCALL_NAME[%d] = %r is STALE (no abi.h enumerator)"
                    % (nr, kt.SYSCALL_NAME[nr]))

missing_arch = sorted(c_arch - py_arch)
extra_arch = sorted(py_arch - c_arch)
if missing_arch:
    problems.append("ARCH_NAME: kicktrace MISSING %s" % missing_arch)
if extra_arch:
    problems.append("ARCH_NAME: kicktrace has EXTRA (stale) %s" % extra_arch)

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

print("IDMAP OK: %d syscalls from abi.h in gen_idmap.cc and kicktrace.py;"
      " %d arch ids match" % (len(abi_syscall), len(c_arch)))
sys.exit(0)
