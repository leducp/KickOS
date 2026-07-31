// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 console-reclaim test (design D-test 3): an unprivileged thread holding the
// U0C0 DEV grant drives the console UART into the true silent-loss state, then faults.
// PASS is the FAULT DUMP still reaching the wire, which proves arch_console_reclaim
// re-initialised the UART from the panic path.
//
// The expected wire text is "=== MPU FAULT ===" plus the register block, NOT
// "KERNEL PANIC": a MemManage lands in kickos_armv7m_fault_report (the XMC vector table
// routes MemManage to HardFault_Handler), which dumps and calls kfault_terminate
// without going through kickos::kpanic. "KERNEL PANIC" on this app's wire is a FAILURE
// report from one of the kos_panic calls below.
//
// Sequence in one straight-line thread, so step 3 strictly follows steps 1-2:
//   1. garble the in-window registers, GATING the channel kernel clock last
//      (KSCFG.BPMODEN=1 with MODEN=0, RM p.18-165) so the channel is left fully dead;
//   2. log a marker through the kernel debug path, ordering the scramble before the fault.
//      The channel is already dead, so this marker cannot reach the wire until the
//      reclaim, and it comes out of the console ring on the panic flush instead;
//   3. write one word PAST the granted window (U0C1 base, ungranted) -> MemManage ->
//      kickos_armv7m_fault_report -> kpanic_enter -> arch_console_reclaim -> polled dump.
//
// kpanic_enter reclaims from ANY console-ownership state (kernel/init/console.cc): this
// app runs with kickos_services_none, so the console is KERNEL_OWNED, never published,
// and a reclaim gated on USER_OWNED would skip and lose the dump.
//
// U0C0 admits ONE holder (the one-holder-per-window rule in domain_for), so this test
// requires a service list that publishes no userspace console driver: with
// KICKOS_SERVICE_LIST=kickos_services_none the kernel owns the UART, the kernel domain
// carries no DEV region, and this scrambler is the sole holder. The property under test
// does not depend on WHO garbled the UART, so the scrambler stands in for a console
// driver that faults with its device misconfigured.
//
// Register addresses / bit fields are clean-room from the XMC4700/XMC4800 Reference
// Manual (V1.3, 2016-07); no XMCLib/DAVE/CMSIS vendor source. Diagnostic app
// (kickos_add_diagnostic_app): never a production image, validated on silicon.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>

// Without enforcement the granted window is a no-op, nothing faults, and the reclaim
// path is never entered.
#if !KICKOS_HAVE_MPU
#error "conreclaim requires enforcement: configure with -DKICKOS_HAVE_MPU=1"
#endif

namespace
{
    // The kernel console channel. 512 B is pow2 and 0x200-aligned, so PMSA encodes it
    // as one exact-cover descriptor.
    constexpr uintptr_t U0C0_BASE = 0x40030000u;
    constexpr uint32_t U0C0_WINDOW = 0x200u;

    constexpr uint8_t SCRAMBLER_PRIO = 14;

    // Per-channel offsets (RM Table 18-20). FDR (0x010), BRG (0x014) and CCR (0x040)
    // are write-PV-only: an unprivileged store there is silently discarded (measured by
    // user/apps/xmc4800-relax/pvprobe), so they are not written here. The set below is
    // U,PV writable, and KSCFG is the write that produces the silent loss.
    constexpr uintptr_t OFF_KSCFG = 0x00Cu;
    constexpr uintptr_t OFF_SCTR = 0x034u;
    constexpr uintptr_t OFF_TCSR = 0x038u;
    constexpr uintptr_t OFF_PCR = 0x03Cu;
    // KSCFG.BPMODEN(1) is the write-enable for MODEN(0); BPMODEN=1 with MODEN=0 gates
    // the channel kernel clock.
    constexpr uint32_t KSCFG_BPMODEN_ONLY = 1u << 1;

    inline volatile uint32_t& r32(uintptr_t a)
    {
        return *reinterpret_cast<volatile uint32_t*>(a);
    }

    void scrambler(void* arg)
    {
        uintptr_t const win = reinterpret_cast<uintptr_t>(arg);

        r32(win + OFF_SCTR) = 0; // wreck word/frame length + shift control
        r32(win + OFF_TCSR) = 0; // drop the transmit-data-valid trigger
        r32(win + OFF_PCR) = 0;  // wreck the ASC protocol config
        r32(win + OFF_KSCFG) = KSCFG_BPMODEN_ONLY; // gate the clock LAST

        kos::print("[conreclaim] U0C0 garbled (clock gated); forcing MPU fault\n");

        // One word past the granted window: U0C1 (0x4003_0200), ungranted.
        r32(win + U0C0_WINDOW) = 0;

        kos_panic("[conreclaim] FAILURE: wild write did not fault");
    }
}

int main(int, char**)
{
    kos::print("[conreclaim] scramble-then-panic console-reclaim test\n");

    int const s = kos::thread::spawn(
        scrambler, reinterpret_cast<void*>(U0C0_BASE), "scrambler",
        SCRAMBLER_PRIO, KOS_POLICY_FIFO, /*quantum_ns=*/0,
        /*privileged=*/false, /*mem=*/nullptr, /*mem_size=*/0,
        /*stack=*/nullptr, /*stack_size=*/0,
        /*mmio=*/reinterpret_cast<void*>(U0C0_BASE), U0C0_WINDOW);
    if (s < 0)
    {
        // -KOS_EBUSY: a live domain already holds U0C0, which no service list may do
        // while this test runs.
        char e[64];
        ksnprintf(e, sizeof(e), "[conreclaim] U0C0 spawn refused, errno %d", -s);
        kos_panic(e);
    }

    // Park: root exiting would end the system before the scrambler faults. Fall back to
    // a sleep park if the semaphore could not be created (else a -1 handle spins a hot
    // loop of failing sem_wait syscalls).
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
