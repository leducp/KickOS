// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host hooks for the RV64 map editor. Record fences, satp writes and peer notifications without
// executing them. The real backend performs table walks, descriptor updates, reclamation,
// residency tracking and slot allocation over a host array. Keep this header independent of
// GTest.

#ifndef KICKOS_TESTS_UNIT_MAPFENCE_RV64_SYSOPS_SEAM_H
#define KICKOS_TESTS_UNIT_MAPFENCE_RV64_SYSOPS_SEAM_H

#include <stdint.h>

#include "sysops_seam.h"

namespace kickos
{
    namespace testfix
    {
        extern char const* const OP_FENCE_W_W;
        extern char const* const OP_SFENCE_PAGE;
        extern char const* const OP_SFENCE_ALL;
        extern char const* const OP_WRITE_SATP;
        extern char const* const OP_RENDEZVOUS;

        // Reset registers, frame pool and trace, then run boot handover.
        // asid_bits sets the implemented low ASID bits before the boot probe runs.
        void sysops_reset(unsigned asid_bits = 16);

        // Bounds of the direct RAM window and virtual base of transient slots.
        // Transient pointers have no host backing and must not be dereferenced.
        uint64_t window_pa_hi();
        uintptr_t window_va();

        // Physical-to-host address offset supplied at handover.
        uintptr_t window_delta();

        // Frame-free count at the last peer notification, for reclamation-order checks.
        uint32_t frames_freed_at_rendezvous();

        // The satp value the hart last wrote.
        uint64_t satp_of_hart(uint32_t core);
    }
}

#endif
