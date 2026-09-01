// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/corestart.h>

#if KICKOS_KERNEL_CORES > 1

#include <kickos/instance.h>
#include <kickos/sys/atomic.h>

#include <stddef.h>

namespace kickos
{
    namespace
    {
        // A53 cache line, so no two cores write one.
        constexpr size_t CORESTART_CACHE_LINE = 64u;

        using Flag = Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE>;

        // One row per core, rounded up to a whole line. EACH CELL HAS EXACTLY ONE WRITER:
        // `seated` is written by the core running kmain and read by this row's own core,
        // `arrived` the other way about.
        struct alignas(CORESTART_CACHE_LINE) CoreRow
        {
            Flag seated;
            Flag arrived;
        };
        static_assert(sizeof(CoreRow) % CORESTART_CACHE_LINE == 0,
                      "a row shorter than a line would share one with the next writer");

        CoreRow g_row[KICKOS_KERNEL_CORES] = {};
    }

    void corestart_seat(uint32_t cores)
    {
        for (uint32_t core = 0; core < KICKOS_KERNEL_CORES; core++)
        {
            if ((cores & (1u << core)) != 0)
            {
                g_row[core].seated = 1u;
            }
        }
    }

    bool corestart_arrived(uint32_t core)
    {
        if (core >= KICKOS_KERNEL_CORES)
        {
            return false;
        }
        return g_row[core].arrived.load() != 0u;
    }

    // The arch seam's two bring-up halves (arch/include/kickos/arch/arch.h).
    extern "C" int kickos_kernel_core_seated(void)
    {
        int answer = 0;
        if (g_row[kickos_kernel_core()].seated.load() != 0u)
        {
            answer = 1;
        }
        return answer;
    }

    extern "C" void kickos_kernel_core_arrive(void)
    {
        g_row[kickos_kernel_core()].arrived = 1u;
    }
}

#endif
