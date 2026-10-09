// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The arch_aspace_acquire holds a host seam has handed out. Keep this header independent of
// GTest.

#ifndef KICKOS_TESTS_UNIT_COMMON_ACQUIRE_HOLDS_H
#define KICKOS_TESTS_UNIT_COMMON_ACQUIRE_HOLDS_H

#include <stddef.h>
#include <stdint.h>

struct arch_aspace;

namespace kickos
{
    namespace testfix
    {
        struct AcquireHolds
        {
            struct Hold
            {
                struct arch_aspace const* space;
                uintptr_t va;
            };
            Hold held[8] = {};
            size_t live = 0;
            // The most holds live at once.
            size_t peak = 0;
            size_t unpaired = 0;

            void take(struct arch_aspace const* space, uintptr_t va)
            {
                if (live < sizeof(held) / sizeof(held[0]))
                {
                    held[live] = Hold{space, va};
                    live++;
                }
                if (live > peak)
                {
                    peak = live;
                }
            }

            // Pairs by (space, address), not by count: a missed release and a doubled one, or a
            // release at the wrong address, would otherwise net out.
            void give(struct arch_aspace const* space, uintptr_t va)
            {
                for (size_t i = 0; i < live; i++)
                {
                    if (held[i].space == space and held[i].va == va)
                    {
                        live--;
                        held[i] = held[live];
                        return;
                    }
                }
                unpaired++;
            }
        };
    }
}

#endif
