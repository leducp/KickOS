// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Console bring-up demo (M4.3): the DEFAULT INIT relinquishes the XMC4800 UART to an
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
//   * The default init (KICKOS_CONSOLE_BRINGUP=kickos_console_xmcuart on this board)
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
// Requires enforcement (-DKICKOS_HAVE_MPU=1): on PMSA the granted U0C0 window is a
// real per-thread capability, so the driver genuinely owns the device and SCU/IOCR
// stay privileged. Without it the isolation the handover relies on is a no-op.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <stdio.h>

#if !KICKOS_HAVE_MPU
#error "consoledemo requires enforcement: configure with -DKICKOS_HAVE_MPU=1"
#endif

namespace
{
    // Pre-publish poison probe (M4.3 silicon repro), re-expressed as a global ctor so
    // it runs BEFORE the init bring-up publishes (main now runs post-publish). cap 0 is
    // empty here, so this printf falls back to the kernel console path; the worker's
    // later printfs still reach xmcuart because _write reclassifies per thread.
    __attribute__((constructor)) void prepublish_ctor()
    {
        printf("[init] pre-publish ctor line\n");
        fflush(stdout);
    }

#if !CONSOLEDEMO_SCRAMBLE_TEST
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
#else
    // --- Scramble-then-panic reclaim test (design D-test 3) ------------------------
    // The init already brought the driver up (prio 12, from the board descriptor); the
    // scrambler runs above it so it preempts promptly.
    constexpr uint8_t SCRAMBLER_PRIO = 14;
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
    // The default init already published + spawned the driver before this main was
    // entered (a bring-up failure would have aborted the app), so the console is live
    // and every thread spawned here gets its cap 0 seated to the endpoint. No handover
    // call here: doing one would DOUBLE publish (a second endpoint + a second driver,
    // both granted U0C0, the first driver parked forever).
#if CONSOLEDEMO_SCRAMBLE_TEST
    // Scramble-then-panic test: the driver is up. Give it a beat to emit its
    // first-light banner, then spawn the scrambler that garbles the UART and faults.
    kos_sleep_ns(200000000ull); // 200 ms
    int const s = kos::thread::spawn(
        scrambler, reinterpret_cast<void*>(U0C0_BASE), "scrambler",
        /*prio=*/SCRAMBLER_PRIO, KOS_POLICY_FIFO, /*quantum_ns=*/0,
        /*privileged=*/false, /*mem=*/nullptr, /*mem_size=*/0,
        /*stack=*/nullptr, /*stack_size=*/0,
        /*mmio=*/reinterpret_cast<void*>(U0C0_BASE), U0C0_WINDOW);
    if (s < 0)
    {
        kos::print("[consoledemo] ERROR: scrambler spawn failed\n");
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
        kos::print("[consoledemo] ERROR: worker spawn failed\n");
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
