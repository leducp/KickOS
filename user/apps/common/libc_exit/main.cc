// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The C library's own exit(), end to end, on whatever libc the port carries.
//
// The WORKER's exit() is what proves the call reached KOS_SYS_EXIT rather than merely
// ending the image: that dispatch ends only the calling thread unless the caller is
// root, so root printing past it cannot be produced by a libc exit of its own.

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
        // Without the marker the gate cannot tell this run from one whose worker exited.
        kos::print("worker spawn refused\n");
        return 0;
    }
    kos::sleep_ns(300000000ull); // root blocks here -> the worker runs and exits
    kos::print("root: survived worker exit()\n");
    kos::print("root: exit()\n");
    exit(EXIT_CODE);
}
