// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The doorbell's rendezvous cells: what a backend takes from here rather than deciding for
// itself, which is how wide they are, which row this image writes, and where they are placed.
//
// The acknowledgement travels in MEMORY: no interrupt controller here reports to a sender that
// a target has serviced a raise, so a backend wanting a rendezvous owns a set of these and
// spins on the answering core's row. Every cell has exactly one writer, the core asking for a
// request row and the core answering for an answer row.
//
// The row's padding is the PART's, so it stays a backend's own argument: an A53 shares a
// 64-byte line, while a part with no data cache may stripe adjacent words across banks.

#ifndef KICKOS_ARCH_DOORBELL_CELLS_H
#define KICKOS_ARCH_DOORBELL_CELLS_H

#include <kickos/arch/amp_shared.h>
#include <kickos/arch/arch.h>

#include <kickos/sys/atomic.h>

#include <stdint.h>

// Which row this image writes, and it is NOT arch_cpu_id(): an own-image node drives one core,
// so that call folds to a constant zero and both nodes would write row zero.
#if KICKOS_AMP_OWN_IMAGE
#define arch_doorbell_core() ((uint32_t)KICKOS_AMP_SELF_CORE)
#else
#define arch_doorbell_core() arch_cpu_id()
#endif

#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)

namespace kickos
{
    namespace doorbell
    {
        static_assert(KICKOS_DOORBELL_CORES >= KICKOS_NUM_CORES,
                      "the matrix is indexed by machine core, so it cannot be narrower than "
                      "the cores this image drives");

        using Seq = Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE>;

        // One row per core, written by that core alone. Keep a backend's request, answer and
        // served arrays SEPARATE: a disassembly gate reads the service body's instruction
        // sequence, and gathering them into one object changes what it loads.
        template <uint32_t LINE>
        struct alignas(LINE) Row
        {
            Seq seq[KICKOS_DOORBELL_CORES];
        };
    }
}

#endif

#endif
