// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The C6 PMU carries only a wake. The common protocol's shared HP-SRAM cells
// carry the request and its answer; LP SRAM cannot hold their atomics.

#include <kickos/arch/arch.h>
#include <kickos/arch/amp_shared.h>
#include <kickos/arch/doorbell_protocol.h>
#include <kickos/sys/atomic.h>

#if KICKOS_AMP_OWN_IMAGE

#include <stdint.h>

extern "C" void kickos_isr_timer(void);

namespace
{
#if defined(KICKOS_ENABLE_SELFTEST)
    using ServedRow = kickos::doorbell::Row<KICKOS_DOORBELL_LINE>;
    KICKOS_AMP_SHARED("cells.served") ServedRow g_served[KICKOS_DOORBELL_CORES] = {};
#endif
    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    constexpr uintptr_t PMU_COMM = 0x600B0184u;
    constexpr uintptr_t HP_INT_CLR = 0x600B0168u;
    constexpr uintptr_t LP_INT_CLR = 0x600B0178u;
    constexpr uint32_t LP_TO_HP = 1u << 30;
    constexpr uint32_t HP_TO_LP = 1u << 31;

    void service()
    {
#if KICKOS_AMP_NODE_ID == 0
        r32(HP_INT_CLR) = 1u << 29;
#else
        r32(LP_INT_CLR) = 1u << 31;
#endif
        // Device I/O: the clear must land before the cells are read, or a raise
        // between the two is lost.
        __asm volatile("fence iorw, iorw" ::: "memory");
        uint32_t const me = arch_doorbell_core();
#if defined(KICKOS_ENABLE_SELFTEST)
        g_served[me].seq[0] = g_served[me].seq[0].load() + 1u;
#endif
        for (uint32_t from = 0; from < KICKOS_DOORBELL_CORES; from++)
        {
            uint32_t const asked = kickos::doorbell::g_request[from].seq[me].load();
            if (asked != kickos::doorbell::g_answer[me].seq[from].load())
            {
                kickos::doorbell::g_answer[me].seq[from] = asked;
            }
        }
        kickos_amp_node_service();
    }
}

extern "C"
{
// Set by HP before wake, then read by LP. Both images give it the same address.
KICKOS_AMP_SHARED("chip") kickos::Atomic<uint32_t, kickos::Order::RELAXED> kickos_c6_amp_rtc_hz = 0u;

void kickos_doorbell_poll(void)
{
    if (not kickos::doorbell::pending())
    {
        return;
    }
    arch_irq_state_t const state = arch_irq_save();
    service();
    arch_irq_restore(state);
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

#if defined(KICKOS_ENABLE_SELFTEST)
uint64_t arch_ipi_counts(uint32_t core)
{
    if (core >= KICKOS_DOORBELL_CORES)
    {
        return 0;
    }
    return static_cast<uint64_t>(g_served[core].seq[0].load());
}

uint32_t arch_ipi_deferred(uint32_t)
{
    return 0;
}

uint32_t arch_ipi_seat_set(uint32_t, uint32_t)
{
    return ARCH_IPI_SEAT_NONE;
}
#endif

// HP dispatches the PMU source through its interrupt matrix. LP dispatches
// PMU_LP_INT through its sole interrupt 30. Both reach the same service body.
void kickos_c6_amp_doorbell_service(void)
{
#if KICKOS_RV32_LP
    if ((r32(0x600B0170u) & (1u << 31)) != 0u)
    {
        service();
    }
    if ((r32(0x600B0C3Cu) & (1u << 31)) != 0u)
    {
        r32(0x600B0C44u) = 1u << 31;
        kickos_isr_timer();
    }
#else
    service();
#endif
}
}
#endif
