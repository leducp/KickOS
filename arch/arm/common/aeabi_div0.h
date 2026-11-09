// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_ARM_COMMON_AEABI_DIV0_H
#define KICKOS_ARCH_ARM_COMMON_AEABI_DIV0_H

// The immediate of the UDF that __aeabi_idiv0 and __aeabi_ldiv0 end in (aeabi_div0.cc): the one
// mark a fault report on a core with no fault status has that the fault was a divide by zero.
#define KICKOS_ARM_UDF_DIV0 0xD0

#ifdef __cplusplus

#include <stdint.h>

extern "C"
{
    extern unsigned char __kickos_rom_start[];
    extern unsigned char __kickos_rom_end[];
}

// A fault handler reads the stacked PC's halfword only inside the chip's flash: a read of a wild
// PC from HardFault faults again and locks the core up, and flash answers a read with no side
// effect.
static inline bool kickos_arm_div0_trap_at(uint32_t pc)
{
    uintptr_t const lo = reinterpret_cast<uintptr_t>(__kickos_rom_start);
    uintptr_t const hi = reinterpret_cast<uintptr_t>(__kickos_rom_end);
    if ((pc & 1u) != 0u or pc < lo or pc >= hi or hi - pc < 2u)
    {
        return false;
    }
    uint16_t const insn = *reinterpret_cast<uint16_t const volatile*>(pc);
    return insn == (0xDE00u | KICKOS_ARM_UDF_DIV0);
}

#endif

#endif
