/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * The dense core index, as an ASSEMBLY sequence. The die owns it because PRID's values are the
 * die's: the ISA loads the register from pins and says only that a processor's value is
 * TYPICALLY 0..NPROCESSORS-1. This die's two differ in bit 1, so the index is a two-instruction
 * extract.
 *
 * IT IS NOT TOTAL: a third value extracts to 0 or 1. arch_cpu_id (chip_esp32.cc) is the total
 * reader and runs on each core before that core's interrupts open. THAT IS A PRECONDITION ON
 * BRING-UP ORDER: a core that takes an interrupt before its init has run breaks this.
 */

#ifndef KICKOS_CHIP_CPUID_H
#define KICKOS_CHIP_CPUID_H

/* The bit of PRID that reads 0 on core 0 and 1 on core 1. */
#define KICKOS_CHIP_CPUID_PRID_BIT 1

#if defined(__ASSEMBLER__)

/* reg <- this core's dense index, 0 or 1. Clobbers reg only. */
    .macro  CHIP_CPU_INDEX reg
    rsr.prid \reg
    extui   \reg, \reg, KICKOS_CHIP_CPUID_PRID_BIT, 1
    .endm

#endif

#endif /* KICKOS_CHIP_CPUID_H */
