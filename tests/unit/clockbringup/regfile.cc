// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "regfile.h"

#include <gtest/gtest.h>

#include <map>
#include <vector>

namespace kickos::testfix::regfile
{
    namespace
    {
        struct Word
        {
            uint32_t v;
            bool touched;
        };

        // std::map: a word's storage never moves, so a reference r32 handed out stays valid.
        std::map<uintptr_t, Word> g_words;
        struct Plant
        {
            uintptr_t address;
            uint32_t mask;
            bool any_want;
            uint32_t want;
        };

        std::vector<Plant> g_timeouts;
        void (*g_hook)(uintptr_t) = nullptr;
        uint32_t g_waits = 0;
        uint32_t g_timed_out = 0;

        uint32_t width_mask(uint32_t width)
        {
            if (width == 1u)
            {
                return 0xFFu;
            }
            if (width == 2u)
            {
                return 0xFFFFu;
            }
            return 0xFFFFFFFFu;
        }
    }

    void reset()
    {
        g_words.clear();
        g_timeouts.clear();
        g_hook = nullptr;
        g_waits = 0;
        g_timed_out = 0;
    }

    void seed(uintptr_t address, uint32_t value)
    {
        g_words[address].v = value;
    }

    uint32_t value(uintptr_t address)
    {
        auto const it = g_words.find(address);
        if (it == g_words.end())
        {
            return 0u;
        }
        return it->second.v;
    }

    bool touched(uintptr_t address)
    {
        auto const it = g_words.find(address);
        return it != g_words.end() and it->second.touched;
    }

    void plant_timeout(uintptr_t address, uint32_t mask)
    {
        g_timeouts.push_back({address, mask, true, 0u});
    }

    void plant_timeout(uintptr_t address, uint32_t mask, uint32_t want)
    {
        g_timeouts.push_back({address, mask, false, want & mask});
    }

    void on_access(void (*hook)(uintptr_t address))
    {
        g_hook = hook;
    }

    uint32_t waits()
    {
        return g_waits;
    }

    uint32_t timeouts()
    {
        return g_timed_out;
    }

    volatile uint32_t& word(uintptr_t address)
    {
        if (g_hook != nullptr)
        {
            g_hook(address);
        }
        Word& w = g_words[address];
        w.touched = true;
        return w.v;
    }

    // Little-endian host: a byte or halfword access to a word is its low end.
    bool wait(void const volatile* reg, uint32_t mask, uint32_t want, uint32_t width)
    {
        g_waits++;
        for (auto& [address, w] : g_words)
        {
            if (static_cast<void const volatile*>(&w.v) != reg)
            {
                continue;
            }
            for (Plant const& p : g_timeouts)
            {
                if (p.address == address and p.mask == mask and (p.any_want or p.want == (want & mask)))
                {
                    g_timed_out++;
                    return false;
                }
            }
            uint32_t const keep = width_mask(width) & ~mask;
            w.v = (w.v & ~width_mask(width)) | (w.v & keep) | (want & mask);
            return true;
        }
        ADD_FAILURE() << "a wait on a register no access reached";
        return false;
    }
}
