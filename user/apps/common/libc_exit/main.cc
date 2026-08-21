// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The C library's own exit(), end to end, on whatever libc the port carries.
//
// The WORKER's exit() proves the call reached KOS_SYS_EXIT: that dispatch ends only the
// calling thread unless the caller is root, so root printing past it is the witness.

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
}

int main(int, char**)
{
    kos::print("KickOS libc exit() regression\n");
    auto exiter = kos::thread::spawn(worker, nullptr, "exiter", 10);
    if (not exiter.valid())
    {
        // The marker separates a refused spawn from a worker that exited.
        kos::print("worker spawn refused\n");
        return 0;
    }
    kos::sleep_ns(300000000ull); // root blocks, so the worker runs and exits first
    kos::print("root: survived worker exit()\n");
    kos::print("root: exit()\n");
    exit(EXIT_CODE);
}
