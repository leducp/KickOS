// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MTIME tick <-> nanosecond conversion, split out of chip_esp32c6.cc so a host unit
// test can pin mtime_ns_to_ticks against the plain "ns * 4ull / 25ull" it replaces
// without dragging in the chip's register headers.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_MTIME_CONV_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_MTIME_CONV_H

#include <stdint.h>

namespace kickos::esp32c6
{
    // MTIME is core-clocked ~160 MHz (reg::clint::MTIME_HZ): 1e9/160e6 = 6.25 ns/tick =
    // 25/4 exactly. An integer ns-per-tick (=6) would truncate and run every
    // sleep/timestamp 4.17% long, so convert with the exact 25/4 ratio in 64-bit
    // (overflows only past ~1460 yr at 160 MHz). 25 is a compile-time constant small
    // enough that GCC synthesizes this as a widening multiply + shift, not a libcall.
    inline uint64_t mtime_ticks_to_ns(uint64_t ticks)
    {
        return ticks * 25ull / 4ull;
    }

    // High 64 bits of the 128-bit product a*b, from 32x32->64 partial products: rv32
    // has no wide multiply and no __int128. Used only by mtime_ns_to_ticks below.
    inline uint64_t mtime_umulh64(uint64_t a, uint64_t b)
    {
        uint32_t const a_lo = static_cast<uint32_t>(a);
        uint32_t const a_hi = static_cast<uint32_t>(a >> 32);
        uint32_t const b_lo = static_cast<uint32_t>(b);
        uint32_t const b_hi = static_cast<uint32_t>(b >> 32);
        uint64_t const lo_lo = static_cast<uint64_t>(a_lo) * b_lo;
        uint64_t const hi_lo = static_cast<uint64_t>(a_hi) * b_lo;
        uint64_t const lo_hi = static_cast<uint64_t>(a_lo) * b_hi;
        uint64_t const hi_hi = static_cast<uint64_t>(a_hi) * b_hi;
        uint64_t const cross = (lo_lo >> 32) + static_cast<uint32_t>(hi_lo) + static_cast<uint32_t>(lo_hi);
        return hi_hi + (hi_lo >> 32) + (lo_hi >> 32) + (cross >> 32);
    }

    // ns*4/25, the inverse of mtime_ticks_to_ns, as a reciprocal multiply. 25 has no
    // power-of-two factor, so unlike mtime_ticks_to_ns this constant divide does not
    // reduce to a shift: rv32imac has no hardware divider, and GCC lowers the plain
    // "ns * 4ull / 25ull" to a call to the software __udivdi3 (confirmed on the built
    // object; R_RISCV_CALL_PLT to __udivdi3 in arch_timer_arm's disassembly). That call
    // sits inside arch_timer_arm's masked window every time (kernel/time.cc ktime_rearm,
    // always under IrqLock), and __udivdi3's bit-serial cost tracks the magnitude of ns,
    // i.e. grows with uptime.
    //
    // (ns * MAGIC) >> 66 matches floor(ns*4/25) bit-for-bit for every
    // ns < 3883525068149379288 (about 123 years of continuous uptime); MAGIC = ceil(4 *
    // 2**66 / 25) is the largest such constant that still fits a uint64_t (66 is the
    // largest shift for which that ceiling stays under 2**64). tests/unit/mtimeconv
    // pins the exact bound and sweeps the full domain against the old arithmetic. No
    // branch, no loop: fixed cost regardless of ns.
    inline uint64_t mtime_ns_to_ticks(uint64_t ns)
    {
        constexpr uint64_t MAGIC = 11805916207174113035ull;
        return mtime_umulh64(ns, MAGIC) >> 2;
    }
}

#endif
