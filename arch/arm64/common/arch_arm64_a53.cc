// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The Cortex-A53 seams that reach nothing but system registers: the architected generic timer
// and the semihosting dead end. No device address is nameable here, which is what lets one
// file answer for every arm64 chip.

#include "a53.h"

#include "gic.h" // kickos_armv8a_gic_percore_init, kickos_armv8a_gic_clear_pending

#include <kickos/arch/arch.h>
#include <kickos/arch/clk_q32.h> // KICKOS_NS_PER_SEC (canonical 1e9 ns/sec)

#include <stdint.h>

using kickos::arm64::ADP_Stopped_ApplicationExit;
using kickos::arm64::halt_masked;
using kickos::arm64::PPI_EL1_PHYS_TIMER;
using kickos::arm64::semihost;
using kickos::arm64::SYS_EXIT;

namespace
{
    constexpr uint64_t CNTP_CTL_ENABLE = 1u << 0;
    // PMCR_EL0: E enables the counters, C resets the cycle counter, D divides it by 64, LC puts
    // its overflow at bit 63.
    constexpr uint64_t PMCR_E = 1u << 0;
    constexpr uint64_t PMCR_C = 1u << 2;
    constexpr uint64_t PMCR_D = 1u << 3;
    constexpr uint64_t PMCR_LC = 1u << 6;

    // Seated before any reader: kickos_armv8a_timebase_init runs at the top of every chip's
    // arch_init, ahead of the first arch_clock_now the kernel can reach.
    uint64_t g_ns_per_tick = 0;

    uint64_t counter_now(void)
    {
        uint64_t t = 0;
        // The ISB is the architected ordered read: without it the counter may be sampled out
        // of order with the surrounding instructions, and every deadline derives from it.
        __asm volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
        return t;
    }

    // The disable governs the timer's output only past a context synchronisation event, and
    // nothing else orders a system-register write against a later Device write, so the ISB is
    // part of the write: a GIC access after this never sees an output still asserted.
    void timer_disable(void)
    {
        __asm volatile("msr cntp_ctl_el0, %0\n\tisb" ::"r"(uint64_t(0)) : "memory");
    }
}

extern "C"
{

uint64_t kickos_armv8a_timebase_init(void)
{
    uint64_t freq = 0;
    __asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    if (freq == 0 or (kickos::KICKOS_NS_PER_SEC % freq) != 0)
    {
        return 0;
    }
    g_ns_per_tick = kickos::KICKOS_NS_PER_SEC / freq;
    return freq;
}

void kickos_armv8a_percore_init(void)
{
    // CNTP_CTL_EL0's reset value is architecturally UNKNOWN, so an already-asserted timer
    // would fire the moment this core's PPI and DAIF open.
    timer_disable();

#if defined(KICKOS_BENCH) && KICKOS_BENCH
    // The bench cycle source (kickos/bench.h) is PMCCNTR_EL0, and nothing else in this port
    // programs the PMU. PMCR_EL0.D divides the count by 64 and its reset value is
    // architecturally UNKNOWN, so it is cleared rather than assumed. Per core, these registers
    // being per PE. One statement: check_image_rules.sh reads C out of the immediate the
    // image ORs in.
    uint64_t pmcr = 0;
    uint64_t set = 0;
    __asm volatile("mrs %0, pmcr_el0\n\tand %0, %0, %2\n\tmov %1, %3\n\torr %0, %0, %1\n\t"
                   "msr pmcr_el0, %0"
                   : "=&r"(pmcr), "=&r"(set)
                   : "i"(~PMCR_D), "i"(PMCR_E | PMCR_C | PMCR_LC));
    __asm volatile("msr pmcntenset_el0, %0" ::"r"(static_cast<uint64_t>(1ull << 31)));
    __asm volatile("isb" ::: "memory");
#endif

    kickos_armv8a_gic_percore_init();
}

// A pure read, as the seam requires: the counter is 64-bit and monotonic in hardware, so there
// is no wrap to extend and no anchor to keep, and a nanosecond count needs 584 years to
// overflow 64 bits whatever the tick.
uint64_t arch_clock_now(void)
{
    return counter_now() * g_ns_per_tick;
}

// CNTP_CVAL_EL0 is an absolute compare, so the write is idempotent and no armed-deadline dedup
// is owed. The division is what keeps a UINT64_MAX deadline from overflowing, where a multiply
// would not.
void arch_timer_arm(uint64_t deadline_ns)
{
    uint64_t const ticks = deadline_ns / g_ns_per_tick;
    __asm volatile("msr cntp_cval_el0, %0" ::"r"(ticks));
    __asm volatile("msr cntp_ctl_el0, %0" ::"r"(CNTP_CTL_ENABLE));
    // A deadline already past leaves the compare met, which asserts the timer's output now.
}

// Disarm has to mean no callback fires. Clearing ENABLE deasserts a level-driven output, so the
// GIC's pending state follows it down; ICPENDR covers a pend latched while masked.
void arch_timer_disarm(void)
{
    timer_disable();
    kickos_armv8a_gic_clear_pending(PPI_EL1_PHYS_TIMER);
}

// The exit status is what lets the harness tell a fault from a hang: a spin here makes every
// gate that should FAIL time out instead. AArch64 SYS_EXIT takes a POINTER to a two-field block
// where AArch32 passes the reason in the register. Where nothing listens, the masked halt is
// where this ends.
void arch_shutdown(int status)
{
    uint64_t block[2];
    block[0] = ADP_Stopped_ApplicationExit;
    block[1] = static_cast<uint64_t>(static_cast<unsigned>(status));
    semihost(SYS_EXIT, block);
    halt_masked();
}

}
