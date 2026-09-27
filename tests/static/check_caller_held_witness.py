#!/usr/bin/env python3
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
"""Prove the caller-held graph gate refuses a lost lock and an unknown edge."""

from pathlib import Path
import sys
from unittest.mock import patch

import check_caller_held as gate


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: check_caller_held_witness.py <qemu-x86_64-ci-build>")
    build = sys.argv[1]
    root = Path(__file__).resolve().parents[2]
    syscall = root / "kernel/syscall/syscall.cc"
    ignored = Path(__file__).resolve().with_name("caller_held_indirect.txt")
    original = Path.read_text
    lock_site = "IrqLock lock;\n                sched::set_prio(pub, KICKOS_PRIO_MIN);"
    unknown_site = "ignore x86_64 * efi_main@1/2 reason:"

    def check_mutation(path, change, expected):
        def planted(self, *args, **kwargs):
            text = original(self, *args, **kwargs)
            if self == path:
                return change(text)
            return text

        gate.source.cache_clear()
        gate.functions.cache_clear()
        with patch.object(Path, "read_text", planted):
            try:
                gate.check(build, "x86_64", "qemu-x86_64", 1)
            except ValueError as exc:
                if expected not in str(exc):
                    sys.exit(f"FAIL: mutation refused for the wrong reason: {exc}")
                return
        sys.exit(f"FAIL: mutation did not turn the caller-held gate red: {expected}")

    def remove_lock(text):
        if text.count(lock_site) != 1:
            sys.exit("FAIL: set_prio lock witness moved")
        return text.replace(lock_site,
                            " " * len("IrqLock lock;")
                            + "\n                sched::set_prio(pub, KICKOS_PRIO_MIN);")

    def remove_binding(text):
        lines = text.splitlines(keepends=True)
        chosen = [line for line in lines if line.startswith(unknown_site)]
        if len(chosen) != 1:
            sys.exit("FAIL: indirect-edge witness moved")
        return text.replace(chosen[0], "")

    check_mutation(syscall, remove_lock, "unguarded entry root")
    check_mutation(ignored, remove_binding, "unknown indirect edge")
    print("caller_held: removed caller lock and unknown indirect edge both refused")


if __name__ == "__main__":
    main()
