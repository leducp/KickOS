// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Two regressions on the thread-exit path, in one boot.
//
// 1. A NON-LAST thread that exits must not panic. A spawned worker runs briefly then
// RETURNS (thread exit) while root is still alive. On an arch that defers the context
// switch (ARM PendSV), the switch away from the exiting thread can only fire once
// exit_current releases its crit section; the bug this guards against ran
// KICKOS_UNREACHABLE first ("an EXITED thread was picked to run").
//
// 2. WAIT-UNTIL-LAST is the shutdown condition: root parks in kos_wait_last while a child
// is alive and is released by that child's exit, and a child's own kos_wait_last is
// refused -KOS_EPERM because the primitive is root's. It lives HERE and not in the
// selftest suite because the condition is GLOBAL: an image carrying a service list has a
// driver thread that never exits, so kernel().live never reaches 1 and the call never
// returns. SCHED_EXIT_SERVICE_THREADS says which kind of image this is, and the phase is
// skipped rather than hung when it cannot complete.
//
// 3. ROOT's exit ends the SYSTEM, not just root's thread. Root spawns a child that never
// exits, then exits itself: with root's exit treated as an ordinary thread exit,
// kernel().live never reaches 0, the system runs on with a dead init, and the image hangs
// until the harness times out. So the witness is the exit STATUS arriving at all, and
// EXIT_CODE is nonzero because a shutdown that dropped the status would still exit 0.
//
// kos_exit, not exit(): exit() and abort() reach that same KOS_SYS_EXIT on every port,
// and apps/libc_exit is the gate on that route.

#include <kickos/kos.h>

namespace
{
    constexpr int EXIT_CODE = 7;

    void worker(void*)
    {
        kos::print("worker: running\n");
        kos::print("worker: exiting\n");
        // return -> thread exit while root is still alive (the non-last case)
    }

    // A NON-ROOT wait-until-last caller: the primitive reaches outside the caller's own
    // spawn subtree and is single-seat, so it is root's alone. The sleep keeps this behind
    // root's own call, which is what makes the phase below observe a released root rather
    // than a root that was already last.
    void second_waiter(void*)
    {
        kos::sleep_ns(3000000ull);
        if (kos_wait_last() == -KOS_EPERM)
        {
            kos::print("child: wait_last refused\n");
        }
        else
        {
            kos::print("child: wait_last NOT refused\n");
        }
        // return -> this exit takes the live count to 1 and releases root
    }

    // Holds kernel().live above zero for root's exit. Never exits: an image that hangs
    // here is the regression, not a stuck test.
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
    kos::thread::spawn(worker, nullptr, "worker", 10);
    kos::sleep_ns(300000000ull); // 0.3s: root blocks here -> worker runs + exits
    kos::print("root: survived worker exit\n");
    auto second = kos::thread::spawn(second_waiter, nullptr, "wlast", 10);
    if (not second.valid())
    {
        kos::print("wait_last spawn refused\n");
        return 0;
    }
#if SCHED_EXIT_SERVICE_THREADS
    // A driver thread from the service list never exits, so the live count never reaches
    // 1: this park would never be released, and the phases after it would never run. The
    // sleep stands in for the release, giving the refused waiter above its turn.
    kos::print("root: service threads present, wait-until-last skipped\n");
    kos::sleep_ns(300000000ull);
#else
    kos::print("root: waiting for the last thread\n");
    if (kos_wait_last() == 0)
    {
        kos::print("root: last thread standing\n");
    }
    else
    {
        kos::print("root: wait_last unexpectedly refused\n");
    }
#endif // SCHED_EXIT_SERVICE_THREADS
    // Reuses the previous phase's reclaimed slot, as every phase here does: the whole
    // image never holds more than ONE concurrent child, so it runs on a two-slot pool.
    auto parked_thread = kos::thread::spawn(parked, nullptr, "parked", 10);
    if (not parked_thread.valid())
    {
        // Without the marker the gate cannot tell this run from one where root simply
        // was the last thread out, which ends the system for an unrelated reason.
        kos::print("parked spawn refused\n");
        return 0;
    }
    kos::print("root: exiting with a child alive\n");
    kos::exit(EXIT_CODE); // root's exit == kos_shutdown(EXIT_CODE)
}
