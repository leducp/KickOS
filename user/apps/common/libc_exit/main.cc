// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The C library's exit() and abort() in a worker, end to end, on whatever libc the port carries.
//
// Either one ends the worker's whole task, main with it, so main printing past the worker is
// the failure. exit() runs the handler main registered before the spawn and flushes the text the
// worker left in stdout's buffer, both in the worker, and the task ends with the worker's code.
// The worker gives stdout a buffer of its own: a board whose heap cannot hold newlib's stdout
// buffer leaves stdout unbuffered, and the text would reach the wire before the exit.
// abort() (KICKOS_LIBC_EXIT_ABORT) runs no handler and flushes nothing, and the task ends with
// 128 + SIGABRT. Either way the default composition ends the system with main's task's status.

#include <kickos/kos.h>

#include <stdio.h>
#include <stdlib.h>

namespace
{
    constexpr int EXIT_CODE = 7;
    constexpr int WORKER_CODE = 3;

    char stdout_buffer[64];

    void worker(void*)
    {
        setvbuf(stdout, stdout_buffer, _IOLBF, sizeof(stdout_buffer));
        fputs("worker: buffered", stdout);
#ifdef KICKOS_LIBC_EXIT_ABORT
        kos::print("worker: abort()\n");
        abort();
#else
        kos::print("worker: exit()\n");
        exit(WORKER_CODE);
#endif
    }

    void main_at_exit()
    {
        kos::print("main: atexit handler\n");
    }
}

int main(int, char**)
{
    kos::print("KickOS libc exit() regression\n");
    atexit(main_at_exit);
    auto exiter = kos::thread::create(worker, nullptr, "exiter", 10);
    if (not exiter.valid())
    {
        // The marker separates a refused spawn from a worker that exited.
        kos::print("worker spawn refused\n");
        return 0;
    }
    kos::sleep_ns(300000000ull); // main blocks, so the worker runs and exits first
    kos::print("main: survived the worker\n");
    exit(EXIT_CODE);
}
