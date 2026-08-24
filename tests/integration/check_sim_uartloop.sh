#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# CI gate for the BUFFERED USERSPACE UART: build the sim with the loopback UART service
# list (kickos_services_simuart) and require a client's bytes to come back through the
# real two-thread driver.
#
# This runs the ACTUAL two-thread driver, where the selftest's uart_service case covers the
# wire ABI and the rings by driving kickos::uart::serve_one in a single thread:
#   - the TX DOORBELL crosses threads. Nothing raises this line: the sim has no
#     hardware source for it, so the only thing that can move a byte is the service
#     thread's kos_irq_notify waking the IRQ thread out of kos_irq_wait. If the doorbell
#     is broken, no byte is ever transmitted and the read returns nothing.
#   - both rings stay SPSC with one writer per index in a DIFFERENT thread.
#   - content, not just counts: the loopback must return every byte in order, so a mask or
#     wrap bug surfaces as a mismatch rather than a plausible length.
#
# Three things stay xmc4800's, per design section 9.2: a hardware TX-empty interrupt driving
# the drain, asynchronous RX from a real line, and the transition-triggered half of RULE T1
# (a host write cannot fail to raise).
#
# usage: check_sim_uartloop.sh <kickos-source-dir> <cmake>

set -eu

KICKOS_SRC="$1"
CMAKE="${2:-cmake}"

fail() { echo "FAIL: $1"; exit 1; }
has() { printf '%s\n' "$OUT" | grep -q "$1"; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "== configuring the sim with the loopback UART service list =="
( cd "$KICKOS_SRC" && "$CMAKE" --preset sim -B "$TMP/build" \
    -DKICKOS_SERVICE_LIST=kickos_services_simuart >/dev/null ) \
  || fail "configure with kickos_services_simuart failed"

echo "== building uartloop =="
"$CMAKE" --build "$TMP/build" --target uartloop >/dev/null || fail "uartloop build failed"

APP="$TMP/build/user/apps/common/uartloop/uartloop"
[ -x "$APP" ] || fail "uartloop binary not produced at $APP"

set +e
OUT="$(timeout "${SIM_TIMEOUT:-30}" "$APP" 2>&1)"
RC=$?
set -e
printf '%s\n' "$OUT"

# First: without the service the app reports a missing endpoint, which names the cause of
# the missing payload below.
has '\[simuart\] UART service up' \
  || fail "the UART service never came up (bring-up failed, or the wrong service list linked)"
if has '\[uartloop\] ERROR'; then
    fail "the app could not reach the service: see its ERROR line above"
fi

# The IRQ thread write(2)s each byte it drains, so the payload on the wire is the transmit
# half seen directly rather than through a counter.
has 'KickOS UART loopback' \
  || fail "the payload never reached the wire: the IRQ thread drained nothing, so the TX doorbell did not wake it"

# SUSTAINED OUTPUT: the arm that fails when a producer stops ringing the doorbell once the
# TX ring is full. The short payload above never fills the ring, so it passes against a
# driver whose channel dies permanently on the first refused write. Asserted on the reported
# byte count and not on a timeout, so a wedged channel FAILS the gate rather than hanging it.
printf '%s\n' "$OUT" | grep -q '\[uartloop\] sustained=4096 of 4096' \
  || fail "sustained output stopped short: the channel wedged with a full ring and never restarted; see the sustained= line above"

has '\[uartloop\] PASS (loopback in order; sustained output past a full ring)' \
  || fail "the loopback did not return the payload intact: see the wrote/read/match line above"

# irq_wakes counts every irq_wait return, so it is the doorbell's own footprint. A PASS above
# is impossible at zero today, and this keeps it so if a change satisfies the read another
# way.
printf '%s\n' "$OUT" | grep -q 'wakes=0' \
  && fail "the IRQ thread never woke: the bytes moved without the doorbell, so this gate stopped testing it"

[ "$RC" -eq 0 ] || fail "expected a clean exit 0, got $RC"

echo "PASS: the two-thread UART driver moves bytes, doorbell-paced, over the loopback"
