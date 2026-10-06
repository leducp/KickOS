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

#include <stddef.h>
#include <stdint.h>

namespace
{
    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }
    constexpr uintptr_t SCB_CCR = 0xE000ED14;    // Configuration and Control
    constexpr uintptr_t SCB_CCSIDR = 0xE000ED80; // Cache Size ID
    constexpr uintptr_t SCB_CSSELR = 0xE000ED84; // Cache Size Selection
    constexpr uintptr_t SCB_ICIALLU = 0xE000EF50; // I-cache invalidate all to PoU
    constexpr uintptr_t SCB_DCISW = 0xE000EF60;   // D-cache invalidate by set/way
    constexpr uintptr_t SCB_DCCMVAC = 0xE000EF68; // D-cache clean by address to PoC
    constexpr uintptr_t SCB_DCCIMVAC = 0xE000EF70; // D-cache clean and invalidate by address
    constexpr uint32_t CCR_DC = 1u << 16;
    constexpr uint32_t CCR_IC = 1u << 17;

    // One maintenance operation per line over [addr, addr + bytes). CCR.DC clear means no data
    // cache is on, which is every M3 and M4 and an M7 that never enabled it: nothing to do.
    void dcache_by_line(uintptr_t op, uintptr_t addr, size_t bytes)
    {
        if ((r32(SCB_CCR) & CCR_DC) == 0 or bytes == 0)
        {
            return;
        }
        // CSSELR is one register for every reader of CCSIDR, so the select and the read are
        // one masked span.
        arch_irq_state_t const s = arch_irq_save();
        r32(SCB_CSSELR) = 0u; // the L1 data cache
        __asm volatile("dsb" ::: "memory");
        __asm volatile("isb" ::: "memory");
        uint32_t const ccsidr = r32(SCB_CCSIDR);
        arch_irq_restore(s);
        uintptr_t const line = static_cast<uintptr_t>(1u) << ((ccsidr & 0x7u) + 4u);
        uintptr_t const end = addr + bytes;
        for (uintptr_t a = addr & ~(line - 1u); a < end; a += line)
        {
            r32(op) = static_cast<uint32_t>(a);
        }
        __asm volatile("dsb" ::: "memory");
        __asm volatile("isb" ::: "memory");
    }
}

extern "C" void arch_dcache_flush(void const* addr, size_t bytes)
{
    dcache_by_line(SCB_DCCMVAC, reinterpret_cast<uintptr_t>(addr), bytes);
}

extern "C" void arch_dcache_invalidate(void* addr, size_t bytes)
{
    dcache_by_line(SCB_DCCIMVAC, reinterpret_cast<uintptr_t>(addr), bytes);
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

// Enable the L1 data cache. INVALIDATE the whole cache by set/way first, never clean:
// RAM is live at boot and the cache lines are garbage, so a clean would write trash over
// RAM. Caller must have the MPU memory attributes correct first (cache-after-MPU).
extern "C" void kickos_armv7m_dcache_enable(void)
{
    r32(SCB_CSSELR) = 0u; // select L1 data cache
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
    uint32_t const ccsidr = r32(SCB_CCSIDR);
    uint32_t const assoc = (ccsidr >> 3) & 0x3FFu;   // ways - 1
    uint32_t const nsets = (ccsidr >> 13) & 0x7FFFu; // sets - 1
    uint32_t const way_shift = static_cast<uint32_t>(__builtin_clz(assoc));
    uint32_t const set_shift = (ccsidr & 0x7u) + 4u; // log2(line bytes)
    for (int32_t s = static_cast<int32_t>(nsets); s >= 0; s--)
    {
        for (int32_t w = static_cast<int32_t>(assoc); w >= 0; w--)
        {
            r32(SCB_DCISW) = (static_cast<uint32_t>(w) << way_shift)
                             | (static_cast<uint32_t>(s) << set_shift);
        }
    }
    __asm volatile("dsb" ::: "memory");
    r32(SCB_CCR) |= CCR_DC;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
}
