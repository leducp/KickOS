/* SPDX-License-Identifier: CECILL-C
 * Copyright (c) 2026 Philippe Leduc
 *
 * Addressing this core's cell of a per-core array, from assembly. ASSEMBLY ONLY: this header
 * defines one macro and declares nothing a C translation unit can use.
 *
 * ONE ARRAY PER CELL, not one per-core block: at one core each array is one element, so every
 * symbol keeps its name and its address and the single-core image is byte-identical.
 */

#ifndef KICKOS_ARCH_LX6_PERCPU_H
#define KICKOS_ARCH_LX6_PERCPU_H

#include <kickos/chip_cpuid.h>

/* reg <- &sym[this core]. `scratch` is clobbered above one core and untouched at one core, and
 * must differ from reg. Every cell so addressed is a 4-byte word, which is what addx4 assumes.
 */
    .macro  PERCPU_CELL reg, sym, scratch
    movi    \reg, \sym
#if KICKOS_NUM_CORES > 1
    CHIP_CPU_INDEX \scratch
    addx4   \reg, \scratch, \reg
#endif
    .endm

#endif /* KICKOS_ARCH_LX6_PERCPU_H */
