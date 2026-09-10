#!/bin/sh
# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# Boot the seam image under qemu-system-x86_64 with UEFI firmware and assert every arm the
# console, the clock, the one-shot timer, the software interrupt controller, the context
# switch and idle report.
#
#   tools/run-qemu-x86_64-x3.sh <application.efi> [workdir]
#
# The machine, the EFI system partition and the serial capture come from
# tools/run-qemu-x86_64-common.sh.
#
# Environment:
#   KICKOS_X3_TOKEN     the token every line must carry (default below). Held against the IMAGE,
#                       never scraped from the source.
#   KICKOS_X3_FIRMWARE  `pflash` (split OVMF_CODE_4M plus OVMF_VARS_4M, the default) or
#                       `bios` (the combined /usr/share/ovmf/OVMF.fd through -bios).
#   KICKOS_X3_MACHINE   qemu machine type, default q35.
#   KICKOS_X3_TIMEOUT   seconds, default 120.
#   KICKOS_X86_64_CPU   a -cpu model, default none. Shared by all five witnesses.
#
# POSIX sh (dash-clean).

set -u

KOS_TOOLS=$(cd "$(dirname "$0")" && pwd); . "$KOS_TOOLS/run-qemu-x86_64-common.sh"

KOS_STEP=X3
KOS_TOKEN="${KICKOS_X3_TOKEN:-KICKOS-X3 e73b1f04 x86_64/q35 seam}"
KOS_FIRMWARE="${KICKOS_X3_FIRMWARE:-pflash}"
KOS_MACHINE="${KICKOS_X3_MACHINE:-q35}"
KOS_TIMEOUT="${KICKOS_X3_TIMEOUT:-120}"
KOS_WORK_LEAF=x3run
# The image ends itself: arch_shutdown writes isa-debug-exit, so the emulator's exit code
# carries the status, (status << 1) | 1, and 1 is a pass while 3 is the image's own FAIL.
KOS_END=exit

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    fail "usage: run-qemu-x86_64-x3.sh <application.efi> [workdir]"
fi

kos_boot "$1" "${2:-}"

need "the image never reached its arms" "^$TOK arms\$"
need "no apic report line" \
     "^  $TOK apic mode=\(x2apic\|xapic\) timer_hz=[1-9][0-9]* tsc_hz=[1-9][0-9]* ref_hz=[1-9][0-9]* ram_size=[1-9][0-9]*\$"

listed=0
for a in calibrated \
         clock_monotonic clock_advances \
         timer_fired timer_not_early timer_in_tolerance timer_disarmed \
         irq_delivered irq_line_identity irq_in_isr irq_mask_silences irq_latch_one_deep \
         irq_two_lines_one_region \
         switch_voluntary \
         switch_preempt_both_ran switch_preempt_alternates switch_preempt_deferred \
         switch_preempt_double_request \
         idle_wakes_masked idle_wakes_open \
         flush_sync_returns
do
    arm_ok "$a"
    listed=$((listed + 1))
done

need "no voluntary switch order line" "^  $TOK switch voluntary order=MVVXM\$"
need "no preemptive switch order line" \
     "^  $TOK switch preempt order=\(AB\)\(AB\)\(AB\)*A* deferred=[0-9][0-9]*\$"

# THE SET AND NOT ONLY ITS MEMBERS. Every name above is asserted to have reported, which says
# nothing about an arm the image reported and this list does not name. A FAILING unlisted arm is
# still caught, by the image's own FAIL line and by its exit status; what is lost without this
# count is the NAME, so the arm runs unread here and a reader of this script cannot tell it
# exists.
onwire="$(grep -c "^  $TOK arm=" "$PLAIN")" || onwire=0
if [ "$listed" -ne "$onwire" ]; then
    fail "the image reported $onwire arm(s) and this script names $listed. An arm the list
  does not name is one nothing here reads, so add it above rather than leaving the two counts
  to differ"
fi

if grep -q "$TOK FAIL" "$PLAIN"; then
    fail "the image reported its own failure"
fi
need "the image did not reach its PASS" "^$TOK PASS\$"

kos_require_clean_exit

echo "PASS: $KOS_TOKEN, all $listed arm(s) reported ok"
echo "      serial: $PLAIN"
