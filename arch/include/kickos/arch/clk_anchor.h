// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The tickless-clock epoch anchor of every chip whose arch_clock_now converts a free-running
// tick count to ns:
//
//     ns = base_ns + (raw_ticks - base_ticks) * mult_q32
//
// The read is pure. The rate is written only at a rate edge: init and, on a retunable chip,
// reprice. A read that re-derived the rate from SystemCoreClock between its write and the
// re-anchor would reprice the whole elapsed history at the new rate, permanently.

#ifndef KICKOS_ARCH_CLK_ANCHOR_H
#define KICKOS_ARCH_CLK_ANCHOR_H

#include <stdint.h>

#include <kickos/arch/clk_q32.h>

namespace kickos
{
    // At -Os GCC outlines a body this size, which would put a call frame in every
    // arch_clock_now read.
#define KICKOS_CLK_ANCHOR_INLINE inline __attribute__((always_inline))

    // No constructor and no member initializers: a namespace-scope anchor stays zero-initialized
    // in .bss, so a static ctor calling ktime_now() before arch_init reads a defined all-zero one.
    struct arch_clk_anchor
    {
        uint64_t base_ns;    // ns already elapsed at the epoch
        uint64_t base_ticks; // raw tick count AT the epoch
        uint64_t mult_q32;   // Q32.32 ns-per-tick; 0 until init()

        // `hz` is the rate the raw ticks advance at, not always the core clock. Zero leaves
        // mult_q32 at 0, so ns_from returns 0.
        KICKOS_CLK_ANCHOR_INLINE void init(uint32_t hz)
        {
            if (hz == 0)
            {
                return;
            }
            mult_q32 = arch_clk_recip_q32(hz);
            base_ticks = 0;
            base_ns = 0;
        }

        // `ticks - base_ticks` wraps on purpose, so a 64-bit tick wrap stays correct.
        KICKOS_CLK_ANCHOR_INLINE uint64_t ns_from(uint64_t ticks) const
        {
            return base_ns + arch_clk_mul_q32(ticks - base_ticks, mult_q32);
        }

        // The caller captures the edge under the OLD rate before touching the clock hardware:
        //
        //     uint64_t const t0 = chip_ticks();
        //     uint64_t const ns0 = anchor.ns_from(t0);   // history at OLD pricing
        //     ... move the clock ...
        //     anchor.reprice(t0, ns0, new_hz);           // history preserved, new rate
        //
        // Call with the clock's readers excluded: the three stores are not atomic as a group.
        // The trailing barrier pins them ahead of any later read the compiler can see.
        KICKOS_CLK_ANCHOR_INLINE void reprice(uint64_t edge_ticks, uint64_t edge_ns, uint32_t hz)
        {
            if (hz == 0)
            {
                return;
            }
            base_ns = edge_ns;
            base_ticks = edge_ticks;
            mult_q32 = arch_clk_recip_q32(hz);
            __asm volatile("" ::: "memory");
        }
    };

#undef KICKOS_CLK_ANCHOR_INLINE
}

#endif
