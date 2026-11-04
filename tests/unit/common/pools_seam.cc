// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The kernel's pools for a host fixture, at the sizes its posture configures: what a system's
// generated pools TU defines for an image.

#include "seam_pools.h"

#include <kickos/pool_storage.h>

KICKOS_SYSTEM_POOLS(KICKOS_SEAM_THREAD_SLOTS, KICKOS_SEAM_TASKS, KICKOS_SEAM_DOMAINS,
                    KICKOS_SEAM_ENDPOINTS, KICKOS_SEAM_NOTIFY, KICKOS_SEAM_IRQ_BINDINGS,
                    KICKOS_SEAM_FRAME_RUNS, KICKOS_SEAM_RAM_OWNERS, KICKOS_SEAM_CAP_SLOTS,
                    KICKOS_SEAM_CAP_DIR, KICKOS_SEAM_ROOT_WIDTH, KICKOS_SEAM_BUDGET_THREADS,
                    KICKOS_SEAM_BUDGET_RAM_BYTES, KICKOS_SEAM_BUDGET_RAM_BLOCKS,
                    KICKOS_SEAM_BUDGET_PAGES, KICKOS_SEAM_BUDGET_ENDPOINTS,
                    KICKOS_SEAM_BUDGET_NOTIFICATIONS, KICKOS_SEAM_BUDGET_IRQ_BINDINGS,
                    KICKOS_SEAM_BUDGET_FRAME_RUNS, KICKOS_SEAM_BUDGET_CAP_SLOTS)

#include <new>

namespace kickos::testfix
{
    // Every pool of the running instance back to its boot state, for a fixture that rebuilds
    // the kernel between arms.
    void pools_reset()
    {
        new (&system_pools[kickos_instance_index()]) SystemPools{};
#if KICKOS_HAVE_ASPACE or KICKOS_BENCH_SCHED_ON
        new (&system_shared) SystemPools::Shared{};
#endif
    }
}
