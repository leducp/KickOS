// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Two regressions on the thread-exit path, in one boot.
//
// 1. A NON-LAST thread that exits must not panic. A spawned worker runs briefly then
// RETURNS (thread exit) while root is still alive. On an arch that defers the context
// switch (ARM PendSV), the switch away from the exiting thread can only fire once
// exit_current releases its crit section; the bug this guards against ran
// KICKOS_UNREACHABLE first ("an EXITED thread was picked to run"). (The sim always
// worked, arch_switch being synchronous; this locks parity across sim/qemu/microbit.)
//
// 2. ROOT's exit ends the SYSTEM, not just root's thread. Root spawns a child that never
// exits, then exits itself: with root's exit treated as an ordinary thread exit,
// kernel().live never reaches 0, the system runs on with a dead init, and the image hangs
// until the harness times out. So the witness is the exit STATUS arriving at all, and
// EXIT_CODE is nonzero because a shutdown that dropped the status would still exit 0.
//
// kos_exit, not exit(): the sim does not compile newlib_stubs.cc, so its exit() is the host
// libc's and reaches no syscall, and newlib's exit() does not LINK on the freestanding
// ports at all (it pulls __libc_fini_array, and no linker script here defines _fini). What
// does reach this call through newlib_stubs.cc's _exit is abort(), and so a failed assert.

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
    // Reuses the worker's reclaimed slot on a two-slot pool, so this needs one
    // concurrent child and runs on every board.
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
