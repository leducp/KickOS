// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Lone-TU fallback (arch/CMakeLists.txt states the rule): exactly one symbol, so a
// backend definition keeps this archive member unextracted.
//
// Raise the physical doorbell over mip.SSIP (the qemu-virt path). The ESP32-C6 defines
// its own (interrupt matrix FROM_CPU source). The raise happens with MIE=0, so it fires
// at arch_irq_restore, the normal ISR path, not a direct post.

#include <kickos/arch/arch.h>

#include <stdint.h>

namespace
{
    constexpr uint32_t MIP_SSIP = 1u << 1;
}

extern "C" void arch_rv_inject_deliver(int line)
{
    (void)line; // ONE doorbell for all lines; the trap reads g_inject_line
    __asm volatile("csrs mip, %0" ::"r"(MIP_SSIP) : "memory");
}
