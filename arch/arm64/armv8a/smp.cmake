# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# armv8a meets every property a part running ONE kernel image across cores requires, and by
# what mechanism:
#
#   coherency       an A53 cluster is one inner-shareable domain, and every Normal descriptor
#                   this port programs carries SH=0b11 (the chip's boot tables and
#                   aspace_armv8a.cc), so shared kernel state needs no software maintenance.
#   exclusion       LDXR/STXR on the same inner-shareable domain, which is architectural on
#                   ARMv8-A and needs no hardware lock block.
#   inter-core IRQ  GIC software-generated interrupts, INTIDs 0..15, targeted per core.
#   identity        MPIDR_EL1.Aff0, which arch_cpu_id reads (arch_armv8a.cc).
#   symmetry        one cluster of identical A53s, so a thread runs the same on either core.
#   targeting       the GIC distributor routes a device line per core (GICD_ITARGETSR on a
#                   GICv2, GICD_IROUTER on a GICv3), so a line is pinned rather than masked.
set(KICKOS_ARCH_SMP_CAPABLE 1)
