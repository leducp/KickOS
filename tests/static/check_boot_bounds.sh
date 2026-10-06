#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The boot ledger of tests/lib/gate.sh, against the ctest TIMEOUT the root CMakeLists hands every
# test as KOS_CTEST_TIMEOUT_S:
#   handed     this gate's own environment carries the TIMEOUT it is registered with;
#   uefi-pe    a boot's bound carries the firmware allowance, and a TIMEOUT it would reach
#              is refused before the boot;
#   kernel     a boot's bound is the image's alone;
#   summed     each boot of one gate is charged on top of the ones before it;
#   unhanded   a script run with no TIMEOUT handed is charged nothing.
#
#   check_boot_bounds.sh <registered TIMEOUT>

set -u
. "$(dirname "$0")/../lib/gate.sh"

[ "$#" -eq 1 ] || fail "usage: check_boot_bounds.sh <registered TIMEOUT>"
rc=0

if [ "${KOS_CTEST_TIMEOUT_S:-}" != "$1" ]; then
    bad "this gate is registered at a TIMEOUT of $1 and was handed '${KOS_CTEST_TIMEOUT_S:-}'"
fi

# Whether `boot_bound` admits every <seconds> in turn under <boot> and <timeout>, in a subshell,
# since a refusal exits. Prints the last bound.
admits() { # <boot> <timeout> <seconds>...
    (
        KICKOS_BOOT="$1"
        KOS_CTEST_TIMEOUT_S="$2"
        shift 2
        for _s in "$@"; do
            boot_bound "$_s"
        done
        echo "$KOS_BOOT_BOUND_S"
    ) 2>/dev/null
}

stop=$((KOS_STOP_TICKS / 5))
uefi_need=$((20 + KOS_UEFI_FIRMWARE_S + stop))

bound="$(admits uefi-pe $((uefi_need + 1)) 20)" \
    || bad "a uefi-pe boot of 20s is refused under a TIMEOUT above its whole bound"
[ "$bound" = "$((20 + KOS_UEFI_FIRMWARE_S))" ] \
    || bad "a uefi-pe boot of 20s is bounded at '${bound}s', not $((20 + KOS_UEFI_FIRMWARE_S))s"
if admits uefi-pe "$uefi_need" 20 >/dev/null; then
    bad "a uefi-pe boot of 20s is admitted under a TIMEOUT its bound reaches"
fi
if admits uefi-pe 40 20 >/dev/null; then
    bad "a uefi-pe boot of 20s is admitted under a TIMEOUT of 40, which the firmware allowance
  alone takes half of"
fi
bound="$(admits kernel $((20 + stop + 1)) 20)" \
    || bad "a kernel boot of 20s is refused under a TIMEOUT above its bound"
[ "$bound" = "20" ] || bad "a kernel boot of 20s is bounded at '${bound}s'"
if admits kernel $((2 * (20 + stop))) 20 20 >/dev/null; then
    bad "two kernel boots of 20s are admitted under a TIMEOUT only one fits"
fi
admits kernel $((2 * (20 + stop) + 1)) 20 20 >/dev/null \
    || bad "two kernel boots of 20s are refused under a TIMEOUT both fit"
admits uefi-pe "" 400 >/dev/null || bad "a boot is refused where no TIMEOUT was handed"

if [ "$rc" -eq 0 ]; then
    echo "PASS: every boot is charged to the ctest TIMEOUT handed to its gate"
fi
exit "$rc"
