#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Boot a UEFI application under qemu-system-x86_64 with UEFI firmware and assert what the
# handover produced on COM1.
#
#   tools/run-qemu-x86_64.sh <application.efi> [workdir]
#
# The machine, the EFI system partition and the serial capture come from
# tools/run-qemu-x86_64-common.sh.
#
# Environment:
#   KICKOS_X1_TOKEN     the banner the arm requires (default below). Held against the IMAGE,
#                       never scraped from the source.
#   KICKOS_X1_FIRMWARE  `pflash` (split OVMF_CODE_4M plus OVMF_VARS_4M, the default) or
#                       `bios` (the combined /usr/share/ovmf/OVMF.fd through -bios).
#   KICKOS_X1_MACHINE   qemu machine type, default q35.
#   KICKOS_X1_TIMEOUT   seconds, default 60.
#   KICKOS_X86_64_CPU   a -cpu model, default none. Shared by all five witnesses.
#
# POSIX sh (dash-clean).

set -u

KOS_TOOLS=$(cd "$(dirname "$0")" && pwd); . "$KOS_TOOLS/run-qemu-x86_64-common.sh"

KOS_STEP=X1
KOS_TOKEN="${KICKOS_X1_TOKEN:-KICKOS-X1 8c41d7a2 x86_64/q35 uefi-handover}"
KOS_FIRMWARE="${KICKOS_X1_FIRMWARE:-pflash}"
KOS_MACHINE="${KICKOS_X1_MACHINE:-q35}"
KOS_TIMEOUT="${KICKOS_X1_TIMEOUT:-60}"
KOS_WORK_LEAF=x1run
# The image HALTS rather than exiting, so nothing ends the emulator on its own.
KOS_END=halt

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    fail "usage: run-qemu-x86_64.sh <application.efi> [workdir]"
fi

kos_boot "$1" "${2:-}"

need_ere "firmware never reached the entry" "^$TOK entry\$"
need_ere "the entry did not read the system table" "^$TOK firmware=..*\$"
# The four figures UEFI 2.11 section 2.3.4 mandates at handover. wp and nx are the firmware's
# own choices and stay loose. nxcap is PINNED to 1: it is CPUID 0x80000001 EDX bit 20, which
# the map editor refuses to come up without (arch/x86/x86_64/aspace_x86_64.cc), and left
# loose this arm passes under `-cpu qemu64,nx=off`.
need_ere "the handover state was not measured before the cli" \
     "^$TOK handover if=1 paging=1 longmode=1 root=1 wp=[01] nx=[01] nxcap=1\$"
need_ere "interrupts were not disabled on entry" "^$TOK if_after_cli=0\$"
# arena_pages is asserted NON-ZERO: a map whose conventional runs all fall below the legacy
# floor would publish nothing and let every arm above it still report ok.
need_ere "no memory map was taken" "^$TOK map descriptors=[1-9][0-9]* stride=[1-9][0-9]* version=[1-9][0-9]* conventional_pages=[1-9][0-9]* arena=0x[0-9a-f]* arena_pages=[1-9][0-9]*\$"
# `fp trapped` is pinned: this port saves no x87, MMX, vector or extended state, so every bit
# that lets an instruction reach any of it must read the same way on every machine.
need_ere "the vector-state posture was not read before it was changed" \
     "^$TOK fp found em=[01] ts=[01] mp=[01] osfxsr=[01] osxmmexcpt=[01] osxsave=[01]\$"
need_ere "x87, MMX, vector and extended state were not refused" \
     "^$TOK fp trapped em=1 ts=1 mp=1 osfxsr=0 osxmmexcpt=0 osxsave=0\$"
need_ere "boot services were not left" "^$TOK boot services left\$"
need_ere "interrupts came back across ExitBootServices" "^$TOK if_after_exit=0\$"
need_ere "the entry did not reach its halt" "^$TOK landed, halting\$"

if grep -q "FAIL" "$PLAIN"; then
    fail "the image reported a failure: $(grep 'FAIL' "$PLAIN" | head -1)"
fi

echo "PASS: $KOS_TOKEN booted under $KOS_FIRMWARE firmware on machine $KOS_MACHINE"
echo "      serial: $PLAIN"
