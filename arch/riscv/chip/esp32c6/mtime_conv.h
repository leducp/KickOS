// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// MTIME rate and tick <-> nanosecond conversion, free of the chip's register headers so a host
// unit test can pin them.
//
// MTIME counts CPU_CLK, so its rate is 160 MHz >> shift: every rate the XTAL and PLL dividers
// produce has that form.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_MTIME_CONV_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_MTIME_CONV_H

#include <stddef.h>
#include <stdint.h>

namespace kickos::esp32c6
{
    constexpr uint32_t MTIME_TOP_HZ = 160000000u;
    constexpr uint32_t MTIME_SHIFT_MAX = 8u;

    constexpr uint32_t SOC_CLK_SEL_XTAL = 0u;
    constexpr uint32_t SOC_CLK_SEL_PLL = 1u;
    constexpr uint32_t PLL_ROOT_HZ = 160000000u;

    // CPU_CLK in Hz from the PCR fields, or 0 where the source has no exact rate (RC_FAST, the
    // reserved selection, a divider that does not divide).
    inline uint32_t cpu_clk_hz(uint32_t soc_clk_sel, uint32_t xtal_mhz, uint32_t cpu_ls_div_num,
                               uint32_t cpu_hs_div_num, bool hs_120m_force)
    {
        uint32_t root = 0;
        uint32_t div = 0;
        if (soc_clk_sel == SOC_CLK_SEL_XTAL)
        {
            root = xtal_mhz * 1000000u;
            div = cpu_ls_div_num + 1u;
        }
        else if (soc_clk_sel == SOC_CLK_SEL_PLL)
        {
            if (hs_120m_force and cpu_hs_div_num == 0u)
            {
                return 120000000u;
            }
            root = PLL_ROOT_HZ;
            div = cpu_hs_div_num + 1u;
        }
        else
        {
            return 0u;
        }
        if (root % div != 0u)
        {
            return 0u;
        }
        return root / div;
    }

    namespace detail
    {
        inline size_t put_text(char* out, size_t at, char const* s)
        {
            while (*s != '\0')
            {
                out[at++] = *s++;
            }
            return at;
        }

        inline size_t put_dec(char* out, size_t at, uint32_t v)
        {
            char digits[10];
            size_t n = 0;
            do
            {
                digits[n++] = static_cast<char>('0' + v % 10u);
                v /= 10u;
            } while (v != 0u);
            while (n != 0u)
            {
                out[at++] = digits[--n];
            }
            return at;
        }

        inline size_t put_hex(char* out, size_t at, uint32_t v)
        {
            at = put_text(out, at, "0x");
            for (int nibble = 7; nibble >= 0; nibble--)
            {
                out[at++] = "0123456789abcdef"[(v >> (4 * nibble)) & 0xFu];
            }
            return at;
        }
    }

    constexpr size_t CLOCK_REFUSAL_MAX = 160u;

    // The boot's refusal of a CPU clock MTIME cannot count exactly, naming the rate decoded (0 for
    // none) and the two PCR words it came from. Answers the length written into `out`.
    inline size_t clock_refusal(char (&out)[CLOCK_REFUSAL_MAX], uint32_t hz, uint32_t sysclk_conf,
                                uint32_t cpu_freq_conf)
    {
        size_t at = detail::put_text(out, 0, "KickOS: ESP32-C6 CPU clock of ");
        at = detail::put_dec(out, at, hz);
        at = detail::put_text(out, at, " Hz is not 160 MHz >> n, so MTIME has no exact rate (SYSCLK_CONF ");
        at = detail::put_hex(out, at, sysclk_conf);
        at = detail::put_text(out, at, ", CPU_FREQ_CONF ");
        at = detail::put_hex(out, at, cpu_freq_conf);
        return detail::put_text(out, at, ")\n");
    }

    // The shift with hz == MTIME_TOP_HZ >> shift, or -1 for a rate the conversions below cannot
    // express exactly.
    inline int mtime_shift_of(uint32_t hz)
    {
        for (uint32_t shift = 0; shift <= MTIME_SHIFT_MAX; shift++)
        {
            if ((MTIME_TOP_HZ >> shift) == hz)
            {
                return static_cast<int>(shift);
            }
        }
        return -1;
    }

    // (25 << shift) / 4 ns per tick, exactly: an integer ns-per-tick would truncate 160 MHz's
    // 6.25. Overflows only past ~146 years at any shift. The shift stays in 32 bits: rv32 lowers a
    // variable 64-bit shift to a libgcc call.
    inline uint64_t mtime_ticks_to_ns(uint64_t ticks, uint32_t shift)
    {
        uint32_t const per_4ns = 25u << shift;
        return (ticks * per_4ns) >> 2;
    }

    // v >> shift for shift < 32, in 32-bit halves for the same reason.
    inline uint64_t mtime_shr64(uint64_t v, uint32_t shift)
    {
        uint32_t const lo = static_cast<uint32_t>(v);
        uint32_t const hi = static_cast<uint32_t>(v >> 32);
        uint32_t const carried = (hi << 1) << (31u - shift);
        return (static_cast<uint64_t>(hi >> shift) << 32) | ((lo >> shift) | carried);
    }

    // High 64 bits of the 128-bit product a*b, from 32x32->64 partial products: rv32
    // has no wide multiply and no __int128.
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

    // floor(ns * 4 / (25 << shift)) as a reciprocal multiply: a plain 64-bit divide lowers to
    // rv32's software __udivdi3, whose cost grows with uptime inside arch_timer_arm's masked
    // window. (ns * MAGIC) >> 66 equals floor(ns*4/25) for every ns < 3883525068149379288
    // (about 123 years), MAGIC = ceil(4 * 2**66 / 25); the further shift keeps it exact since
    // floor(floor(x) / 2^s) == floor(x / 2^s). tests/unit/mtimeconv pins the bound.
    inline uint64_t mtime_ns_to_ticks(uint64_t ns, uint32_t shift)
    {
        constexpr uint64_t MAGIC = 11805916207174113035ull;
        return mtime_shr64(mtime_umulh64(ns, MAGIC) >> 2, shift);
    }
}

#endif
