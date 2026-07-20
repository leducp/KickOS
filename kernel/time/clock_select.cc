// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// User-selectable CPU clock: the arch-neutral coherence orchestration around the
// arch_cpu_clock_set MECHANISM seam. This is NOT policy -- no governor, DVFS, or
// idle heuristic lives here; it "changes the clock coherently to the requested
// P-state and reports the landed Hz" for a future userspace power-manager service
// to drive. The spec (with the load-bearing rulings) is docs/design-m3-clock-select.md;
// the sequence below is section 2.3.

#include <kickos/time.h>
#include <kickos/arch/arch.h>
#include <kickos/irqlock.h>
#include <kickos/console_tx.h>

#include <kickos/sys/abi.h>

namespace kickos
{
    // Coherently retune the core clock to `target` and return the LANDED Hz (0 ==
    // could-not / did-not move). Runs the full coherence tail ONLY when the clock
    // actually moved (achieved != previous, B1) -- never gated on a success flag,
    // because a staged fallback (K64F fail_to_fei) moved the clock too and must be
    // plumbed exactly like a success. Caller (the syscall) has already gated on
    // privilege; this owns the console-ownership refusal + the masked transition.
    uint32_t cpu_clock_set(kos_pstate_t target)
    {
        // S4: a userspace driver owns the UART -> the kernel cannot re-derive or
        // relocate its baud across a peripheral-clock move, so REFUSE before any
        // masking (a true no-op: previous Hz, unchanged). RECLAIMED (panic took the
        // UART back) is also not-kernel-owned; a retune on a panic path is never wanted.
        if (console_owner_is_kernel() == 0)
        {
            return arch_cpu_clock_hz();
        }

        IrqLock lock; // single-core: masks the one timer, quiescing time across the change
        uint32_t const previous = arch_cpu_clock_hz();

        // S1: stop SysTick + clear g_armed_deadline_ns + drop a pended SysTick, so
        // nothing fires mid-transition at the stale rate AND the trailing ktime_rearm
        // cannot be no-op'd by the arm-dedup guard (it always reloads after a disarm).
        arch_timer_disarm();

        // Flush console TX to SHIFT-IDLE (S6): drain the software ring into the UART
        // (polled -- the TX IRQ is masked here), then wait for the shift register to
        // empty, so no byte is still clocking out at the OLD baud when the peripheral
        // clock moves. Both are no-ops on a chip that does not retune / has no ring.
        console_tx_flush_sync();
        arch_console_flush_sync();

        // The backend does flash-WS/voltage, the divider/PLL staircase, the re-anchor
        // at the rate edge, and writes SystemCoreClock; it returns the landed Hz. The
        // seam carries the pstate as a plain u32 (arch.h stays ABI-neutral).
        uint32_t const hz = arch_cpu_clock_set(static_cast<uint32_t>(target));

        // COHERENCE TAIL: run on ANY actual change (success OR staged fallback), never
        // on a success flag (B1). ns deadlines + the RR slice are clock-invariant and
        // need no rescale (they are stored in ns); the re-anchor kept `now` continuous.
        if (hz != 0 and hz != previous)
        {
            arch_console_retune(); // re-derive the baud from the new SystemCoreClock
        }

        // (e) Always re-arm: we disarmed above. Reloads SysTick against the current
        // SystemCoreClock (unchanged if hz == 0), so a re-arm is never skipped.
        ktime_rearm();

        return hz; // truthful landed Hz; 0 == cannot-change / unsupported
    }
}

// Weak default for the arch seam: this chip cannot change its clock at all. A backend
// whose core clock is retunable (XMC4800, K64F) strong-overrides this. Lives in a
// neutral kernel TU so every arch build (ARM, RISC-V, Xtensa, RX, sim) gets the
// default without each backend having to define it. See arch.h for the contract.
extern "C" uint32_t __attribute__((weak)) arch_cpu_clock_set(uint32_t target)
{
    (void)target;
    return 0;
}
