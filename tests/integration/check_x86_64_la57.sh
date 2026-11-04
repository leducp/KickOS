#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The self-test under five-level paging (docs/design-m10-kernel-share.md section 1.2). It counts
# only if the boot REPORTS that the kernel adopted a five-level root, where the program's base lies
# in the top-level slot firmware's identity map takes; the stream then gets the same verdict as
# under four levels, image_fixed_base among its arms.

set -u
here="$(dirname "$0")"
. "$here/../lib/gate.sh"
: "${QEMU_TIMEOUT:=180}"

img="${1:?usage: check_x86_64_la57.sh <selftest_la57.efi> <expected-arms>}"
want_arms="${2:?usage: check_x86_64_la57.sh <selftest_la57.efi> <expected-arms>}"

need_qemu_machine
run_image "$img"
[ "$RC" -ne 124 ] || fail "the ${QEMU_TIMEOUT}s hang backstop fired; the capture is cut off"

printf '%s\n' "$OUT" | grep -aqE '^x86_64 la57 witness: (five-level paging entered|firmware left five-level paging live)$' \
    || fail "the boot reported no five-level root, so this run is the four-level case again:
  $(printf '%s\n' "$OUT" | grep -a 'la57 witness' | head -n 1)"
printf '%s\n' "$OUT" | grep -aqE '^ok [0-9]+ - image_fixed_base$' \
    || fail "image_fixed_base did not pass under five levels"

printf '%s\n' "$OUT" | "$here/check_tap_stream.sh" "qemu/$QEMU_MACHINE la57" "$want_arms"
