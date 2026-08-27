// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// AArch64 data-cache maintenance to the Point of Coherency, the backend of arch.h's
// flush/invalidate seam. Clean-room from the Armv8-A ARM (DDI 0487, section D7 cache
// maintenance) and the Cortex-A53 TRM (DDI 0500J section 4.3.26, CTR_EL0).
//
// NOTHING IN THE TREE CALLS THIS YET (docs/design-m6-mmu.md section 7), so this member is
// compiled and never extracted. It is here rather than absent because a port that grows a
// caller must get maintenance, not a no-op that reports it.

#include <kickos/arch/arch.h>

#include <stddef.h>
#include <stdint.h>

namespace
{
    // CTR_EL0.DminLine is log2 of the smallest data line in WORDS across every level, which
    // is the figure a by-address loop must step by: the A53's is 16 words, but a part with a
    // smaller line elsewhere in its hierarchy would leave lines untouched at 64.
    size_t dcache_line()
    {
        uint64_t ctr = 0;
        __asm volatile("mrs %0, ctr_el0" : "=r"(ctr));
        unsigned const words = 1u << ((ctr >> 16) & 0xFu);
        return static_cast<size_t>(words) * 4u;
    }

    // The half-open line-aligned span, or false where there is nothing to do. The end is
    // computed before the alignment so a caller's unaligned tail is inside the last line.
    bool span_of(void const* addr, size_t bytes, uintptr_t* first, uintptr_t* end,
                 size_t* line)
    {
        if (bytes == 0)
        {
            return false;
        }
        uintptr_t const a = reinterpret_cast<uintptr_t>(addr);
        if (bytes > UINTPTR_MAX - a)
        {
            return false; // a wrapping range names no memory
        }
        size_t const l = dcache_line();
        *line = l;
        *first = a & ~static_cast<uintptr_t>(l - 1u);
        *end = a + bytes;
        return true;
    }
}

extern "C"
{

void arch_dcache_flush(void const* addr, size_t bytes)
{
    uintptr_t p = 0;
    uintptr_t end = 0;
    size_t line = 0;
    if (not span_of(addr, bytes, &p, &end, &line))
    {
        return;
    }
    for (; p < end; p += line)
    {
        __asm volatile("dc cvac, %0" ::"r"(p) : "memory");
    }
    // The observer's read may be issued by hardware that never executes our code, so the
    // barrier is what orders the writes out against whatever the caller writes next to start it.
    __asm volatile("dsb ish" ::: "memory");
}

void arch_dcache_invalidate(void* addr, size_t bytes)
{
    uintptr_t p = 0;
    uintptr_t end = 0;
    size_t line = 0;
    if (not span_of(addr, bytes, &p, &end, &line))
    {
        return;
    }
    for (; p < end; p += line)
    {
        // CIVAC and not IVAC: the first and last lines are shared with whatever sits beside
        // the buffer, and a plain invalidate discards their dirty bytes with no way for the
        // caller to have prevented it.
        __asm volatile("dc civac, %0" ::"r"(p) : "memory");
    }
    __asm volatile("dsb ish" ::: "memory");
}

}
