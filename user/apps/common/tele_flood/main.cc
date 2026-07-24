// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Telemetry ring wrap / full-drop gate (spike CI gate 2). Emits far more records
// than the ch1 ring can hold (thousands of yield syscalls, each ENTER+EXIT), so
// the NoBlockSkip sink is driven full and drops whole records. The gate then
// asserts the drop path is RECORD-ATOMIC: the flushed file parses into whole
// records with contiguous sequence numbers (never a torn record), and the drop
// accounting reconciles -- decoded records + dropped == attempted (printed by
// kickos_trace_report_counters at shutdown).

#include <kickos/kos.h>

namespace
{
    // Enough to overflow any reasonable ch1 ring: each yield emits 2 records
    // (SYSCALL_ENTER + SYSCALL_EXIT) ~= 22 bytes, so 20000 yields ~= 440 KiB.
    constexpr int YIELDS = 20000;
}

int main(int, char**)
{
    kos::print("tele_flood: overflowing the ch1 ring\n");
    for (int i = 0; i < YIELDS; i++)
    {
        // yield with no other ready thread returns immediately (no switch), but
        // still traps -> a SYSCALL_ENTER/EXIT pair per iteration. Pure record
        // pressure, no timing dependence.
        kos::yield();
    }
    kos::print("tele_flood: done\n");
    return 0;
}
