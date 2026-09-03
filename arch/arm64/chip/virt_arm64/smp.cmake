# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# What THIS PART decides, of the six properties one kernel image across cores requires. The
# other three are ARMv8-A's and live in arch/arm64/armv8a/smp.cmake.
#
#   inter-core IRQ  GIC software-generated interrupts, INTIDs 0..15, raised per core. The
#                   machine wires a GICv2 by default and a GICv3 under gic-version=3, which
#                   is a machine option here rather than a property of a die.
#   symmetry        one cluster of identical Cortex-A53s, so a thread runs the same on any of
#                   them and there is no second core type on the machine.
#   targeting       the distributor routes a shared line at one core, GICD_ITARGETSR on a
#                   GICv2 and GICD_IROUTER on a GICv3, so a line is pinned rather than masked
#                   on the cores that must not take it.
#
# The launch is not one of the six and is not declared here: this machine holds its
# secondaries in PSCI and chip_virt_arm64.cc releases them with CPU_ON over HVC.
set(KICKOS_CHIP_SMP_INTERRUPT 1)
set(KICKOS_CHIP_SMP_SYMMETRIC 1)
set(KICKOS_CHIP_SMP_TARGETING 1)
