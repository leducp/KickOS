// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The Cortex-A53 seams that reach nothing but system registers: the architected generic timer
// and the semihosting dead end.

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
    // CNTKCTL_EL1.EL0PCTEN: EL0 reads CNTPCT_EL0 and CNTFRQ_EL0.
    constexpr uint64_t CNTKCTL_EL0PCTEN = 1u << 0;
    // PMCR_EL0: E enables the counters, C resets the cycle counter, D divides it by 64, LC puts
    // its overflow at bit 63.
    constexpr uint64_t PMCR_E = 1u << 0;
    constexpr uint64_t PMCR_C = 1u << 2;
    constexpr uint64_t PMCR_D = 1u << 3;
    constexpr uint64_t PMCR_LC = 1u << 6;

    // Seated by kickos_armv8a_timebase_init at the top of every chip's arch_init.
    uint64_t g_ns_per_tick = 0;

    uint64_t counter_now(void)
    {
        uint64_t t = 0;
        // The ISB orders the counter read against the surrounding instructions.
        __asm volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
        return t;
    }

    // The disable governs the timer's output only past a context synchronisation event, so the
    // ISB is part of the write: a GIC access after this never sees an output still asserted.
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
    kickos_armv8a_mmu_check();
    kickos_armv8a_uguard_init();

    // CNTP_CTL_EL0's reset value is UNKNOWN: an asserted timer would fire the moment this
    // core's PPI and DAIF open.
    timer_disable();

    // Written whole: CNTKCTL_EL1 resets UNKNOWN, and a stray EL0PTEN would hand EL0 the
    // kernel's own timer.
    __asm volatile("msr cntkctl_el1, %0\n\tisb" ::"r"(CNTKCTL_EL0PCTEN) : "memory");

#if KICKOS_KERNEL_CORES > 1
    // EL0 reads TPIDRRO_EL0 and cannot write it: kos_core_current answers from it what
    // KOS_SYS_CORE_CURRENT answers, kickos_kernel_core(), which is arch_cpu_id() only under the
    // shared model.
    static_assert(KICKOS_MULTICORE_MODEL_SHARED, "the seated id must be the kernel core's slot");
    __asm volatile("msr tpidrro_el0, %0" ::"r"(static_cast<uint64_t>(arch_cpu_id())));
#endif

#if KICKOS_BENCH
    // PMCCNTR_EL0 is the bench cycle source (kickos/bench.h). PMCR_EL0.D divides the count by
    // 64 and resets UNKNOWN, so it is cleared. One statement: check_image_rules.sh reads C out
    // of the immediate the image ORs in.
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

uint64_t arch_clock_now(void)
{
    return counter_now() * g_ns_per_tick;
}

// CNTP_CVAL_EL0 is an absolute compare, so the write is idempotent. The division keeps a
// UINT64_MAX deadline from overflowing where a multiply would.
void arch_timer_arm(uint64_t deadline_ns)
{
    uint64_t const ticks = deadline_ns / g_ns_per_tick;
    __asm volatile("msr cntp_cval_el0, %0" ::"r"(ticks));
    __asm volatile("msr cntp_ctl_el0, %0" ::"r"(CNTP_CTL_ENABLE));
    // A deadline already past asserts the timer's output now.
}

// Clearing ENABLE deasserts the level-driven output; the pending clear covers a pend latched
// while masked.
void arch_timer_disarm(void)
{
    timer_disable();
    kickos_armv8a_gic_clear_pending(PPI_EL1_PHYS_TIMER);
}

// The exit status lets the harness tell a fault from a hang. AArch64 SYS_EXIT takes a pointer
// to a two-field block, where AArch32 passes the reason in the register.
void arch_shutdown(int status)
{
    uint64_t block[2];
    block[0] = ADP_Stopped_ApplicationExit;
    block[1] = static_cast<uint64_t>(static_cast<unsigned>(status));
    semihost(SYS_EXIT, block);
    halt_masked();
}

}
