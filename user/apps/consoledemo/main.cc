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

#if !CONSOLEDEMO_SCRAMBLE_TEST
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
#else
    // --- Scramble-then-panic reclaim test (design D-test 3) ------------------------
    // U0C0 window (same base+size the driver is granted). PMSA gives the scrambler its
    // own copy of this DEV grant, so it reaches the shared physical registers.
    constexpr uintptr_t U0C0_BASE = 0x40030000u;
    constexpr uint32_t U0C0_WINDOW = 0x200u;
    // Per-channel offsets (RM Table 18-20), enough to reproduce the hostile state.
    constexpr uintptr_t OFF_KSCFG = 0x00Cu;
    constexpr uintptr_t OFF_FDR = 0x010u;
    constexpr uintptr_t OFF_BRG = 0x014u;
    constexpr uintptr_t OFF_SCTR = 0x034u;
    constexpr uintptr_t OFF_TCSR = 0x038u;
    constexpr uintptr_t OFF_PCR = 0x03Cu;
    constexpr uintptr_t OFF_CCR = 0x040u;
    // KSCFG.BPMODEN(1) is the write-enable for MODEN(0); writing BPMODEN=1,MODEN=0
    // GATES the channel kernel clock -- the true XMC silent-loss write (RM p.18-165).
    constexpr uint32_t KSCFG_BPMODEN_ONLY = 1u << 1;

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    // Unprivileged, granted only the U0C0 window (+ its own stack). It (1) garbles the
    // in-window registers -- baud/mode first, then GATES the clock (KSCFG.MODEN=0) LAST
    // so the earlier writes land and the channel is left fully dead; (2) logs a marker
    // via kos_kconsole_write (RTT / kernel debug path -- NOT the dark chip path, NOT
    // stdio) so the scramble is provably ordered BEFORE the fault; (3) writes one word
    // PAST its granted window (U0C1 base, un-granted) to force an MPU fault ->
    // kickos_isr_fault -> kpanic_enter -> arch_console_reclaim -> polled banner. Step 3
    // strictly follows steps 1-2 in this single straight-line thread.
    void scrambler(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);

        r32(win + OFF_FDR) = 0;   // kill the fractional divider (dead baud)
        r32(win + OFF_BRG) = 0;   // kill the bit-time dividers
        r32(win + OFF_SCTR) = 0;  // wreck word/frame length + shift control
        r32(win + OFF_TCSR) = 0;  // drop the transmit-data-valid trigger
        r32(win + OFF_PCR) = 0;   // wreck the ASC protocol config
        r32(win + OFF_CCR) = 0;   // disable the channel (MODE=0)
        r32(win + OFF_KSCFG) = KSCFG_BPMODEN_ONLY; // GATE the clock LAST (silent loss)

        kos::print("[scramble] U0C0 garbled (KSCFG.MODEN=0, clock gated); forcing MPU fault\n");

        // Wild write one word past the granted window -> U0C1 (0x4003_0200), un-granted
        // -> MPU fault (the B2 path: the likely real faulter is the console driver).
        r32(win + U0C0_WINDOW) = 0;

        kos::print("[scramble] ERROR: wild write did not fault (MPU not enforcing?)\n");
        kos_exit(1);
    }
#endif
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
#if CONSOLEDEMO_SCRAMBLE_TEST
        // Scramble-then-panic test: handover is live (USER_OWNED, driver up). Give the
        // driver a beat to emit its first-light banner, then spawn the scrambler that
        // garbles the UART and faults. Scrambler prio > driver so it runs promptly.
        kos_sleep_ns(200000000ull); // 200 ms
        int const s = kos::thread::spawn(
            scrambler, reinterpret_cast<void*>(U0C0_BASE), "scrambler",
            /*prio=*/DRIVER_PRIO + 2, KOS_POLICY_FIFO, /*quantum_ns=*/0,
            /*privileged=*/false, /*mem=*/nullptr, /*mem_size=*/0,
            /*stack=*/nullptr, /*stack_size=*/0,
            /*mmio=*/reinterpret_cast<void*>(U0C0_BASE), U0C0_WINDOW);
        if (s < 0)
        {
            kos::print("[consoledemo] ERROR: scrambler spawn failed\n");
        }
#else
        // 5: spawn the printing worker AFTER a confirmed driver (so its index-0 cap
        // is seated to the published endpoint by cap_install_defaults).
        int const w = kos::thread::spawn(worker, nullptr, "worker", WORKER_PRIO);
        if (w < 0)
        {
            kos::print("[consoledemo] ERROR: worker spawn failed\n");
        }
#endif
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
