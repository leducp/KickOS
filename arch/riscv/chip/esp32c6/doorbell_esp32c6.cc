// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The PMU carries only a wake; the request and answer travel in shared HP-SRAM cells, since
// LP SRAM cannot hold their atomics.

#include <kickos/arch/arch.h>
#include <kickos/chip_limits.h>
#include <kickos/arch/amp_shared.h>
#include <kickos/arch/doorbell_protocol.h>
#include <kickos/io/mmio.h>
#include <kickos/sys/atomic.h>

#if KICKOS_AMP_OWN_IMAGE

#include <stdint.h>

extern "C" void kickos_isr_timer(void);

namespace
{
    constexpr uintptr_t PMU_COMM = 0x600B0184u;
    constexpr uintptr_t HP_INT_CLR = 0x600B0168u;
    constexpr uintptr_t LP_INT_CLR = 0x600B0178u;
    constexpr uint32_t LP_TO_HP = 1u << 30;
    constexpr uint32_t HP_TO_LP = 1u << 31;

    struct Part
    {
        // Device I/O: the clear must land before the cells are read, or a raise between the two
        // is lost.
        static void clear(void)
        {
#if KICKOS_AMP_NODE_ID == 0
            r32(HP_INT_CLR) = 1u << 29;
#else
            r32(LP_INT_CLR) = 1u << 31;
#endif
            __asm volatile("fence iorw, iorw" ::: "memory");
        }
        static void raise_self(void)
        {
            kickos_doorbell_raise(1u << arch_doorbell_core());
        }
        static bool inject_owed(void)
        {
            return false;
        }
    };
}

// No barrier: no translation to invalidate.
kickos::doorbell::Fenced kickos::doorbell::service_fence(Observed)
{
    return Fenced();
}

extern "C"
{
// Set by HP before wake, then read by LP. Both images give it the same address.
KICKOS_AMP_SHARED("chip") kickos::Atomic<uint32_t, kickos::Order::RELAXED> kickos_c6_amp_rtc_hz = 0u;

void kickos_doorbell_poll(void)
{
    kickos::doorbell::poll<Part>();
}

void kickos_doorbell_raise(uint32_t cores)
{
    // The publication must be visible before the device store that announces it.
    __asm volatile("fence iorw, iorw" ::: "memory");
    if ((cores & 1u) != 0u)
    {
        r32(PMU_COMM) = LP_TO_HP;
    }
    if ((cores & 2u) != 0u)
    {
        r32(PMU_COMM) = HP_TO_LP;
    }
}

void arch_ipi_fence(void)
{
    __asm volatile("fence rw, rw" ::: "memory");
}

// HP dispatches the PMU source through its interrupt matrix. LP dispatches
// PMU_LP_INT through its sole interrupt 30. Both reach the same service body.
void kickos_c6_amp_doorbell_service(void)
{
#if KICKOS_RV32_LP
    if ((r32(0x600B0170u) & (1u << 31)) != 0u)
    {
        Part::clear();
        kickos_doorbell_service();
    }
    if ((r32(0x600B0C3Cu) & (1u << 31)) != 0u)
    {
        r32(0x600B0C44u) = 1u << 31;
        kickos_isr_timer();
    }
#else
    Part::clear();
    kickos_doorbell_service();
#endif
}
}
#endif
