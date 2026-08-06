// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Console bring-up demo: the DEFAULT INIT relinquishes the XMC4800 UART to an
// UNPRIVILEGED userspace driver BEFORE this app's main runs, so a normal worker's
// printf() output reaches the wire THROUGH that driver with zero app code doing the
// handover.
//
// Flow:
//   * Before main: a global constructor emits one line via printf. Constructors run
//     in the root thread BEFORE the init bring-up publishes, so root's cap 0 is still
//     empty; _write's kos_send(0) fails and falls back to the kernel console path.
//     That line is the kernel-path proof (and exercises the per-thread _write reprobe:
//     a stale process-wide probe would have poisoned the worker's later output).
//   * The default init walks this board's service list, whose first KOS_SVC_CONSOLE entry
//     runs xmcuart_console_start(): create endpoint E, kos_console_publish(E) (kernel
//     UART goes dark, stdout routes to E, and the publisher's own cap 0 is seated),
//     spawn the unprivileged driver granted the USIC0 CH0 window + {E | WAIT}, close
//     root's own WAIT cap. Only if that succeeds is this main entered.
//   * main spawns a normal unprivileged worker that printf()s. Its libc _write
//     self-sends to cap index 0 (seated to E by cap_install_defaults because the
//     worker is spawned AFTER publish), rendezvous-delivered to the driver, then
//     poll-written to the UART. No knowledge of endpoints/drivers/MMIO in the worker.
//
// Observable on silicon (XMC has no QEMU model, and a live publish silences the sim
// TAP, so this is a BUILD-ONLY pass here): the kernel boot banner (kernel-owned UART),
// the "[init] pre-publish ctor line" (kernel path), THEN "[xmcuart] driver up" + the
// worker's numbered lines, all emerging via the userspace driver.
//
// Requires enforcement (the board's base variant): on PMSA the granted U0C0 window is a
// real per-thread capability, so the driver genuinely owns the device and SCU/IOCR
// stay privileged. Without it the isolation the handover relies on is a no-op.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <stdio.h>

#if !KICKOS_HAVE_MPU
#error "consoledemo requires enforcement: build the board's base variant, not its flat one"
#endif

namespace
{
    // Pre-publish poison probe (silicon repro), re-expressed as a global ctor so
    // it runs BEFORE the init bring-up publishes (main now runs post-publish). cap 0 is
    // empty here, so this printf falls back to the kernel console path; the worker's
    // later printfs still reach xmcuart because _write reclassifies per thread.
    __attribute__((constructor)) void prepublish_ctor()
    {
        printf("[init] pre-publish ctor line\n");
        fflush(stdout);
    }

    constexpr uint8_t WORKER_PRIO = 10;

    // Unprivileged worker: an ordinary app that just prints. printf -> _write ->
    // kos_send(cap 0) -> the console endpoint -> the userspace driver -> the UART.
    // No knowledge of endpoints/drivers/MMIO, which is the whole point of the bring-up.
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
    // The default init already published + spawned the driver before this main was
    // entered (a bring-up failure would have aborted the app), so the console is live
    // and every thread spawned here gets its cap 0 seated to the endpoint. No handover
    // call here: doing one would DOUBLE publish (a second endpoint + a second driver,
    // both granted U0C0, the first driver parked forever).
    // Decision-5 proof: this main runs in the ROOT thread, which init entered AFTER the
    // publish, and the publish syscall seated ROOT's own cap 0 to the endpoint. So this
    // printf from root reaches the wire THROUGH the userspace driver, with no child thread.
    printf("[root] post-publish line via the userspace driver\n");
    fflush(stdout);
    // Spawn the printing worker: its index-0 cap is seated to the published endpoint
    // by cap_install_defaults (it is spawned after the init's publish).
    auto const w = kos::thread::spawn(worker, nullptr, "worker", WORKER_PRIO);
    if (not w.valid())
    {
        char e[64];
        snprintf(e, sizeof(e), "[consoledemo] worker spawn refused, errno %d", -w.error());
        kos_panic(e);
    }

    // Park: keep the app alive (root exiting would tear the system down). Fall back to a
    // sleep park if the semaphore could not be created, else an unmintable handle hot-loops
    // sem_wait.
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
