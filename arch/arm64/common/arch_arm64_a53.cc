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

// E (bit 0), C (bit 2) and LC (bit 6) in ONE immediate, which is what the static gate reads
// out of the linked image.
#define KICKOS_A53_PMCR_E_C_LC 0x45u

namespace
{
    constexpr uint64_t CNTP_CTL_ENABLE = 1u << 0;

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
    __asm volatile("msr cntp_ctl_el0, %0" ::"r"(uint64_t(0)));
    // The disable governs the timer's output only past a context synchronisation event. Without
    // this, an output still asserted when the GIC below enables this core's PPI pends the line
    // the write above exists to silence.
    __asm volatile("isb" ::: "memory");

#if defined(KICKOS_BENCH) && KICKOS_BENCH
    // The bench cycle source (kickos/bench.h) is PMCCNTR_EL0, and nothing else in this port
    // programs the PMU. PMCR_EL0.D divides the count by 64 and its reset value is
    // architecturally UNKNOWN, so it is cleared rather than assumed.
    //
    // LC IS PART OF THE WIDTH CLAIM. bench_cyccnt subtracts PMCCNTR_EL0 at 64 bits
    // (KICKOS_BENCH_TICK_BITS), and with LC clear the cycle counter's overflow sits at bit 31;
    // Arm deprecates that setting. tests/static/check_bench_a53_pmcr.sh holds the bit, because
    // no vehicle in this tree can tell the two images apart at run time.
    // Per core, these registers being per PE.
    uint64_t pmcr = 0;
    __asm volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
    pmcr &= ~static_cast<uint64_t>(1u << 3);
    pmcr |= KICKOS_A53_PMCR_E_C_LC;
    __asm volatile("msr pmcr_el0, %0" ::"r"(pmcr));
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
    __asm volatile("msr cntp_ctl_el0, %0" ::"r"(uint64_t(0)));
    // Between the two: the disable reaches the timer's output only past a context
    // synchronisation event, and the Device write below is not ordered against a system-register
    // write by anything else, so a level still asserted re-pends the line behind the clear.
    __asm volatile("isb" ::: "memory");
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
