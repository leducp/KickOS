// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Console bring-up demo: its composition names the packaged xmcuartirq as stdout, so the init
// hands USIC0 channel 0 to that unprivileged driver, and main and a worker print through it. The
// wire shows the kernel boot banner, "[xmcuartirq] device up" and a constructor's line in either
// order, then the numbered lines through the userspace driver.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <stdio.h>

#if !KICKOS_HAVE_MPU
#error "consoledemo requires enforcement: build the board's base variant, not its flat one"
#endif

namespace
{
    // Runs in main's entry thread before main, with the driver's bring-up before or after it.
    // _write classifies stdout per thread: a process-wide probe cached here would poison the
    // worker's printfs.
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
    // main's stdout is the driver's endpoint, served once the driver is up.
    printf("[main] post-publish line via the userspace driver\n");
    fflush(stdout);
    auto const w = kos::thread::create(worker, nullptr, "worker", WORKER_PRIO);
    if (not w.valid())
    {
        printf("[consoledemo] ERROR: worker spawn refused, errno %d\n", -w.error());
        return 1;
    }
    (void)w.join();
    return 0;
}
