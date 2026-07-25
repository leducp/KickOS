// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F console bring-up demo (M4.3): the DEFAULT INIT relinquishes the UART0 OpenSDA
// VCOM to an UNPRIVILEGED userspace driver (kickos_k64uart) BEFORE this app's main
// runs, so a normal worker's printf() output reaches the wire THROUGH that driver with
// zero app code doing the handover.
//
// Honesty (coarse-AIPS ceiling): unlike the XMC PMSA reference, the handover here is
// FUNCTIONAL + RECLAIM-PROOF, NOT per-thread peripheral ownership. AIPS peripheral
// bridges are not SYSMPU slave ports (chip_mk64f.cc, the PIT-ceiling note ~163-167:
// per-AIPS-slot protection is the accepted K64F ceiling), so once the bring-up opens
// UART0's AIPS PACR the peripheral is reachable by EVERY unprivileged thread; the
// per-thread SYSMPU window grant is inert for it. What SYSMPU DOES still enforce is
// MEMORY isolation (each thread's stack/data), and that is exactly what the scramble
// test faults against to trigger the panic-path console reclaim. That is why the
// KICKOS_HAVE_MPU gate stays: without SYSMPU the memory-fault trigger cannot fire.
//
// Flow:
//   * Before main: a global constructor emits one line via printf. Constructors run
//     in the root thread BEFORE the init bring-up publishes, so root's cap 0 is still
//     empty; _write's kos_send(0) fails and falls back to the kernel console path.
//     That line is the kernel-path proof (and exercises the per-thread _write reprobe:
//     a stale process-wide probe would have poisoned the worker's later output).
//   * The default init walks this board's service list (KICKOS_SERVICE_LIST=
//     kickos_services_frdmk64f), whose first KOS_SVC_CONSOLE entry
//     runs k64uart_console_start(): create endpoint E, kos_console_publish(E) (kernel
//     UART goes dark, stdout routes to E, and the publisher's own cap 0 is seated),
//     open UART0's AIPS slot, spawn the unprivileged driver granted the UART0 window +
//     {E | WAIT}, close root's own WAIT cap. Only if that succeeds is this main entered.
//   * main spawns a normal unprivileged worker that printf()s. Its libc _write
//     self-sends to cap index 0 (seated to E by cap_install_defaults because the
//     worker is spawned AFTER publish), rendezvous-delivered to the driver, then
//     poll-written to UART0. No knowledge of endpoints/drivers/MMIO in the worker.
//
// Requires enforcement (-DKICKOS_HAVE_MPU=1): SYSMPU memory isolation is what the
// scramble test's ungranted-memory write faults against to trigger the reclaim path.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <stdio.h>

#if !KICKOS_HAVE_MPU
#error "k64console requires enforcement: configure with -DKICKOS_HAVE_MPU=1"
#endif

namespace
{
    // Pre-publish poison probe (M4.3 silicon repro), re-expressed as a global ctor so
    // it runs BEFORE the init bring-up publishes (main now runs post-publish). cap 0 is
    // empty here, so this printf falls back to the kernel console path; the worker's
    // later printfs still reach k64uart because _write reclassifies per thread.
    __attribute__((constructor)) void prepublish_ctor()
    {
        printf("[init] pre-publish ctor line\n");
        fflush(stdout);
    }

#if !K64CONSOLE_SCRAMBLE_TEST
    constexpr uint8_t WORKER_PRIO = 10;

    // Unprivileged worker: an ordinary app that just prints. printf -> _write ->
    // kos_send(cap 0) -> the console endpoint -> the userspace driver -> UART0. No
    // knowledge of endpoints/drivers/MMIO, which is the whole point of the handover.
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

    // Unprivileged, granted NO MMIO window at all. It reaches UART0 anyway through the
    // AIPS slot the bring-up opened, which IS the coarse-AIPS point being shown: a
    // peripheral window grant would not have contained it. `arg` is an SRAM cell root
    // allocated but did NOT grant to this thread (the fault target). It (1) garbles
    // UART0 to the silent-loss state via byte writes; (2) logs a marker via kos::print
    // (RTT / kernel debug path, NOT the dark chip path, NOT stdio) so the scramble is
    // provably ordered before the fault; (3) provokes a fault via that UN-GRANTED
    // MEMORY write. Step 3 strictly follows steps 1-2 in this straight-line thread.
    void scrambler(void* arg)
    {
        uintptr_t const uart = UART0_BASE;
        r8(uart + OFF_MODEM) = MODEM_TXCTSE; // absent-CTS silent loss (the king)
        r8(uart + OFF_BDH) = 0xFFu;          // garble baud high
        r8(uart + OFF_BDL) = 0xFFu;          // garble baud low
        r8(uart + OFF_C3) = C3_TXINV;        // invert the TX line
        r8(uart + OFF_C2) = 0;               // TX/RX off LAST -> UART fully dead

        kos::print("[scramble] UART0 garbled (TXCTSE/baud/TXINV, TX off); forcing memory fault\n");

        // Fault via an UN-GRANTED MEMORY write (the mpu_fault mechanism). A past-the-
        // window PERIPHERAL write does NOT fault on coarse-AIPS (peripherals are not
        // SYSMPU slave ports), so the trigger MUST be memory. `arg` points into an SRAM
        // region root allocated but never granted to this thread, so this store raises a
        // SYSMPU protection error -> bus/HardFault (MMFSR=0 on K64F) ->
        // kickos_armv7m_fault_report -> kpanic_enter -> arch_console_reclaim (the real
        // K64F reclaim body runs, undoing the garble) -> the fault report prints THROUGH
        // the reclaimed UART. Expected on-wire banner: "=== HARD FAULT ===" plus the
        // "SYSMPU ISOLATION FAULT" capture line (NOT "MPU FAULT": that label needs a core
        // MMFSR byte, which K64F does not set for a bus-side SYSMPU trap).
        volatile int* bad = static_cast<volatile int*>(arg);
        *bad = 0xDEAD;

        kos::print("[scramble] ERROR: memory write did not fault (SYSMPU not enforcing?)\n");
        kos_exit(1);
    }
#endif
}

int main(int, char**)
{
    // The default init already published + spawned the driver before this main was
    // entered (a bring-up failure would have aborted the app), so the console is live
    // and every thread spawned here gets its cap 0 seated to the endpoint. No handover
    // call here: doing one would DOUBLE publish (a second endpoint + a second driver,
    // both granted UART0, the first driver parked forever).
#if K64CONSOLE_SCRAMBLE_TEST
    // Scramble-then-panic test: the driver is up. Allocate an SRAM cell that we do NOT
    // grant to the scrambler; its write there faults (the mpu_fault mechanism). Give the
    // driver a beat to emit its first-light banner, then spawn the scrambler (NO MMIO
    // grant, NO memory grant) that garbles UART0 and faults.
    void* const fault_target = kos_ram_alloc(4096);
    if (fault_target == nullptr)
    {
        kos::print("[k64console] ERROR: fault-target ram_alloc failed\n");
    }
    kos_sleep_ns(200000000ull); // 200 ms
    int const s = kos::thread::spawn(
        scrambler, fault_target, "scrambler",
        /*prio=*/SCRAMBLER_PRIO, KOS_POLICY_FIFO, /*quantum_ns=*/0,
        /*privileged=*/false);
    if (s < 0)
    {
        kos::print("[k64console] ERROR: scrambler spawn failed\n");
    }
#else
    // Decision-5 proof: this main runs in the ROOT thread, which init entered AFTER the
    // publish, and the publish syscall seated ROOT's own cap 0 to the endpoint. So this
    // printf from root reaches the wire THROUGH the userspace driver, with no child thread.
    printf("[root] post-publish line via the userspace driver\n");
    fflush(stdout);
    // Spawn the printing worker: its index-0 cap is seated to the published endpoint
    // by cap_install_defaults (it is spawned after the init's publish).
    int const w = kos::thread::spawn(worker, nullptr, "worker", WORKER_PRIO);
    if (w < 0)
    {
        kos::print("[k64console] ERROR: worker spawn failed\n");
    }
#endif

    // Park: keep the app alive (root exiting would tear the system down). Fall back to a
    // sleep park if the semaphore could not be created (else a -1 handle hot-loops sem_wait).
    int const idle = kos_sem_create(0);
    while (true)
    {
        if (idle < 0)
        {
            kos_sleep_ns(1000000000ull);
            continue;
        }
        kos_sem_wait(idle);
    }
}
