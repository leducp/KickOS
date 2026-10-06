// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The C library's own exit(), end to end, on whatever libc the port carries.
//
// The WORKER's exit() proves the call reached KOS_SYS_EXIT: that dispatch ends only the
// calling thread unless the caller is its task's entry, so main printing past it is the
// witness. Main's own exit() then ends its task, and the system with it, after running the
// handler it registered: newlib keeps one handler list for the whole image and the worker's
// exit() consumes every entry already on it, so main registers only after the worker has
// exited, just before its own exit().

#include <kickos/kos.h>

#include <stdlib.h>

namespace
{
    constexpr int EXIT_CODE = 7;
    constexpr int WORKER_CODE = 3;

    void worker(void*)
    {
        kos::print("worker: exit()\n");
        exit(WORKER_CODE);
    }

    void main_at_exit()
    {
        kos::print("main: atexit handler\n");
    }
}

int main(int, char**)
{
    kos::print("KickOS libc exit() regression\n");
    auto exiter = kos::thread::create(worker, nullptr, "exiter", 10);
    if (not exiter.valid())
    {
        // The marker separates a refused spawn from a worker that exited.
        kos::print("worker spawn refused\n");
        return 0;
    }
    kos::sleep_ns(300000000ull); // main blocks, so the worker runs and exits first
    kos::print("main: survived worker exit()\n");
    kos::print("main: exit()\n");
    atexit(main_at_exit);
    exit(EXIT_CODE);
}
