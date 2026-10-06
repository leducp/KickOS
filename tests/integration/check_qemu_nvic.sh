#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The NVIC line count the QEMU machine a board runs on models, against the KICKOS_MAX_IRQ its
# chip file states, which the vector table and the kernel's line table are sized by.
#
#   check_qemu_nvic.sh <image> <generated chip_limits.h>
#
# The image is only what kickos_add_qemu_test passes; the machine is QEMU_MACHINE.

set -u
. "$(dirname "$0")/../lib/gate.sh"

USAGE="usage: check_qemu_nvic.sh <image> <chip_limits.h>"
[ "$#" -eq 2 ] || fail "$USAGE"
LIMITS="$2"
need_qemu_machine
need_qemu
[ -f "$LIMITS" ] || fail "no chip limits at $LIMITS"
WANT="$(sed -n 's/^#define KICKOS_MAX_IRQ \([0-9]*\).*/\1/p' "$LIMITS")"
[ -n "$WANT" ] || fail "$LIMITS defines no KICKOS_MAX_IRQ"
# shellcheck disable=SC2086
TREE="$(echo "info qtree" | timeout "${QEMU_TIMEOUT:-20}" "$QEMU_BIN" -M "$QEMU_MACHINE" ${QEMU_EXTRA:-} -S \
        -nographic -monitor stdio -serial null 2>&1)"
GOT="$(printf '%s\n' "$TREE" | grep -A3 'dev: armv7m_nvic' | sed -n 's/^ *num-irq = \([0-9]*\).*/\1/p' | head -n 1)"
[ -n "$GOT" ] || fail "QEMU's $QEMU_MACHINE reported no armv7m_nvic num-irq"
[ "$GOT" = "$WANT" ] || fail "QEMU's $QEMU_MACHINE models $GOT NVIC lines, and the chip file states $WANT"
echo "PASS: QEMU's $QEMU_MACHINE models the $WANT NVIC lines the chip file states"
