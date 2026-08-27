// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F console bring-up demo: the DEFAULT INIT relinquishes the UART0 OpenSDA
// VCOM to an UNPRIVILEGED userspace driver (kickos_k64uart) BEFORE this app's main
// runs, so a normal worker's printf() output reaches the wire THROUGH that driver with
// zero app code doing the handover.
//
// The handover is FUNCTIONAL and RECLAIM-PROOF. This app's window grant is one of the
// three inert ones (docs/reference/boards.md, "When an MMIO grant is INERT"); what
// SYSMPU enforces is MEMORY isolation (each thread's stack/data), which is what confines
// the unprivileged driver that owns the published console.
//
// Flow:
//   * Before main: a global constructor emits one line via printf. Constructors run in
//     the root thread BEFORE the init bring-up publishes, so root's cap 0 is still
//     empty; _write's kos_send(0) fails and falls back to the kernel console path. That
//     line is the kernel-path proof, and it exercises the per-thread _write reprobe that
//     keeps the worker's later output on the driver.
//   * The default init walks this board's service list (KICKOS_SERVICE_LIST=
//     kickos_services_frdmk64f), whose first KOS_SVC_CONSOLE entry runs
//     k64uart_console_start(): create endpoint E, kos_console_publish(E) (kernel UART
//     goes dark, stdout routes to E, and the publisher's own cap 0 is seated), open
//     UART0's AIPS slot, spawn the unprivileged driver granted the UART0 window +
//     {E | WAIT}, close root's own WAIT cap. Only if that succeeds is this main entered.
//   * main spawns a normal unprivileged worker that printf()s. Its libc _write
//     self-sends to cap index 0 (seated to E by cap_install_defaults because the worker
//     is spawned AFTER publish), rendezvous-delivered to the driver, then poll-written to
//     UART0.
//
// Requires enforcement: the board's base variant.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <stdio.h>

#if !KICKOS_HAVE_MPU
#error "k64console requires enforcement: build the board's base variant, not its flat one"
#endif

namespace
{
    // Runs BEFORE the init bring-up publishes, so cap 0 is still empty and this printf
    // falls back to the kernel console path. The worker's later printfs reach k64uart
    // because _write reclassifies per thread.
    __attribute__((constructor)) void prepublish_ctor()
    {
        printf("[init] pre-publish ctor line\n");
        fflush(stdout);
    }

#if !K64CONSOLE_SCRAMBLE_TEST
    constexpr uint8_t WORKER_PRIO = 10;

    // Unprivileged worker: an ordinary app that just prints. printf -> _write ->
    // kos_send(cap 0) -> the console endpoint -> the userspace driver -> UART0, with no
    // knowledge of endpoints, drivers or MMIO.
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
#else
    // Scramble-then-panic reclaim test. The init already brought the driver up (prio 12,
    // from the board descriptor); the scrambler runs above it so it preempts promptly.
    constexpr uint8_t SCRAMBLER_PRIO = 14;
    // UART0 byte-mapped register offsets (RM ch.52) needed to reproduce a hostile
    // silent-loss state, exactly the registers arch_console_reclaim undoes.
    constexpr uintptr_t UART0_BASE = 0x4006A000u;
    constexpr uintptr_t OFF_BDH = 0x00u;   // baud high (RM 52.3.1)
    constexpr uintptr_t OFF_BDL = 0x01u;   // baud low  (RM 52.3.2)
    constexpr uintptr_t OFF_C2 = 0x03u;    // TX/RX enable (RM 52.3.4)
    constexpr uintptr_t OFF_C3 = 0x06u;    // TXINV (RM 52.3.7)
    constexpr uintptr_t OFF_MODEM = 0x0Du; // TXCTSE (RM 52.3.13)
    constexpr uint8_t MODEM_TXCTSE = 1u << 0; // wait-forever on an absent CTS: silent loss
    constexpr uint8_t C3_TXINV = 1u << 4;     // invert TX line: every framed byte corrupt

    inline volatile uint8_t& r8(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint8_t*>(a);
    }

    // Unprivileged, granted NO MMIO window at all: it reaches UART0 through the AIPS
    // slot the bring-up opened, which is the coarse-AIPS behaviour under test. The marker
    // goes out on kos::print (RTT / kernel debug path, never the dark chip path or
    // stdio), and the panic strictly follows the garble in this straight-line thread.
    void scrambler(void*)
    {
        uintptr_t const uart = UART0_BASE;
        r8(uart + OFF_MODEM) = MODEM_TXCTSE; // absent-CTS silent loss (the king)
        r8(uart + OFF_BDH) = 0xFFu;          // garble baud high
        r8(uart + OFF_BDL) = 0xFFu;          // garble baud low
        r8(uart + OFF_C3) = C3_TXINV;        // invert the TX line
        r8(uart + OFF_C2) = 0;               // TX/RX off LAST -> UART fully dead

        kos::print("[scramble] UART0 garbled (TXCTSE/baud/TXINV, TX off); ending the system\n");

        // THE TERMINAL EVENT MUST BE A PANIC AND NOT A FAULT. An unprivileged thread's
        // fault is answered by kickos_fault_kill_thread before kpanic_enter runs:
        // arch_fault_is_user_thread says yes for an unprivileged thread whose frame is its
        // own, so the thread dies, the system lives, and the thread-kill path deliberately
        // does not reclaim the console (the ruling is stated in kpanic_enter,
        // kernel/init/console.cc). Nothing would then reach the wire: while the console is
        // published the chip path is dropped, and the record routed to the driver is
        // poll-written into the UART this thread just killed. PRIVDEFENA is on, so a
        // privileged thread is no way round it either.
        //
        // kos_panic is the terminal path an unprivileged thread has, and it reaches
        // kpanic_enter through the same call the fault reporter used, so the reclaim body
        // under test is unchanged. Kept well inside user_panic's 64-byte buffer, so the
        // verdict is not truncated.
        kos_panic("[k64console] PASS: the dead channel came back");
    }
#endif
}

int main(int, char**)
{
    // The default init already published and spawned the driver before this main was
    // entered (a bring-up failure would have aborted the app), so the console is live and
    // every thread spawned here gets its cap 0 seated to the endpoint. A handover call
    // here would DOUBLE publish: a second endpoint and a second driver, both granted
    // UART0, with the first driver parked forever.
#if K64CONSOLE_SCRAMBLE_TEST
    // The driver needs a beat to emit its first-light banner before the scrambler, which
    // holds no grant of any kind, garbles UART0 under it.
    kos_sleep_ns(200000000ull); // 200 ms
    auto const s = kos::thread::create(
        scrambler, nullptr, "scrambler",
        /*prio=*/SCRAMBLER_PRIO, KOS_POLICY_FIFO, /*quantum_ns=*/0,
        /*privileged=*/false);
    if (not s.valid())
    {
        kos::print("[k64console] ERROR: scrambler spawn failed\n");
    }
#else
    // Decision-5 proof: this main runs in the ROOT thread, which init entered AFTER the
    // publish, and the publish syscall seated ROOT's own cap 0 to the endpoint. So this
    // printf from root reaches the wire THROUGH the userspace driver, with no child thread.
    printf("[root] post-publish line via the userspace driver\n");
    fflush(stdout);
    // The worker's index-0 cap is seated to the published endpoint by
    // cap_install_defaults, because it is spawned after the init's publish.
    auto const w = kos::thread::create(worker, nullptr, "worker", WORKER_PRIO);
    if (not w.valid())
    {
        kos::print("[k64console] ERROR: worker spawn failed\n");
    }
#endif

    // Park: root exiting would tear the system down. Sleep park when the semaphore could
    // not be created, else an unmintable handle hot-loops sem_wait.
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
