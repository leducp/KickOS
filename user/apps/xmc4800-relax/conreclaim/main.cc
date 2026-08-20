// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 console-reclaim test (design D-test 3): an unprivileged thread holding the
// U0C0 DEV grant drives the console UART into the true silent-loss state, then ends the
// system. PASS is the TERMINAL REPORT still reaching the wire, which proves
// arch_console_reclaim re-initialised the UART from inside kpanic_enter.
//
// The expected wire text is "KERNEL PANIC: [conreclaim] PASS: the dead channel came back"
// plus the marker line above it, both arriving on a channel that was fully dead when they
// were written.
//
// THE TERMINAL EVENT MUST BE A PANIC AND NOT A FAULT. Under fault isolation an unprivileged
// thread's fault is answered by kickos_fault_kill_thread BEFORE kpanic_enter runs: the
// thread dies, the system lives, and the thread-kill path deliberately does not reclaim the
// console (see kpanic_enter in kernel/init/console.cc, which states that ruling). A wild
// write here would kill the scrambler, leave the channel dead and take the whole run dark. A
// privileged thread is no way out either, because PRIVDEFENA is on and privileged accesses
// do not MemManage. kos_panic is the one terminal path an unprivileged thread still has, and
// it reaches kpanic_enter through exactly the same call the fault reporter used, so the
// reclaim body under test is unchanged. The fault-isolation side of the same event is covered
// by rootfault / mpu_fault / faultsurvive, which assert the thread died and nothing panicked.
//
// Sequence in one straight-line thread, so step 3 strictly follows steps 1-2:
//   1. garble the in-window registers, GATING the channel kernel clock last
//      (KSCFG.BPMODEN=1 with MODEN=0, RM p.18-165) so the channel is left fully dead;
//   2. log a marker through the kernel debug path, ordering the scramble before the end.
//      The channel is already dead, so this marker cannot reach the wire until the
//      reclaim, and it comes out of the console ring on the panic flush instead;
//   3. kos_panic -> user_panic -> kickos::kpanic -> kpanic_enter -> arch_console_reclaim
//      -> the banner and the queued marker on a polled, re-initialised channel.
//
// kpanic_enter reclaims from ANY console-ownership state (kernel/init/console.cc): this
// app runs with kickos_services_none, so the console is KERNEL_OWNED, never published,
// and a reclaim gated on USER_OWNED would skip and lose the report.
//
// U0C0 admits ONE holder (the one-holder-per-window rule in domain_for), so this test
// requires a service list that publishes no userspace console driver: with
// KICKOS_SERVICE_LIST=kickos_services_none the kernel owns the UART, the kernel domain
// carries no DEV region, and this scrambler is the sole holder. The property under test
// does not depend on WHO garbled the UART, so the scrambler stands in for a console
// driver that dies with its device left misconfigured.
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
#error "conreclaim requires enforcement: build the board's base variant, not its flat one"
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

        kos::print("[conreclaim] U0C0 garbled (clock gated); ending the system\n");

        // The line an operator reads as PASS: the kernel prints it after its own trusted
        // "KERNEL PANIC: " prefix, on a channel this thread killed a moment ago. Kept well
        // inside user_panic's 64-byte buffer, because a truncated verdict reads as a cut-off
        // run.
        kos_panic("[conreclaim] PASS: the dead channel came back");
    }
}

int main(int, char**)
{
    kos::print("[conreclaim] scramble-then-panic console-reclaim test\n");

    auto const s = kos::thread::spawn(
        scrambler, reinterpret_cast<void*>(U0C0_BASE), "scrambler",
        SCRAMBLER_PRIO, KOS_POLICY_FIFO, /*quantum_ns=*/0,
        /*privileged=*/false, /*mem=*/nullptr, /*mem_size=*/0,
        /*stack=*/nullptr, /*stack_size=*/0,
        /*mmio=*/reinterpret_cast<void*>(U0C0_BASE), U0C0_WINDOW);
    if (not s.valid())
    {
        // -KOS_EBUSY: a live domain already holds U0C0, which no service list may do
        // while this test runs.
        char e[64];
        ksnprintf(e, sizeof(e), "[conreclaim] U0C0 spawn refused, errno %d", -s.error());
        kos_panic(e);
    }

    // Park: root exiting would end the system before the scrambler faults. Fall back to
    // a sleep park if the semaphore could not be created (else an unmintable handle spins a
    // hot loop of failing sem_wait syscalls).
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
