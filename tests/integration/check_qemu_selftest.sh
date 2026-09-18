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
# Allow for slow hosts. Keep the CTest timeout above this limit so the
# script can report a QEMU timeout before CTest terminates it.
: "${QEMU_TIMEOUT:=180}"

elf="${1:?usage: check_qemu_selftest.sh <selftest.elf> <expected-arms>}"
want_arms="${2:?usage: check_qemu_selftest.sh <selftest.elf> <expected-arms>}"

need_qemu_machine
started="$(date +%s)"
run_image "$elf"
elapsed="$(($(date +%s) - started))"
# Report timeout status 124 before validating the incomplete TAP output.
if [ "$RC" -eq 124 ]; then
    fail "the ${QEMU_TIMEOUT}s hang backstop fired after ${elapsed}s and killed the image, so the
  capture above is a CUT-OFF run and not a finished one. selftest self-terminates, so this is a
  hang, or a host far slower than a backstop set at several multiples of a legitimate run. It
  is never an arm's verdict."
fi
printf '%s\n' "$OUT" | "$here/check_tap_stream.sh" "qemu/$QEMU_MACHINE" "$want_arms"
