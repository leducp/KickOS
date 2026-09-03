# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# What THIS PART decides, of the six properties one kernel image across cores requires. The
# other three are ARMv8-A's and live in arch/arm64/armv8a/smp.cmake.
#
#   inter-core IRQ  the die wires a GIC-500, so software-generated interrupts INTIDs 0..15
#                   raised through ICC_SGI1R_EL1. Not a machine option as it is on an
#                   emulated `virt`: this part has one controller and no version to choose.
#   symmetry        THE PROPERTY IS THE CLUSTER'S AND NOT THE DIE'S. The four Cortex-A53s are
#                   identical and interchangeable, so one kernel image schedules a thread onto
#                   any of them. The Cortex-M7 companion beside them has its own NVIC and its
#                   own instruction set, so it is asymmetric BY CONSTRUCTION and no image spans
#                   it; it is an AMP node, and what this declaration asserts is the cluster.
#   targeting       GICD_IROUTER points a shared line at one core, so a line is pinned
#                   rather than masked on the cores that must not take it.
#
# The launch is not one of the six and is not declared here: this part's release has no
# vehicle on the machine, and chip_imx8mp.cc refuses a count above one.
set(KICKOS_CHIP_SMP_INTERRUPT 1)
set(KICKOS_CHIP_SMP_SYMMETRIC 1)
set(KICKOS_CHIP_SMP_TARGETING 1)
