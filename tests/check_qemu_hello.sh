#!/usr/bin/env bash
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU smoke gate for the armv7m target: boot the `hello` image on a QEMU
# Cortex-M4 (mps2-an386) via semihosting, let the two userspace threads
# ping-pong for a few seconds, then assert the exchange happened. This exercises
# the whole M1 arch layer end to end on real ARM: reset -> scheduler start
# (PendSV) -> SVC-trampoline syscalls -> SysTick timer -> semaphore reschedule.

set -u
elf="${1:?usage: check_qemu_hello.sh <hello.elf>}"
qemu="${QEMU:-qemu-system-arm}"
machine="${QEMU_MACHINE:-mps2-an386}"   # armv7m default; microbit for armv6m
cpu_arg=""
if [ -n "${QEMU_CPU:-}" ]; then
    cpu_arg="-cpu ${QEMU_CPU}"
fi
# Extra machine args (RISC-V virt needs `-bios none` to run our image bare-metal in
# M-mode rather than under OpenSBI); empty for the ARM machines.
extra_arg="${QEMU_EXTRA:-}"

if ! command -v "$qemu" >/dev/null 2>&1; then
    # Exit 77 (not 0): CTest treats this as SKIP, not PASS, so a CI box without
    # QEMU does not silently green-light the gate.
    echo "SKIP: $qemu not found"
    exit 77
fi

# The demo ping-pongs forever ("press Ctrl+C"); poll its output and stop QEMU as
# soon as the banner + a few rounds are seen (scheduling + syscalls + timer all
# live), rather than burning the whole timeout window. QEMU_TIMEOUT bounds the
# no-progress (fail) path.
log="$(mktemp)"
# shellcheck disable=SC2086
"$qemu" -M "$machine" $cpu_arg $extra_arg -nographic -semihosting -kernel "$elf" >"$log" 2>&1 &
qpid=$!
pass=0
for _ in $(seq 1 $(( ${QEMU_TIMEOUT:-8} * 5 ))); do   # poll at 5 Hz
    if grep -q "KickOS" "$log" && grep -q "ping 3" "$log" && grep -q "pong 3" "$log"; then
        pass=1
        break
    fi
    kill -0 "$qpid" 2>/dev/null || break   # QEMU exited on its own
    sleep 0.2
done
{ kill "$qpid"; wait "$qpid"; } 2>/dev/null
cat "$log"; rm -f "$log"

if [ "$pass" = 1 ]; then
    echo "PASS: QEMU armv7m hello ping-ponged"
    exit 0
fi

echo "FAIL: expected banner + ping/pong rounds not observed"
exit 1
