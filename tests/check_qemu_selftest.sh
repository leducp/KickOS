#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU TAP gate: boot the `selftest` image on QEMU (semihosting console), let the
# TAP suite run to completion, and assert a clean run -- "# all tests passed" with
# no "not ok". This is the SAME binary/suite that runs on the sim, now exercised on
# real ISA mechanism (armv7m/armv6m PendSV, rv32imac msip). selftest self-terminates
# (arch_shutdown when done), so QEMU_TIMEOUT is only a hang backstop.
#
# SKIP BUDGET (MAX_SKIPS, default 0). A test whose board cannot host it reports a real
# TAP skip -- `ok N - name # SKIP reason` -- and the harness prints `# skipped: N`. A
# skip is NOT a pass, so it must not be free: the default budget of 0 makes EVERY
# registered test required, and a board that legitimately cannot run some of them
# declares that number at its call site (microbit's 2-thread pool). The harness cannot
# make this judgement -- only the board can -- and a plain count is what the gate can
# check, so raising a budget is a deliberate, reviewable act rather than a silent
# green. This is precisely the hole that let the mutex / domain_share suites skip
# themselves on microbit for milestones while CI stayed green.

set -u
elf="${1:?usage: check_qemu_selftest.sh <selftest.elf>}"
qemu="${QEMU:-qemu-system-arm}"
machine="${QEMU_MACHINE:-mps2-an386}"
extra_arg="${QEMU_EXTRA:-}"             # e.g. -bios none (RISC-V virt)
max_skips="${MAX_SKIPS:-0}"

if ! command -v "$qemu" >/dev/null 2>&1; then
    # Exit 77 -> CTest SKIP (not PASS), so a QEMU-less box doesn't green-light it.
    echo "SKIP: $qemu not found"
    exit 77
fi

# Strip CR: a board with KICKOS_CONSOLE_CRLF cooks '\n' on the wire, which would
# otherwise defeat the end-anchored parse of the summary line below.
out="$(timeout "${QEMU_TIMEOUT:-30}" "$qemu" -M "$machine" $extra_arg -nographic -semihosting -kernel "$elf" 2>&1 | tr -d '\r')"
echo "$out"

if echo "$out" | grep -q "not ok"; then
    echo "FAIL: a TAP test reported not ok"
    exit 1
fi
if ! echo "$out" | grep -q "# all tests passed"; then
    echo "FAIL: TAP completion marker missing (crash / hang / truncated run?)"
    exit 1
fi

# Parsed AFTER the completion marker, so a truncated run is reported as truncated
# rather than as a missing skip count. run_all() emits this line unconditionally
# (zero included), so absence here means the harness itself regressed.
skipped="$(echo "$out" | sed -n 's/^# skipped: \([0-9][0-9]*\)$/\1/p' | tail -1)"
if [ -z "$skipped" ]; then
    echo "FAIL: no '# skipped: N' summary in the TAP stream (harness regression?)"
    exit 1
fi
if [ "$skipped" -gt "$max_skips" ]; then
    echo "FAIL: $skipped test(s) skipped, budget is $max_skips (MAX_SKIPS). Skipped:"
    echo "$out" | grep "# SKIP"
    exit 1
fi

echo "PASS: selftest TAP suite clean ($skipped skipped, budget $max_skips)"
exit 0
