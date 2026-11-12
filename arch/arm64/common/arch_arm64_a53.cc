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
    // MDSCR_EL1.TDCC: EL0 access to the debug comms channel traps.
    constexpr uint64_t MDSCR_TDCC = 1u << 12;
    // ID_AA64DFR0_EL1.PMUVer, bits [11:8]: 0 none, 0xF IMPLEMENTATION DEFINED.
    constexpr uint32_t DFR0_PMUVER_SHIFT = 8;
    constexpr uint64_t DFR0_PMUVER_MASK = 0xF;
    constexpr uint64_t PMUVER_NONE = 0x0;
    constexpr uint64_t PMUVER_IMPDEF = 0xF;

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

    // Both reset UNKNOWN: a debug enable reaches EL1, a clear TDCC hands EL0 the per-core debug
    // comms channel and a PMUSERENR_EL0.EN the counters, both shared by every task.
    __asm volatile("msr mdscr_el1, %0\n\tisb" ::"r"(MDSCR_TDCC) : "memory");
    uint64_t dfr0 = 0;
    __asm volatile("mrs %0, id_aa64dfr0_el1" : "=r"(dfr0));
    uint64_t const pmuver = (dfr0 >> DFR0_PMUVER_SHIFT) & DFR0_PMUVER_MASK;
    if (pmuver != PMUVER_NONE and pmuver != PMUVER_IMPDEF)
    {
        __asm volatile("msr pmuserenr_el0, xzr\n\tisb" ::: "memory");
    }

#if KICKOS_KERNEL_CORES > 1
    // EL0 reads TPIDRRO_EL0 and cannot write it: kos_core_current answers from it what
    // KOS_SYS_CORE_CURRENT answers, kickos_kernel_core(), which is arch_cpu_id() only under the
    // shared model.
    static_assert(KICKOS_MULTICORE_MODEL_SHARED, "the seated id must be the kernel core's slot");
    __asm volatile("msr tpidrro_el0, %0" ::"r"(static_cast<uint64_t>(arch_cpu_id())));
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
