# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc
#
# What THIS PART decides, of the six properties one kernel image across cores requires. The
# other three are the LX6's and live in arch/xtensa/lx6/smp.cmake.
#
#   inter-core IRQ  four GLOBAL single-bit triggers, DPORT_CPU_INTR_FROM_CPU_0..3_REG at
#                   0x3FF000DC + 4n, bit 0, R/W, reset 0 (TRM v5.8 Register 12.24, p.258),
#                   raised as interrupt matrix sources 24..27 (Table 8.3-1, p.176). NOT per
#                   target core: one bit is one source and each CPU's matrix bank routes it
#                   independently. The register carries plain R/W where a self-clearing bit in
#                   this manual carries R/W/SC, and no prose anywhere states a hardware clear,
#                   so the receiver clears by writing the register and a set racing that write
#                   is a real lost edge to design against.
#   symmetry        the two LX6 cores are interchangeable for anything a thread does. The die
#                   is asymmetric in two places a thread never reaches: RTC FAST, 8 KB, is
#                   readable and writable by PRO_CPU alone (3.3.2.7, p.70), and the PID
#                   controller is two controllers behind one address range, each CPU reaching
#                   only its own (3.3.5.1, p.74). Requirement 5 holds for SCHEDULING.
#   targeting       each CPU has its own bank of 69 map registers, PRO at DPORT+0x104..0x214 and
#                   APP at DPORT+0x218..0x328, and 8.3.3 (p.179) disables a source for one CPU
#                   by pointing that CPU's map register at any of the six CPU inputs connected
#                   to neither core, 6, 7, 11, 15, 16 and 29. 26 of each CPU's 32 inputs are
#                   matrix-targetable. TRIGGER TYPE AND PRIORITY ARE FIXED PER INPUT NUMBER with
#                   no register to change either (Table 8.3-2, p.178), so choosing a target also
#                   chooses the type the line is taken with.
#
# The launch is not one of the six and is not declared here: this part's APP_CPU comes up on
# six DPORT and RTC_CNTL writes, and its clock gate resets CLOSED.
set(KICKOS_CHIP_SMP_INTERRUPT 1)
set(KICKOS_CHIP_SMP_SYMMETRIC 1)
set(KICKOS_CHIP_SMP_TARGETING 1)
