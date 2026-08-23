// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Telemetry ring wrap / full-drop witness. Emits far more records than the ch1 ring can
// hold, so the NoBlockSkip sink runs full and drops whole records. What that exercises is
// record atomicity under overflow: the flushed file must parse into whole records with
// contiguous sequence numbers, and decoded + dropped must equal attempted (both counts
// printed by kickos_trace_report_counters at shutdown).

#include <kickos/kos.h>

namespace
{
    // Enough to overflow any reasonable ch1 ring: each yield emits 2 records
    // (SYSCALL_ENTER + SYSCALL_EXIT), 11 bytes each, so 20000 yields is ~430 KiB.
    constexpr int YIELDS = 20000;
}

int main(int, char**)
{
    kos::print("tele_flood: overflowing the ch1 ring\n");
    for (int i = 0; i < YIELDS; i++)
    {
        // With no other ready thread this returns without a switch, but it still traps,
        // so every iteration costs one ENTER/EXIT pair and nothing depends on timing.
        kos::yield();
    }
    kos::print("tele_flood: done\n");
    return 0;
}
