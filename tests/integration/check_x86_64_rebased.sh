#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The self-test linked at a base firmware cannot honour (docs/design-m10-kernel-share.md
# section 1.2). It counts as the relocated case only if the boot REPORTS that firmware moved
# it, which moves the kernel's view of the program it embeds; the stream then gets the same
# verdict as the image at its preferred base.

set -u
here="$(dirname "$0")"
. "$here/../lib/gate.sh"
: "${QEMU_TIMEOUT:=180}"

img="${1:?usage: check_x86_64_rebased.sh <selftest_rebased.efi> <expected-arms>}"
want_arms="${2:?usage: check_x86_64_rebased.sh <selftest_rebased.efi> <expected-arms>}"

need_qemu_machine
run_image "$img"
[ "$RC" -ne 124 ] || fail "the ${QEMU_TIMEOUT}s hang backstop fired; the capture is cut off"

line="$(printf '%s\n' "$OUT" | grep -a '^x86_64 image: ' | head -n 1)"
[ -n "$line" ] || fail "the boot printed no image load line, so which load case ran is unknown"
delta="$(printf '%s\n' "$line" | sed -n 's/.* load_delta=\(0x[0-9a-fA-F]*\).*/\1/p')"
case "$delta" in
    0x) fail "unparsable load line: $line" ;;
    '') fail "unparsable load line: $line" ;;
esac
if [ "$(printf '%s' "$delta" | tr -d '0x')" = "" ]; then
    fail "firmware loaded the rebased image at its preferred base (load_delta=$delta), so this
  run is the preferred case again and witnesses nothing"
fi

printf '%s\n' "$OUT" | "$here/check_tap_stream.sh" "qemu/$QEMU_MACHINE rebased" "$want_arms"
