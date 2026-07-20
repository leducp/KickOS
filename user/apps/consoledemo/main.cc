// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Console handover demo (M3 #4 stage ii-a): the kernel relinquishes the XMC4800
// UART to an UNPRIVILEGED userspace driver, and a normal worker's printf() output
// then reaches the wire THROUGH that driver -- not the kernel chip path.
//
// Flow (privileged main; see the design note D3/D7/D8/D9 + lifecycle section):
//   1-4. xmcuart_console_start(): create endpoint E, kos_console_publish(E)
//        (kernel UART goes dark, stdout routes to E), spawn the unprivileged
//        driver granted the USIC0 CH0 window + {E | WAIT}, then CLOSE root's own
//        WAIT cap on E (S4) so driver death can EPIPE clients instead of hanging
//        them. driver_prio (12) >= the worker prio (10) per D9 (no PI on rendezvous).
//   5.   spawn a normal unprivileged worker that printf()s -- its libc _write
//        self-sends to cap index 0 (seated to E by cap_install_defaults at spawn,
//        because it is spawned AFTER publish), rendezvous-delivered to the driver,
//        poll-written to the UART.
//
// Observable on silicon (next pass -- XMC has no QEMU model, and a live publish
// silences the sim TAP, so this pass is BUILD-ONLY): the kernel boot banner
// (kernel-owned UART) THEN "[xmcuart] driver up" + the worker's numbered lines,
// all emerging via the userspace driver. Kernel kprintf after publish goes to RTT
// or nowhere on the wire (accepted M3 cost, design Q2).
//
// Requires enforcement (-DKICKOS_HAVE_MPU=1): on PMSA the granted U0C0 window is a
// real per-thread capability, so the driver genuinely owns the device and SCU/IOCR
// stay privileged. Without it the isolation the handover relies on is a no-op.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/xmcuart.h>

#include <stdio.h>

#if !KICKOS_HAVE_MPU
#error "consoledemo requires enforcement: configure with -DKICKOS_HAVE_MPU=1"
#endif

namespace
{
    constexpr uint8_t DRIVER_PRIO = 12; // >= WORKER_PRIO (D9)
    constexpr uint8_t WORKER_PRIO = 10;

    // Unprivileged worker: an ordinary app that just prints. printf -> _write ->
    // kos_send(cap 0) -> the console endpoint -> the userspace driver -> the UART.
    // No knowledge of endpoints/drivers/MMIO -- the whole point of the handover.
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
    // 1-4: publish + spawn the driver + drop root's WAIT cap (all inside the helper).
    if (xmcuart_console_start(DRIVER_PRIO) != 0)
    {
        // Publish+spawn are inseparable (S6): if the driver did not come up, do NOT
        // spawn console-dependent apps (they would park forever on their first printf).
        kos::print("[consoledemo] handover bring-up FAILED; not spawning worker\n");
    }
    else
    {
        // 5: spawn the printing worker AFTER a confirmed driver (so its index-0 cap
        // is seated to the published endpoint by cap_install_defaults).
        int const w = kos::thread::spawn(worker, nullptr, "worker", WORKER_PRIO);
        if (w < 0)
        {
            kos::print("[consoledemo] ERROR: worker spawn failed\n");
        }
    }

    // Park: keep the app (and thus the driver + endpoint) alive. Fall back to a sleep
    // park if the semaphore could not be created (else a -1 handle hot-loops sem_wait).
    int const idle = kos_sem_create(0);
    for (;;)
    {
        if (idle < 0)
        {
            kos_sleep_ns(1000000000ull);
            continue;
        }
        kos_sem_wait(idle);
    }
}
