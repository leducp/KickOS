// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The payload of sim_multi_instance, which runs fifty kernels in one process: file-scope state
// here would be one copy shared by every instance.
//
// A worker exits while main is alive, then main exits with a child parked, which ends the system
// with EXIT_CODE.

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
