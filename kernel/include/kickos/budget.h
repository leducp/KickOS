// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Budgets: the most of each kind a task may hold, and the one admission every allocation goes
// through. The figure is the task's declaration's (<kickos/sys/image.h>), or root's, the init's
// from the pools descriptor (<kickos/pools.h>). Usage is DERIVED from what the task possesses at
// the moment of the admission, never counted beside it, so no release path owes the budget a
// decrement.
//
// A kind that names a pool (<kickos/sys/budget.h>) sizes it to the sum of its budgets, so a task
// within its budget always finds its slot.

#ifndef KICKOS_BUDGET_H
#define KICKOS_BUDGET_H

#include <stdint.h>

#include <kickos/sys/budget.h>

namespace kickos
{
    struct Task;

#define KICKOS_BUDGET_KIND(name, key, pool) name = KOS_BUDGET_##name,
    enum class BudgetKind : uint8_t
    {
        KOS_BUDGET_KINDS(KICKOS_BUDGET_KIND) COUNT = KOS_BUDGET_KIND_COUNT
    };
#undef KICKOS_BUDGET_KIND

    // What a row of a per-kind table names where its kind draws on no budget.
    constexpr BudgetKind NO_BUDGET = BudgetKind::COUNT;

    // The task's budget of `kind`: its declaration's for a declared instance, the init's for a
    // record the boot seats (root's, and each idle task's, which allocates nothing). Null-safe:
    // 0. Caller holds IrqLock.
    uint32_t task_budget(Task const* t, BudgetKind kind);

    // The capability-table width a thread of `t` takes where its spawn asks none: its
    // declaration's, or the init's for a record the boot seats. Null-safe: the init's. Caller
    // holds IrqLock.
    uint32_t task_cap_width(Task const* t);

    // What `t` holds of `kind` now, by the kind's derivation. Caller holds IrqLock.
    uint32_t task_usage(Task const* t, BudgetKind kind);

    // Whether `t` may take `n` more of `kind`: its usage plus `n` within its budget. A refusal is
    // the caller's -KOS_EAGAIN, or the null kos_ram_alloc answers. A null `t` is a claim of the
    // kernel's own, which no budget charges. Caller holds IrqLock.
    [[nodiscard]] bool task_admit(Task const* t, BudgetKind kind, uint32_t n);
}

#endif
