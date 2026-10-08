// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// User-selectable CPU clock: coherence orchestration around the arch_cpu_clock_set seam.
// No policy here. The step order is load-bearing: refuse, mask, disarm, flush to
// shift-idle, retune, re-derive baud, re-arm.

#include <kickos/time.h>
#include <kickos/sched.h>
#include <kickos/arch/arch.h>
#include <kickos/irqlock.h>
#include <kickos/console_tx.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/errno.h>

namespace kickos
{
    // Returns the landed Hz (0 == did not move). The coherence tail runs on any actual
    // change, never on a success flag: a staged fallback (K64F fail_to_fei) moved the clock
    // too.
    uint64_t cpu_clock_set(kos_pstate_t target)
    {
        // A userspace driver owns the UART and its baud cannot be re-derived, so refuse
        // before masking. A panic-reclaimed UART is not kernel-owned either.
        if (console_owner_is_kernel() == 0)
        {
            return arch_cpu_clock_hz();
        }

        IrqLock lock;
        uint64_t const previous = arch_cpu_clock_hz();

        // Forget the deadline too: the trailing rearm must not be skipped as a repeat of
        // what is no longer programmed.
        ktime_disarm(lock);

        // No byte may still clock out at the old baud when the peripheral clock moves. The
        // TX IRQ is masked, so the ring drains polled.
        console_tx_flush_sync();
        arch_console_flush_sync();

        uint64_t const hz = arch_cpu_clock_set(static_cast<uint32_t>(target));

        if (hz != 0 and hz != previous)
        {
            arch_console_retune();
        }

        // hz == 0 included: the disarm above left nothing armed.
        ktime_rearm(sched::current(), lock);

        return hz;
    }
}
