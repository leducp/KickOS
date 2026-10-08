// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/corestart.h>

#if KICKOS_KERNEL_CORES > 1

#include <kickos/instance.h>
#include <kickos/sched.h>
#include <kickos/sys/atomic.h>

#include <stddef.h>

namespace kickos
{
    namespace
    {
        // A53 cache line, so no two cores write one.
        constexpr size_t CORESTART_CACHE_LINE = 64u;

        using Flag = Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE>;

        // EACH CELL HAS EXACTLY ONE WRITER: `seated` is written by the core running kmain and
        // read by this row's own core, `arrived` the other way about.
        struct alignas(CORESTART_CACHE_LINE) CoreRow
        {
            Flag seated;
            Flag arrived;
        };

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
        return g_row[core].arrived.load() != 0u;
    }

    // The idle cell is read only behind the acquire that publishes it.
    extern "C" int kickos_kernel_core_startable(void)
    {
        KernelCore const me = kickos_kernel_core();
        return static_cast<int>(g_row[me].seated.load() != 0u and kernel().idle(me) != nullptr);
    }

    extern "C" void kickos_kernel_core_start(void)
    {
        g_row[kickos_kernel_core()].arrived = 1u;
        sched::start();
        while (true)
        {
            arch_idle_wait();
        }
    }
}

#endif
