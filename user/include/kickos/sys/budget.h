// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The kinds of budget a composition gives each task, and the kernel pool each one sizes. A
// declaration carries one figure per kind (<kickos/sys/image.h>); a system's pool of a kind holds
// the sum of every declaration's figure, the init's and the reserve's.
//
// THE ONE LIST: the kernel's BudgetKind, the export manifest's budgets and the composer's pool
// sizes are all generated from it. X(NAME, key, pool): the enumerator, the composition's key, and
// the pool the kind sizes, `none` where the kind draws on memory no pool counts.

#ifndef KICKOS_SYS_BUDGET_H
#define KICKOS_SYS_BUDGET_H

#define KOS_BUDGET_KINDS(X)                     \
    X(THREADS, threads, threads)                \
    X(RAM_BYTES, ram_bytes, none)               \
    X(RAM_BLOCKS, ram_blocks, ram_owners)       \
    X(PAGES, pages, none)                       \
    X(ENDPOINTS, endpoints, endpoints)          \
    X(NOTIFICATIONS, notifications, notifies)   \
    X(IRQ_BINDINGS, irq_bindings, irq_bindings) \
    X(FRAME_RUNS, frame_runs, frame_runs)       \
    X(CAP_SLOTS, cap_slots, cap_slab)

#define KOS_BUDGET_ENUMERATOR(name, key, pool) KOS_BUDGET_##name,

enum kos_budget_kind
{
    KOS_BUDGET_KINDS(KOS_BUDGET_ENUMERATOR) KOS_BUDGET_KIND_COUNT
};

#undef KOS_BUDGET_ENUMERATOR

#endif
