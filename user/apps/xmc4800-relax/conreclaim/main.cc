// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 console-reclaim test (design D-test 3): an unprivileged thread holding the
// U0C0 DEV grant drives the console UART into the true silent-loss state, then ends the
// system. PASS is the TERMINAL REPORT still reaching the wire, which proves
// arch_console_reclaim re-initialised the UART from inside kpanic_enter.
//
// THE TERMINAL EVENT MUST BE A PANIC AND NOT A FAULT. An unprivileged thread's fault is
// answered by kickos_fault_kill_thread before kpanic_enter runs, and that path leaves the
// console alone by design (kpanic_enter, kernel/init/console.cc), so a wild write here
// would leave the channel dead and take the run dark. A privileged thread reaches no fault
// either: PRIVDEFENA is on and privileged accesses do not MemManage. kos_panic enters
// kpanic_enter through the same call the fault reporter uses, so the reclaim body under
// test is the real one.
//
// The marker logged after the scramble is evidence: the channel is already dead when it is
// written, so it can only reach the wire out of the console ring on the panic flush.
//
// The reclaim under test is the one from KERNEL_OWNED: this app runs with
// kickos_services_none, so the console is never published.
//
// U0C0 admits ONE holder (the one-holder-per-window rule in domain_for), so the service
// list must publish no userspace console driver; with
// KICKOS_SERVICE_LIST=kickos_services_none the kernel owns the UART, the kernel domain
// carries no DEV region, and this scrambler is the sole holder.
//
// Register addresses / bit fields are clean-room from the XMC4700/XMC4800 Reference
// Manual (V1.3, 2016-07); no XMCLib/DAVE/CMSIS vendor source. Diagnostic app
// (kickos_add_diagnostic_app): never a production image.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

#include <stdint.h>

// Enforcement is what confines the scrambler: the granted window is only a real capability
// under PMSA, so the reclaim is witnessed against a device an unprivileged thread genuinely
// held.
#if !KICKOS_HAVE_MPU
#error "conreclaim requires enforcement: build the board's base variant, not its flat one"
#endif

namespace
{
    // The kernel console channel. 512 B is pow2 and 0x200-aligned, so PMSA encodes the
    // window as one exact-cover descriptor.
    constexpr uintptr_t U0C0_BASE = 0x40030000u;
    constexpr uint32_t U0C0_WINDOW = 0x200u;

    constexpr uint8_t SCRAMBLER_PRIO = 14;

    // Per-channel offsets (RM Table 18-20). These four are U,PV writable, so an
    // unprivileged store lands; FDR (0x010), BRG (0x014) and CCR (0x040) are write-PV-only
    // and discard it silently (measured by user/apps/xmc4800-relax/pvprobe). KSCFG is the
    // write that produces the silent loss.
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

        // Kept inside user_panic's 64-byte buffer: a truncated verdict reads as a cut-off run.
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
        // -KOS_EBUSY: a live domain already holds U0C0.
        char e[64];
        ksnprintf(e, sizeof(e), "[conreclaim] U0C0 spawn refused, errno %d", -s.error());
        kos_panic(e);
    }

    // Park: root exiting would end the system before the scrambler does. The sleep fallback
    // covers a semaphore that could not be created, since an unmintable handle would spin a
    // hot loop of failing sem_wait syscalls.
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
