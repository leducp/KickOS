// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// User-selectable CPU clock: coherence orchestration around the arch_cpu_clock_set seam.
// No policy here. The step order is load-bearing: mask, refuse, disarm, drain the device to
// shift-idle, retune, re-derive baud, re-prime, re-arm.

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
        IrqLock lock;
        // Read under the lock the retune holds: a publish landing after an unlocked read would
        // have its driver's baud rewritten. A panic-reclaimed UART is not kernel-owned either.
        if (console_owner_is_kernel() == 0)
        {
            return arch_cpu_clock_hz();
        }
        uint64_t const previous = arch_cpu_clock_hz();

        // Forget the deadline too: the trailing rearm must not be skipped as a repeat of
        // what is no longer programmed.
        ktime_disarm(lock);

        // No byte may still clock out at the old baud when the peripheral clock moves. The mask
        // holds every pusher off, so the ring keeps its bytes and only the device drains.
        arch_console_flush_sync();

        uint64_t const hz = arch_cpu_clock_set(static_cast<uint32_t>(target));

        if (hz != 0 and hz != previous)
        {
            arch_console_retune();
            console_tx_reprime();
        }

        // hz == 0 included: the disarm above left nothing armed.
        ktime_rearm(sched::current(), lock);

        return hz;
    }
}
