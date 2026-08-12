#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# QEMU TAP gate: boot the `selftest` image (semihosting console) and hand the stream to
# tests/integration/check_tap_stream.sh, which owns the verdict. selftest self-terminates, so
# QEMU_TIMEOUT is only a hang backstop.

set -u
here="$(dirname "$0")"
. "$here/../lib/gate.sh"
: "${QEMU_TIMEOUT:=30}"

elf="${1:?usage: check_qemu_selftest.sh <selftest.elf> <expected-arms>}"
want_arms="${2:?usage: check_qemu_selftest.sh <selftest.elf> <expected-arms>}"

need_qemu_machine
run_image "$elf"
printf '%s\n' "$OUT" | "$here/check_tap_stream.sh" "qemu/$QEMU_MACHINE" "$want_arms"
