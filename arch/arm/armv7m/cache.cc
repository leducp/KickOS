// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ARMv7-M L1 cache bring-up (Cortex-M7). The SCB cache-maintenance registers exist
// only on cache-equipped v7-M; on M3/M4 the CCR IC/DC bits are reserved and on v6-M
// the registers are absent -- so this is OPT-IN by call (a chip invokes it from
// arch_init), never ambient. Clean-room from the ARMv7-M ARM / Cortex-M7 TRM.
//
// M7 gotcha (see docs/design-teensy-mpu-hang.md): enabling a cache ARMS speculative
// prefetch of Normal memory, so the MPU anti-speculation regions MUST already be
// programmed (cache-after-MPU) before calling this -- else the M7 speculates into
// unbacked external memory and the AHB stalls with no fault.

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
    constexpr uint32_t CCR_DC = 1u << 16;
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

// Enable the L1 data cache. INVALIDATE the whole cache by set/way first -- never clean:
// RAM is live at boot and the cache lines are garbage, so a clean would write trash over
// RAM. Caller must have the MPU memory attributes correct first (cache-after-MPU).
extern "C" void kickos_armv7m_dcache_enable(void)
{
    r32(SCB_CSSELR) = 0u; // select L1 data cache
    __asm volatile("dsb" ::: "memory");
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
