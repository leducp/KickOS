// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Console bring-up demo: the DEFAULT INIT relinquishes the XMC4800 UART to an
// UNPRIVILEGED userspace driver BEFORE this app's main runs, so a normal worker's
// printf() output reaches the wire THROUGH that driver with zero app code doing the
// handover.
//
// The wire ORDER is the claim: the kernel boot banner and the pre-publish ctor line come
// out of the kernel-owned UART, then "[xmcuart] driver up" and the worker's numbered lines
// come out through the userspace driver.
//
// Requires enforcement (the board's base variant): on PMSA the granted U0C0 window is a
// real per-thread capability, so the driver owns the device while SCU/IOCR stay privileged.

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
        kos_exit(0);
    }
}

int main(int, char**)
{
    // The default init published and spawned the driver before entering main, so a handover
    // call here would DOUBLE publish: a second endpoint and a second driver, both granted
    // U0C0, with the first driver parked forever.
    // Decision-5 proof: root is the thread init entered AFTER the publish, and the publish
    // syscall seated ROOT's own cap 0, so this printf reaches the wire through the userspace
    // driver with no child thread involved.
    printf("[root] post-publish line via the userspace driver\n");
    fflush(stdout);
    // Spawned after the init's publish, so cap_install_defaults seats this worker's index-0
    // cap to the published endpoint.
    auto const w = kos::thread::create(worker, nullptr, "worker", WORKER_PRIO);
    if (not w.valid())
    {
        char e[64];
        snprintf(e, sizeof(e), "[consoledemo] worker spawn refused, errno %d", -w.error());
        kos_panic(e);
    }

    // Park: root exiting would tear the system down. The sleep fallback covers a semaphore
    // that could not be created, since an unmintable handle would hot-loop sem_wait.
    kos_cap_t idle = KOS_CAP_NONE;
    (void)kos_sem_create(0, &idle);
    while (true)
    {
        if (idle == KOS_CAP_NONE)
        {
            kos_sleep_ns(1000000000ull);
            continue;
        }
        kos_sem_wait(idle);
    }
}
