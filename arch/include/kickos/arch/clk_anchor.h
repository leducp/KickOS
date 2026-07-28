// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Single source for the tickless-clock EPOCH ANCHOR (B2) shared by every chip whose
// arch_clock_now converts a free-running hardware tick count to ns:
//
//     ns = base_ns + (raw_ticks - base_ticks) * mult_q32
//
// The read side is PURE. The rate (mult_q32) is written ONLY at a rate edge: boot
// (init) and, on a retunable chip, the arch_cpu_clock_set re-anchor (reprice). A read
// that re-derived the rate from SystemCoreClock, called between the SystemCoreClock
// write and the re-anchor, would reprice the WHOLE elapsed history at the new rate and
// bake that phantom jump into the epoch permanently.
//
// The raw tick source and the Hz the ticks run at stay per-chip; only the arithmetic
// is shared.

#ifndef KICKOS_ARCH_CLK_ANCHOR_H
#define KICKOS_ARCH_CLK_ANCHOR_H

#include <stdint.h>

#include <kickos/arch/clk_q32.h> // the Q32.32 reciprocal + multiply this anchor is built on

namespace kickos
{
    // always_inline is load-bearing, not a hint: at -Os GCC emits an out-of-line copy
    // of a body this size, which would put a call frame in every arch_clock_now read.
#define KICKOS_CLK_ANCHOR_INLINE inline __attribute__((always_inline))

    // No constructor and no member initializers ON PURPOSE: a namespace-scope
    // arch_clk_anchor is zero-initialized (.bss, no __init_array entry), so a static
    // ctor calling ktime_now() before arch_init sees a defined all-zero anchor rather
    // than one whose own ctor has not run yet.
    struct arch_clk_anchor
    {
        uint64_t base_ns;    // ns already elapsed at the epoch
        uint64_t base_ticks; // raw tick count AT the epoch
        uint64_t mult_q32;   // Q32.32 ns-per-tick; 0 until init()

        // Anchor at boot: rate only, epoch at the tick origin. `hz` is the rate the
        // RAW TICKS advance at (not always the core clock; the chip derives it).
        // hz == 0 leaves mult_q32 at 0, so ns_from returns 0.
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

        // The pure read. `ticks - base_ticks` is deliberately unsigned-wrapping, so it
        // stays correct across a 64-bit tick wrap.
        KICKOS_CLK_ANCHOR_INLINE uint64_t ns_from(uint64_t ticks) const
        {
            return base_ns + arch_clk_mul_q32(ticks - base_ticks, mult_q32);
        }

        // Re-anchor at a rate edge (a retunable chip only). The caller captures the
        // edge under the OLD rate BEFORE touching the clock hardware:
        //
        //     uint64_t const t0 = chip_ticks();
        //     uint64_t const ns0 = anchor.ns_from(t0);   // history at OLD pricing
        //     ... move the clock ...
        //     anchor.reprice(t0, ns0, new_hz);           // history preserved, new rate
        //
        // so `now` is continuous across the edge: only the ticks inside the (masked,
        // brief) retune window are mispriced. Must be called with the clock's readers
        // excluded; the three stores are not atomic as a group. The trailing barrier
        // pins them ahead of any later read the compiler can see.
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
