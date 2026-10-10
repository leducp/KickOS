// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The arch boundary under the real system-call layer at the translating posture, beside the
// common page-table editor (tests/unit/common/host_aspace.h).
//
// Keep this header GTEST-FREE, for the reason kfixture.h states.

#ifndef KICKOS_TESTS_UNIT_SYSDISPATCH_SYS_SEAM_H
#define KICKOS_TESTS_UNIT_SYSDISPATCH_SYS_SEAM_H

#include <kickos/arch/arch.h>

#include <stddef.h>
#include <stdint.h>

#include "host_aspace.h"
#include "host_frame_pool.h"

namespace kickos
{
    namespace testfix
    {
        constexpr size_t SYS_GRANULE = HOST_POOL_GRANULE;

        // The bytes behind a user address of a pool frame, which the gate reads and writes.
        unsigned char* user_bytes(uintptr_t va);

        // arch_dcache_invalidate calls.
        extern uint32_t g_syncs;
        // arch_irq_window calls.
        extern uint32_t g_windows;
        // Runs at every window the kernel opens, with that window's ordinal since the hook was
        // set. A window the hook's own work opens does not call it again.
        using WindowHook = void (*)(uint32_t ordinal);
        void on_window(WindowHook fn);

        // Clears the counters, the hooks, the refusals and the pool's countdown. The pool and
        // the spaces keep what they hold.
        void sys_seam_reset();
    }
}

#endif
