#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The self-test linked at a base firmware cannot honour (docs/design-m10-kernel-share.md
# section 1.8). It counts as the relocated case only if the boot REPORTS that firmware moved
# it and that the app window held words to move; the stream then gets the same verdict as the
# image at its preferred base.

set -u
here="$(dirname "$0")"
. "$here/../lib/gate.sh"
: "${QEMU_TIMEOUT:=180}"

img="${1:?usage: check_x86_64_rebased.sh <selftest_rebased.efi> <expected-arms>}"
want_arms="${2:?usage: check_x86_64_rebased.sh <selftest_rebased.efi> <expected-arms>}"

need_qemu_machine
run_image "$img"
[ "$RC" -ne 124 ] || fail "the ${QEMU_TIMEOUT}s hang backstop fired; the capture is cut off"

line="$(printf '%s\n' "$OUT" | grep -a '^x86_64 app relocation: ' | head -n 1)"
[ -n "$line" ] || fail "the boot printed no app relocation line, so which load case ran is unknown"
delta="$(printf '%s\n' "$line" | sed -n 's/.* load_delta=\(0x[0-9a-fA-F]*\).*/\1/p')"
words="$(printf '%s\n' "$line" | sed -n 's/.* app_words=\([0-9]*\).*/\1/p')"
case "$delta" in
    0x) fail "unparsable relocation line: $line" ;;
    '') fail "unparsable relocation line: $line" ;;
esac
[ -n "$words" ] || fail "unparsable relocation line: $line"
if [ "$(printf '%s' "$delta" | tr -d '0x')" = "" ]; then
    fail "firmware loaded the rebased image at its preferred base (load_delta=$delta), so this
  run is the preferred case again and witnesses nothing"
fi
[ "$words" -gt 0 ] || fail "the app window carried no absolute word (app_words=0), so the
  second relocation was not exercised on top of firmware's"

printf '%s\n' "$OUT" | "$here/check_tap_stream.sh" "qemu/$QEMU_MACHINE rebased" "$want_arms"
