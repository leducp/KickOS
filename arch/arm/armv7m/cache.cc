// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARMv7-M L1 cache bring-up and data-cache maintenance (Cortex-M7). The SCB cache-maintenance
// registers exist only on cache-equipped v7-M; on M3/M4 the CCR IC/DC bits are reserved and on
// v6-M the registers are absent, so bring-up is OPT-IN by call (a chip invokes it from
// arch_init), never ambient, and maintenance does nothing while CCR.DC is clear. Clean-room
// from the ARMv7-M ARM / Cortex-M7 TRM.
//
// M7 gotcha: enabling a cache ARMS speculative prefetch of Normal memory, so the
// MPU anti-speculation regions MUST already be programmed (cache-after-MPU)
// before calling this, else the M7 speculates into unbacked external memory and
// the AHB stalls with no fault.

#include <kickos/arch/arch.h>
#include <kickos/chip_limits.h>

#include <stddef.h>
#include <stdint.h>

namespace
{
    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }
    constexpr uintptr_t SCB_CCR = 0xE000ED14;    // Configuration and Control
    constexpr uintptr_t SCB_ICIALLU = 0xE000EF50; // I-cache invalidate all to PoU
    constexpr uint32_t CCR_IC = 1u << 17;
}

// Enable the L1 instruction cache. Invalidate to PoU first (reset state is garbage).
extern "C" void kickos_armv7m_icache_enable(void)
{
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
    r32(SCB_ICIALLU) = 0u; // invalidate whole I-cache (also flushes the branch predictor)
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
    r32(SCB_CCR) |= CCR_IC;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}

#if KICKOS_CHIP_DCACHE

#include <kickos/diag.h>

#include "dcache_plan.h"

namespace kickos
{
    void kpanic(char const* msg) __attribute__((noreturn));
}

namespace
{
    constexpr uintptr_t SCB_CCSIDR = 0xE000ED80; // Cache Size ID
    constexpr uintptr_t SCB_CSSELR = 0xE000ED84; // Cache Size Selection
    constexpr uintptr_t SCB_DCISW = 0xE000EF60;   // D-cache invalidate by set/way
    constexpr uintptr_t SCB_DCCMVAC = 0xE000EF68; // D-cache clean by address to PoC
    constexpr uintptr_t SCB_DCCSW = 0xE000EF6C;   // D-cache clean by set/way
    constexpr uintptr_t SCB_DCCIMVAC = 0xE000EF70; // D-cache clean and invalidate by address
    constexpr uintptr_t SCB_DCCISW = 0xE000EF74;   // D-cache clean and invalidate by set/way
    constexpr uint32_t CCR_DC = 1u << 16;

    // The L1 data cache's CCSIDR, read once before CCR.DC is set.
    uint32_t g_dcache_ccsidr = 0;

    void dcache_set_way(uintptr_t op, uint32_t ccsidr)
    {
        uint32_t const assoc = (ccsidr >> 3) & 0x3FFu;   // ways - 1
        uint32_t const nsets = (ccsidr >> 13) & 0x7FFFu; // sets - 1
        uint32_t way_shift = 32u;
        if (assoc != 0u)
        {
            way_shift = static_cast<uint32_t>(__builtin_clz(assoc));
        }
        uint32_t const set_shift = (ccsidr & 0x7u) + 4u; // log2(line bytes)
        for (int32_t s = static_cast<int32_t>(nsets); s >= 0; s--)
        {
            for (int32_t w = static_cast<int32_t>(assoc); w >= 0; w--)
            {
                uint32_t way = 0u;
                if (way_shift < 32u)
                {
                    way = static_cast<uint32_t>(w) << way_shift;
                }
                r32(op) = way | (static_cast<uint32_t>(s) << set_shift);
            }
        }
    }

    // CCR.DC clear means no data cache is on, an M7 that never enabled it: nothing to
    // maintain, but a wrapping range is refused all the same.
    void dcache_maintain(uintptr_t by_line, uintptr_t by_set_way, uintptr_t addr, size_t bytes)
    {
        uint32_t const ccsidr = g_dcache_ccsidr;
        kickos::armv7m::DcachePlan const plan = kickos::armv7m::dcache_plan(ccsidr, addr, bytes);
        if (plan.kind == kickos::armv7m::DcachePlan::Kind::WRAPS)
        {
            kickos::kpanic(kickos::diag::kDcacheWraps);
        }
        if ((r32(SCB_CCR) & CCR_DC) == 0 or plan.kind == kickos::armv7m::DcachePlan::Kind::NONE)
        {
            return;
        }
        // Maintenance by address or by set/way is unordered against the stores before it
        // without a DSB (ARMv7-M ARM B2.2.7, p.B2-579).
        __asm volatile("dsb" ::: "memory");
        if (plan.kind == kickos::armv7m::DcachePlan::Kind::SET_WAY)
        {
            dcache_set_way(by_set_way, ccsidr);
        }
        else
        {
            kickos::armv7m::dcache_each_line(plan, [by_line](uintptr_t a) {
                r32(by_line) = static_cast<uint32_t>(a);
            });
        }
        __asm volatile("dsb" ::: "memory");
        __asm volatile("isb" ::: "memory");
    }
}

extern "C" void arch_dcache_flush(void const* addr, size_t bytes)
{
    dcache_maintain(SCB_DCCMVAC, SCB_DCCSW, reinterpret_cast<uintptr_t>(addr), bytes);
}

extern "C" void arch_dcache_invalidate(void* addr, size_t bytes)
{
    dcache_maintain(SCB_DCCIMVAC, SCB_DCCISW, reinterpret_cast<uintptr_t>(addr), bytes);
}

// Enable the L1 data cache. INVALIDATE the whole cache by set/way first, never clean:
// RAM is live at boot and the cache lines are garbage, so a clean would write trash over
// RAM. Caller must have the MPU memory attributes correct first (cache-after-MPU).
extern "C" void kickos_armv7m_dcache_enable(void)
{
    r32(SCB_CSSELR) = 0u; // select L1 data cache
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
    uint32_t const ccsidr = r32(SCB_CCSIDR);
    g_dcache_ccsidr = ccsidr;
    dcache_set_way(SCB_DCISW, ccsidr);
    __asm volatile("dsb" ::: "memory");
    r32(SCB_CCR) |= CCR_DC;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}

#endif
