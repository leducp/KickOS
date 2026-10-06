// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Two regressions on the thread-exit path, in one boot of the default system.
//
// 1. A NON-LAST thread that exits must not panic. A spawned worker runs briefly then
// RETURNS (thread exit) while main is still alive. On an arch that defers the context
// switch (ARM PendSV), the switch away from the exiting thread can only fire once
// exit_current releases its crit section; taking it any earlier reaches
// KICKOS_UNREACHABLE ("an EXITED thread was picked to run").
//
// 2. MAIN's exit ends the SYSTEM, not just main's thread. Main spawns a child that never
// exits, then exits itself: its exit ends its task, which stops the child, and the task the
// default composition `ends` on ending ends the system through the init. So the witness is
// the exit STATUS arriving at all, and EXIT_CODE is nonzero because a shutdown that dropped
// the status would still exit 0.
//
// kos_exit, not exit(): exit() and abort() reach that same KOS_SYS_EXIT on every port.

#include <kickos/kos.h>

namespace
{
    constexpr int EXIT_CODE = 7;

    void worker(void*)
    {
        kos::print("worker: running\n");
        kos::print("worker: exiting\n");
    }

    // Never exits: an image that hangs here is the regression, not a stuck test.
    void parked(void*)
    {
        while (true)
        {
            kos::sleep_ns(1000000000ull);
        }
    }
}

int main(int, char**)
{
    kos::print("KickOS sched-exit regression\n");
    kos::thread::create(worker, nullptr, "worker", 10);
    kos::sleep_ns(300000000ull); // 0.3s: main blocks here -> worker runs + exits
    kos::print("main: survived worker exit\n");
    auto parked_thread = kos::thread::create(parked, nullptr, "parked", 10);
    if (not parked_thread.valid())
    {
        // The marker separates this from a run where main was simply the last thread
        // out, which ends the system for an unrelated reason.
        kos::print("parked spawn refused\n");
        return 0;
    }
    kos::print("main: exiting with a child alive\n");
    kos::exit(EXIT_CODE);
}
