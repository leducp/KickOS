# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# The SMP SEAM family, for tests/static/check_smp_sigdiff.sh. This file is handed to awk as a
# SECOND program after tests/static/aspace_seam.awk:
#
#   awk -f tests/static/aspace_seam.awk -f tests/static/smp_seam_family.awk <stripped>
#
# awk runs BEGIN blocks in the order the programs are named, so the assignment below replaces
# the aspace family's PREFIX and nothing else. The extraction RULES stay the aspace
# extractor's, byte for byte, for the reason entry_seam_family.awk gives: a copy with one line
# changed lets the verdicts drift apart about what a signature IS.
#
# MEMBERSHIP, the four members the seam carries today, all of them in
# arch/include/kickos/arch/arch.h:
#   - KICKOS_NUM_CORES, the core count. Every other member below is shaped by it and folds on
#     it, so its VALUE is part of this signature.
#   - arch_cpu_id, the per-core identity: a function above one core, a function-like macro
#     folding to 0u at one. Both arms are members, so the fold itself is in the record set.
#   - arch_ipi_send and arch_ipi_wait, the cross-core doorbell. Two calls, so an initiator can
#     poke every core once and wait once.
#
# THE BASELINE PREDATES EVERY SMP BACKEND: one frozen after the first backend lands measures
# that backend against itself and reports clean whatever it did.
#
# THE CROSS-CORE LOCK HAS NO SEAM MEMBER YET. When it gets one, the member is added to the
# PREFIX below and to the group table in check_smp_sigdiff.sh in the same commit, and the
# baseline moves WITH that commit.
#
# THE DOORBELL IS ONE SEAM WITH TWO SEMANTICS: the shared-kernel IPI a TLB shootdown
# rendezvous rides on, and the AMP inter-node doorbell. Same hardware, same declaration. A
# narrowing of arch_ipi_send or arch_ipi_wait to SMP-only semantics therefore takes the AMP
# use with it.
#
# WHAT THIS FAMILY CANNOT SEE:
#   - Every per-core structure ABOVE the seam. The kernel-side C++ headers are out of reach of
#     an extractor that reads C, and a per-core run queue, a per-core idle thread or a core
#     mask living in one of them changes no record below.
#   - What a backend DOES behind a member. The doorbell's contract, that the far side takes no
#     kernel lock, is prose in arch.h and holds or fails in the backend; a signature is the
#     shape of the call, not its ordering.
#   - A member named outside the prefixes below. The group table in check_smp_sigdiff.sh
#     refuses a member admitted here and classified nowhere.

BEGIN {
    PREFIX = "^(KICKOS_NUM_CORES" \
             "|arch_cpu_id" \
             "|arch_ipi_)"
}
