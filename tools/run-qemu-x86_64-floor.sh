#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Boot a UEFI application on qemu64, a processor below the x86-64-v3 floor, and assert that it
# refused the processor before its first line (arch/x86/x86_64/floor_x86_64.cc).
#
#   tools/run-qemu-x86_64-floor.sh <application.efi> [workdir]
#
# The machine, the EFI system partition and the serial capture come from
# tools/run-qemu-x86_64-common.sh. KICKOS_X86_64_CPU is this script's own.
#
# POSIX sh (dash-clean).

set -u

KOS_TOOLS=$(cd "$(dirname "$0")" && pwd); . "$KOS_TOOLS/run-qemu-x86_64-common.sh"

KOS_STEP=X1
KOS_TOKEN="KICKOS-X1 8c41d7a2 x86_64/q35 uefi-handover"
KOS_FIRMWARE=pflash
KOS_MACHINE=q35
KOS_TIMEOUT=60
KOS_WORK_LEAF=x1floor
KOS_END=halt
KOS_REFUSAL="KickOS: this processor is below x86-64-v3"
KOS_END_RE="$KOS_REFUSAL"
KICKOS_X86_64_CPU=qemu64

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    fail "usage: run-qemu-x86_64-floor.sh <application.efi> [workdir]"
fi

kos_boot "$1" "${2:-}"

need "the image did not refuse a processor below the floor" "$KOS_REFUSAL"
if grep -q "^$TOK entry\$" "$PLAIN"; then
    fail "the image ran its handover on a processor below the floor"
fi

echo "PASS: $KOS_TOKEN refused qemu64 below the x86-64-v3 floor"
echo "      serial: $PLAIN"
