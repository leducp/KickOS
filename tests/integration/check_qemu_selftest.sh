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
# SEVERAL MULTIPLES OF A LEGITIMATE RUN, NOT A HAIR ABOVE ONE. Preset `qemu` measured 25.61 s
# on an idle box, so the 30 s this used to hold left 15 percent of headroom and a contended box
# spent it: a backstop that close decides the verdict on how loaded the host is, and reports a
# missing verdict line when it does. The ctest TIMEOUT registered in
# tests/integration/gates/selftest.cmake has to stay above this value, or ctest kills the
# script first and no reason reaches the reader at all.
: "${QEMU_TIMEOUT:=180}"

elf="${1:?usage: check_qemu_selftest.sh <selftest.elf> <expected-arms>}"
want_arms="${2:?usage: check_qemu_selftest.sh <selftest.elf> <expected-arms>}"

need_qemu_machine
started="$(date +%s)"
run_image "$elf"
elapsed="$(($(date +%s) - started))"
# `timeout` reports 124 when it killed the image, and that is said HERE rather than downstream:
# the capture then stops wherever the kill landed, which check_tap_stream.sh can only read as a
# missing verdict line, sending the reader after a crash that never happened.
if [ "$RC" -eq 124 ]; then
    fail "the ${QEMU_TIMEOUT}s hang backstop fired after ${elapsed}s and killed the image, so the
  capture above is a CUT-OFF run and not a finished one. selftest self-terminates, so this is a
  hang, or a host far slower than a backstop set at several multiples of a legitimate run. It
  is never an arm's verdict."
fi
printf '%s\n' "$OUT" | "$here/check_tap_stream.sh" "qemu/$QEMU_MACHINE" "$want_arms"
