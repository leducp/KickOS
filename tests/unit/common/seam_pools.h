// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The pools a host gate's kernel runs over, and the budgets of its root task, which a system's
// composition gives an image (tests/unit/common/pools_seam.cc). A posture overrides any by -D.

#ifndef KICKOS_TESTS_UNIT_COMMON_SEAM_POOLS_H
#define KICKOS_TESTS_UNIT_COMMON_SEAM_POOLS_H

#include <kickos/config.h>

// The thread pool, root's slot included.
#ifndef KICKOS_SEAM_THREAD_SLOTS
#define KICKOS_SEAM_THREAD_SLOTS 17
#endif
#ifndef KICKOS_SEAM_TASKS
#define KICKOS_SEAM_TASKS 18
#endif
#ifndef KICKOS_SEAM_DOMAINS
#if KICKOS_HAVE_ASPACE
#define KICKOS_SEAM_DOMAINS 20
#else
#define KICKOS_SEAM_DOMAINS 18
#endif
#endif
#ifndef KICKOS_SEAM_ENDPOINTS
#define KICKOS_SEAM_ENDPOINTS 4
#endif
#ifndef KICKOS_SEAM_NOTIFY
#define KICKOS_SEAM_NOTIFY 8
#endif
#ifndef KICKOS_SEAM_IRQ_BINDINGS
#define KICKOS_SEAM_IRQ_BINDINGS 8
#endif
#ifndef KICKOS_SEAM_FRAME_RUNS
#define KICKOS_SEAM_FRAME_RUNS 8
#endif
#ifndef KICKOS_SEAM_RAM_OWNERS
#define KICKOS_SEAM_RAM_OWNERS 48
#endif

// Root's budgets, which every task the fixture seats without a declaration shares.
// The init's table width, the width a fixture thread's table takes, and the slab backing both: one
// fixture table for every thread slot beside the init's.
#ifndef KICKOS_SEAM_ROOT_WIDTH
#define KICKOS_SEAM_ROOT_WIDTH 16
#endif
#ifndef KICKOS_SEAM_CAP_WIDTH
#define KICKOS_SEAM_CAP_WIDTH 8
#endif
#ifndef KICKOS_SEAM_CAP_SLOTS
#define KICKOS_SEAM_CAP_SLOTS (KICKOS_SEAM_ROOT_WIDTH + KICKOS_SEAM_THREAD_SLOTS * KICKOS_SEAM_CAP_WIDTH)
#endif
#ifndef KICKOS_SEAM_CAP_DIR
#define KICKOS_SEAM_CAP_DIR (KICKOS_SEAM_ROOT_WIDTH / 8)
#endif

#ifndef KICKOS_SEAM_BUDGET_THREADS
#define KICKOS_SEAM_BUDGET_THREADS KICKOS_SEAM_THREAD_SLOTS
#endif
#ifndef KICKOS_SEAM_BUDGET_RAM_BYTES
#define KICKOS_SEAM_BUDGET_RAM_BYTES 0x7FFFFFFF
#endif
// A translating posture's blocks are its space's reservations, which no owner record bounds.
#ifndef KICKOS_SEAM_BUDGET_RAM_BLOCKS
#if KICKOS_HAVE_ASPACE
#define KICKOS_SEAM_BUDGET_RAM_BLOCKS 0x7FFFFFFF
#else
#define KICKOS_SEAM_BUDGET_RAM_BLOCKS KICKOS_SEAM_RAM_OWNERS
#endif
#endif
#ifndef KICKOS_SEAM_BUDGET_PAGES
#define KICKOS_SEAM_BUDGET_PAGES 0x7FFFFFFF
#endif
#ifndef KICKOS_SEAM_BUDGET_ENDPOINTS
#define KICKOS_SEAM_BUDGET_ENDPOINTS 3
#endif
#ifndef KICKOS_SEAM_BUDGET_NOTIFICATIONS
#define KICKOS_SEAM_BUDGET_NOTIFICATIONS 7
#endif
#ifndef KICKOS_SEAM_BUDGET_IRQ_BINDINGS
#define KICKOS_SEAM_BUDGET_IRQ_BINDINGS 7
#endif
#ifndef KICKOS_SEAM_BUDGET_FRAME_RUNS
#define KICKOS_SEAM_BUDGET_FRAME_RUNS 7
#endif
#ifndef KICKOS_SEAM_BUDGET_CAP_SLOTS
#define KICKOS_SEAM_BUDGET_CAP_SLOTS KICKOS_SEAM_CAP_SLOTS
#endif

// A fixture declaration's last two fields: its width, and its budgets by enum kos_budget_kind.
#define KICKOS_SEAM_DECL_BUDGETS                                                                  \
    KICKOS_SEAM_CAP_WIDTH,                                                                        \
    {                                                                                             \
        KICKOS_SEAM_BUDGET_THREADS, KICKOS_SEAM_BUDGET_RAM_BYTES, KICKOS_SEAM_BUDGET_RAM_BLOCKS,     \
            KICKOS_SEAM_BUDGET_PAGES, KICKOS_SEAM_BUDGET_ENDPOINTS,                              \
            KICKOS_SEAM_BUDGET_NOTIFICATIONS, KICKOS_SEAM_BUDGET_IRQ_BINDINGS,                   \
            KICKOS_SEAM_BUDGET_FRAME_RUNS, KICKOS_SEAM_BUDGET_CAP_SLOTS                          \
    }

#endif
