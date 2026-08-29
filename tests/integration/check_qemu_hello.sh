#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU smoke gate for the armv7m target: boot the `hello` image on a QEMU
# Cortex-M4 (mps2-an386) via semihosting, let the two userspace threads
# ping-pong for a few seconds, then assert the exchange happened. This exercises
# the whole M1 arch layer end to end on real ARM: reset -> scheduler start
# (PendSV) -> SVC-trampoline syscalls -> SysTick timer -> semaphore reschedule.

set -u
. "$(dirname "$0")/../lib/gate.sh"

elf="${1:?usage: check_qemu_hello.sh <hello.elf>}"

# The demo ping-pongs forever. Round 3 of BOTH threads, so one thread looping alone is not
# enough.
poll_image "$elf" "KickOS" "ping 3" "pong 3"

if [ "$POLL_OK" -ne 1 ]; then
    fail "expected banner + ping/pong rounds not observed"
fi
assert_no_panic "hello panicked while ping-ponging"

echo "PASS: QEMU hello ping-ponged"
exit 0
