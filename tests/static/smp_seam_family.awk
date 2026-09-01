# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The SMP SEAM family, for tests/static/check_smp_sigdiff.sh. This file is handed to awk as a
# SECOND program after tests/static/aspace_seam.awk:
#
#   awk -f tests/static/aspace_seam.awk -f tests/static/smp_seam_family.awk <stripped>
#
# awk runs BEGIN blocks in the order the programs are named, so the assignment below replaces
# the aspace family's PREFIX and nothing else. The extraction RULES stay the aspace extractor's
# byte for byte: a copy with one line changed lets the verdicts drift apart about what a
# signature IS.
#
# MEMBERSHIP, the seven members the seam carries today, all of them in
# arch/include/kickos/arch/arch.h:
#   - KICKOS_NUM_CORES, the core count, and KICKOS_KERNEL_CORES, how many cores ONE KERNEL
#     schedules on. Every other member below is shaped by one of them and folds on it, so both
#     VALUES are part of this signature. The two are separate members because they are separate
#     facts: an AMP image raises the count and keeps one core per kernel.
#   - arch_cpu_id, the per-core identity: a function above one core, a function-like macro
#     folding to 0u at one. Both arms are members, so the fold itself is in the record set.
#   - arch_ipi_send and arch_ipi_wait, the cross-core doorbell. Two calls, so an initiator can
#     poke every core once and wait once.
#   - arch_kernel_lock and arch_kernel_unlock, the cross-core kernel lock, folding on
#     KICKOS_KERNEL_CORES rather than on the count.
#   - kickos_switch_unlock, kickos_kernel_core_ready, kickos_kernel_core_start and
#     kickos_kernel_core_resched, the four the KERNEL supplies and a shared kernel's backend
#     calls: the release that ends the lock's span over a context switch, the two halves of a
#     peer core's arrival at the scheduler, and the doorbell's scheduling half. Their arm is the
#     multi-core one alone, where every member above folds: a kernel that schedules one core has
#     nothing on the far side to call them.
#
# THE DOORBELL IS ONE SEAM WITH TWO SEMANTICS: the shared-kernel IPI a TLB shootdown rendezvous
# rides on, and the AMP inter-node doorbell. Same hardware, same declaration, so a narrowing of
# arch_ipi_send or arch_ipi_wait to SMP-only semantics takes the AMP use with it.
#
# The extractor reads C, so the per-core structures above the seam are out of its reach, and a
# signature is the shape of a call: what a backend DOES behind a member, the acquire loop's
# servicing of a pending doorbell included, is prose in arch.h and holds or fails in the backend.
#
# The group table in check_smp_sigdiff.sh refuses a member admitted here and classified
# nowhere.

BEGIN {
    PREFIX = "^(KICKOS_NUM_CORES" \
             "|KICKOS_KERNEL_CORES" \
             "|arch_cpu_id" \
             "|arch_ipi_" \
             "|arch_kernel_lock" \
             "|arch_kernel_unlock" \
             "|kickos_switch_unlock" \
             "|kickos_kernel_core_)"
}
