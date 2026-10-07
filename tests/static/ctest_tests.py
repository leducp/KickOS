#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Flattens `ctest --show-only=json-v1` into one TAB-separated line per test, for
# tools/sweep_image_gates.sh:
#
#   <name> <TAB> <,label,label,> <TAB> <0|1 DISABLED> <TAB> <program>
#
# and, with --images <file>, for tests/integration/check_image_listing.sh, one line for each
# absolute path in the command of a test that is neither disabled nor labelled host:
#
#   <file name less --suffix> | <name>
#
# or `@none|<name>` for such a test with no command.
#
# With --host-boots <file>, for tests/static/check_host_gate_boots.sh, the name of each test
# labelled host that is not disabled and whose command or ENVIRONMENT sets QEMU_MACHINE or names
# a qemu-system program, one per line.
#
#   ctest_tests.py --json <file> --out <file> [--images <file> [--suffix <s>]]
#                  [--host-boots <file>]
#
# NO FIELD IS EVER EMPTY. The caller reads these lines with IFS set to a tab, and a tab is
# IFS whitespace, so `read` collapses two adjacent tabs into one separator and shifts every
# later field left. Hence the comma fences on the label set (`,` when unlabelled) and @none.
#
# <program> is argv0 with the two wrappers this tree registers through peeled off, so the
# caller compares against the script or binary that actually runs:
#   - `cmake -E env [VAR=value]... prog args`  -> prog
#   - `cmake --build <dir>`                    -> @build
# It is @none when ctest emitted no command at all, which it does when argv0 does not
# resolve to a program on disk: an unbuilt tree, where every directly-registered image
# binary goes commandless. The caller refuses that rather than reading it as a class.

import argparse
import json
import os
import re
import sys

BOOT_RE = re.compile(r"(^|/)qemu-system-|^QEMU_MACHINE=")
ENV_ASSIGN_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*=")


def program(command):
    if not command:
        return "@none"
    a = 0
    if os.path.splitext(os.path.basename(command[0]))[0] == "cmake" and len(command) > 1:
        if command[1] == "--build":
            return "@build"
        if command[1] == "-E" and len(command) > 2 and command[2] == "env":
            a = 3
            while a < len(command) and ENV_ASSIGN_RE.match(command[a]):
                a += 1
    if a < len(command):
        return command[a]
    return "@none"


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--json", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--images")
    p.add_argument("--suffix", default="")
    p.add_argument("--host-boots")
    args = p.parse_args()

    with open(args.json, encoding="utf-8") as f:
        tests = json.load(f).get("tests", [])
    if not tests:
        sys.exit(f"ctest_tests.py: {args.json} declares zero tests")

    table, images, host_boots = [], [], []
    for test in tests:
        name = test["name"]
        labels, disabled, boots = ",", False, False
        for prop in test.get("properties", []):
            if prop["name"] == "LABELS":
                labels += "".join(f"{label}," for label in prop["value"])
            elif prop["name"] == "ENVIRONMENT":
                boots = boots or any(BOOT_RE.search(e) for e in prop["value"])
            elif prop["name"] == "DISABLED":
                disabled = disabled or bool(prop["value"])
        command = test.get("command", [])
        boots = boots or any(BOOT_RE.search(a) for a in command)
        host = ",host," in labels

        table.append(f"{name}\t{labels}\t{int(disabled)}\t{program(command)}\n")
        if boots and not disabled and host:
            host_boots.append(f"{name}\n")
        if args.images and not disabled and not host:
            if not command:
                images.append(f"@none|{name}\n")
            for a in command:
                if a.startswith("/"):
                    file = a.rsplit("/", 1)[-1]
                    if args.suffix and len(file) > len(args.suffix) and file.endswith(args.suffix):
                        file = file[: -len(args.suffix)]
                    images.append(f"{file}|{name}\n")

    with open(args.out, "w", encoding="utf-8") as f:
        f.writelines(table)
    if args.images:
        with open(args.images, "w", encoding="utf-8") as f:
            f.writelines(images)
    if args.host_boots:
        with open(args.host_boots, "w", encoding="utf-8") as f:
            f.writelines(host_boots)


main()
