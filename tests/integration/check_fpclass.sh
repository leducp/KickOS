#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Require fpclass's own verdict on double classification and printf, and every arm's line by
# name. Under QEMU it boots the image; on silicon, where rxv3 has no emulator, it reads the
# console log a bench capture fetched:
#
#   check_fpclass.sh <fpclass.elf>
#   check_fpclass.sh --log <capture.log>
#
# The arm list is the app's (user/apps/common/fpclass/main.cc). A PASS is printed by whatever
# arms ran, so an arm deleted there leaves the verdict green; naming each one here is what
# keeps the coverage the claim makes.
set -u
. "$(dirname "$0")/../lib/gate.sh"

if [ "${1:-}" = "--log" ]; then
    log="${2:?usage: check_fpclass.sh <fpclass.elf> | --log <capture.log>}"
    [ -r "$log" ] || fail "no capture at $log"
    OUT="$(tr -d '\r' < "$log")"
    printf '%s\n' "$OUT" | grep '^\[fpclass\]'
    has_e '\[fpclass\] (PASS|FAIL)' || fail "the capture carries no fpclass verdict"
else
    elf="${1:?usage: check_fpclass.sh <fpclass.elf> | --log <capture.log>}"
    poll_image "$elf" "\[fpclass\] (PASS|FAIL)"
    if [ "$POLL_OK" -ne 1 ]; then
        fail "fpclass reached no verdict: neither PASS nor FAIL was printed"
    fi
fi

assert_no_panic "fpclass panicked before reaching a verdict"

for _arm in \
    'class qnan' 'class 0/0' 'class +inf' 'class -inf' 'class +0' 'class -0' 'class 1' \
    'class -2.5' 'class max' 'class min' 'class sub' 'class -sub' \
    'order nan,1' 'order 1,nan' 'order nan,nan' 'order 1,2' 'order 2,1' 'order 1,1' \
    'order -inf,inf' 'orderf nan,1' 'orderf 1,nan' 'orderf 1,2' 'orderf 2,1' 'orderf 1,1' \
    'fmt f1' 'fmt f-2.5' 'fmt g0.1' 'fmt e' 'fmt 17g' 'fmt gbig' 'fmt gmax' 'fmt gsub' \
    'fmt fnan' 'fmt finf' 'fmt e-inf'
do
    _line=$(printf '%s\n' "$OUT" | grep -F "[fpclass] $_arm " | tail -1)
    case "$_line" in
        "") fail "fpclass printed no '$_arm' line, so that arm did not run and PASS covers less
  than it claims" ;;
        *" ok") ;;
        *) fail "fpclass arm '$_arm' is wrong: $_line" ;;
    esac
done

if has "\[fpclass\] FAIL"; then
    fail "fpclass reported FAIL"
fi

echo "PASS: every double fpclass tried classified, compared and printed right"
exit 0
