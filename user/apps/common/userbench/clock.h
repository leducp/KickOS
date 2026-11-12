// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The bench's one clock: kos_clock_now, or CCOUNT on lx6. Where user code reads it with no
// kernel entry a row states each sample's p50/p99; elsewhere their mean.

#ifndef KICKOS_USER_APPS_COMMON_USERBENCH_CLOCK_H
#define KICKOS_USER_APPS_COMMON_USERBENCH_CLOCK_H

#include <stdint.h>

#include <kickos/board_config.h>
#include <kickos/kos.h>
#include <kickos/libc/fmt.h>

namespace bench
{
#if defined(__XTENSA__)
    // CCOUNT is per core and the cores' counts are not in step: both ends of a span must be
    // stamped on one core.
    constexpr bool FINE_CLOCK = true;
    inline uint32_t g_ccount_hz = 0;

    inline uint64_t stamp()
    {
        uint32_t c = 0;
        __asm__ volatile("rsr.ccount %0" : "=a"(c));
        return c;
    }

    inline uint32_t span_ns(uint64_t from, uint64_t to)
    {
        uint32_t const cycles = static_cast<uint32_t>(to) - static_cast<uint32_t>(from);
        if (g_ccount_hz == 0)
        {
            return 0;
        }
        return static_cast<uint32_t>(static_cast<uint64_t>(cycles) * 1000000000ull / g_ccount_hz);
    }
#else
    constexpr bool FINE_CLOCK = KICKOS_CLOCK_USER != 0;

    inline uint64_t stamp()
    {
        return kos_clock_now();
    }

    inline uint32_t span_ns(uint64_t from, uint64_t to)
    {
        return static_cast<uint32_t>(to - from);
    }
#endif

    // False where a stamp cannot be converted to nanoseconds.
    inline bool clock_seat(uint64_t cpu_hz)
    {
#if defined(__XTENSA__)
        g_ccount_hz = static_cast<uint32_t>(cpu_hz);
        return g_ccount_hz != 0;
#else
        (void)cpu_hz;
        return true;
#endif
    }

    // The samples of one row. Each is kept only on a fine clock; the mean needs none kept.
    struct Series
    {
        uint32_t* v;
        uint32_t cap;
        uint32_t n;
        uint64_t sum;

        void add(uint32_t x)
        {
            if (FINE_CLOCK and n < cap)
            {
                v[n] = x;
            }
            sum += x;
            n++;
        }

        uint32_t kept() const
        {
            if (n < cap)
            {
                return n;
            }
            return cap;
        }

        void sort()
        {
            uint32_t const k = kept();
            for (uint32_t i = 1; i < k; i++)
            {
                uint32_t const x = v[i];
                uint32_t j = i;
                while (j > 0 and v[j - 1u] > x)
                {
                    v[j] = v[j - 1u];
                    j--;
                }
                v[j] = x;
            }
        }

        // Of a sorted series.
        uint32_t ranked(uint32_t num, uint32_t den) const
        {
            uint32_t const k = kept();
            uint32_t r = static_cast<uint32_t>((static_cast<uint64_t>(k) * num + den - 1u) / den);
            if (r == 0)
            {
                r = 1;
            }
            return v[r - 1u];
        }

        // The median on a fine clock, the mean elsewhere; 0 for no sample.
        uint32_t figure()
        {
            if (n == 0)
            {
                return 0;
            }
            if (FINE_CLOCK)
            {
                sort();
                return ranked(1, 2);
            }
            return static_cast<uint32_t>(sum / n);
        }
    };

    inline Series series(uint32_t* storage, uint32_t cap)
    {
        Series s = {storage, cap, 0, 0};
        return s;
    }

    inline uint32_t less_tare(uint32_t v, uint32_t tare)
    {
        if (v < tare)
        {
            return 0;
        }
        return v - tare;
    }

    // `<p50>/<p99>/<max> ns  (p50/p99/max, n=<n>)` on a fine clock, `<mean> ns avg  (n=<n>)`
    // elsewhere, each less `tare`.
    inline void series_print(char const* row, Series* s, uint32_t tare, char const* tail)
    {
        char line[192];
        if (s->n == 0)
        {
            ksnprintf(line, sizeof(line), "  %s: SKIP (no sample)\n", row);
            kos::print(line);
            return;
        }
        uint32_t const fig = less_tare(s->figure(), tare);
        if (FINE_CLOCK)
        {
            ksnprintf(line, sizeof(line), "  %s: %u/%u/%u ns  (p50/p99/max, n=%u)%s\n", row,
                      static_cast<unsigned>(fig),
                      static_cast<unsigned>(less_tare(s->ranked(99, 100), tare)),
                      static_cast<unsigned>(less_tare(s->ranked(1, 1), tare)),
                      static_cast<unsigned>(s->kept()), tail);
        }
        else
        {
            ksnprintf(line, sizeof(line), "  %s: %u ns avg  (n=%u)%s\n", row,
                      static_cast<unsigned>(fig), static_cast<unsigned>(s->n), tail);
        }
        kos::print(line);
    }

    // Two stamps back to back: what a stamp adds to a span between two.
    inline uint32_t clock_tare(uint32_t* storage, uint32_t kept, uint32_t n)
    {
        Series s = series(storage, kept);
        for (uint32_t i = 0; i < n; i++)
        {
            uint64_t const a = stamp();
            uint64_t const b = stamp();
            s.add(span_ns(a, b));
        }
        return s.figure();
    }
}

#endif
