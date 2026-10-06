// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Console bring-up demo: its composition names the packaged xmcuart as stdout, so the init hands
// USIC0 channel 0 to that unprivileged driver before main runs, and main and a worker print
// through it. The wire shows the kernel boot banner and the pre-publish
// constructor line from the kernel-owned UART, then "[xmcuart] driver up" and the numbered lines
// through the userspace driver.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <stdio.h>

#if !KICKOS_HAVE_MPU
#error "consoledemo requires enforcement: build the board's base variant, not its flat one"
#endif

namespace
{
    // A ctor runs before the init bring-up publishes, so cap 0 is still empty and this
    // printf takes the kernel console path. The worker's later printfs still reach xmcuart
    // because _write reclassifies per thread; a process-wide probe cached here would
    // poison them.
    __attribute__((constructor)) void prepublish_ctor()
    {
        printf("[init] pre-publish ctor line\n");
        fflush(stdout);
    }

    constexpr uint8_t WORKER_PRIO = 10;

    void worker(void*)
    {
        for (int i = 0; i < 5; i++)
        {
            printf("[worker] line %d via the userspace console driver\n", i);
            fflush(stdout); // newlib line-buffers a non-tty; flush so each line ships now
            kos_sleep_ns(100000000ull); // 100 ms, so the lines are visibly paced on the wire
        }
        printf("[worker] done\n");
        fflush(stdout);
    }
}

int main(int, char**)
{
    // The init published and spawned the driver before main, so main's own stdout is already the
    // endpoint and this line reaches the wire through the userspace driver.
    printf("[main] post-publish line via the userspace driver\n");
    fflush(stdout);
    auto const w = kos::thread::create(worker, nullptr, "worker", WORKER_PRIO);
    if (not w.valid())
    {
        printf("[consoledemo] worker spawn refused, errno %d\n", -w.error());
        return 1;
    }
    (void)w.join();
    return 0;
}
