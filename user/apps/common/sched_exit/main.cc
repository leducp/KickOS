// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Regression: a NON-LAST thread that exits must not panic. A spawned worker runs
// briefly then RETURNS (thread exit) while root is still alive. On an arch that
// defers the context switch (ARM PendSV), the switch away from the exiting thread
// can only fire once exit_current releases its crit section -- the bug this guards
// against ran KICKOS_UNREACHABLE first ("an EXITED thread was picked to run"). Root
// then wakes and prints the survival marker; root's own return is the last-thread
// exit -> clean shutdown. (The sim always worked, arch_switch being synchronous;
// this locks parity across sim/qemu/microbit.)

#include <kickos/kos.h>

namespace
{
    void worker(void*)
    {
        kos::print("worker: running\n");
        kos::print("worker: exiting\n");
        // return -> thread exit while root is still alive (the non-last case)
    }
}

int main(int, char**)
{
    kos::print("KickOS sched-exit regression\n");
    kos::thread::spawn(worker, nullptr, "worker", 10);
    kos::sleep_ns(300000000ull); // 0.3s: root blocks here -> worker runs + exits
    kos::print("root: survived worker exit\n");
    return 0; // last non-idle thread out -> clean shutdown
}
