#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# An exception nothing catches: the terminate handler names the exception's type and what(),
# then stops the image.
#
#   check_qemu_cxxterm.sh <cxxterm.elf>

set -u
. "$(dirname "$0")/../lib/gate.sh"
: "${QEMU_TIMEOUT:=15}"

elf="${1:?usage: check_qemu_cxxterm.sh <cxxterm.elf>}"

need_qemu_machine
run_image "$elf"

assert_no_panic "the image faulted on its uncaught exception"
has "^cxxterm: throwing" || fail "the image never reached its throw"
if has "^cxxterm: returned"; then
    fail "an uncaught exception returned to its thrower"
fi
printf '%s\n' "$OUT" | grep -qxF "terminate: uncaught exception [St13runtime_error]: cxxterm uncaught" \
    || fail "terminate did not report the exception's type and what()"
if [ "$RC" -eq 124 ]; then
    fail "the image did not stop after terminate (timed out)"
fi
echo "PASS: terminate reported the uncaught exception and stopped the image"
