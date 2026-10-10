// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host machine for the x86-64 map editor: a fixed CPUID model, the control and model-specific
// registers per core, and INVLPG and peer rendezvous recorded through sysops_seam.h. Physical
// addresses are host addresses, since the backend reaches every table by identity. Keep this
// header independent of GTest.

#ifndef KICKOS_TESTS_UNIT_MAPSHOOT_X86_64_SYSOPS_SEAM_H
#define KICKOS_TESTS_UNIT_MAPSHOOT_X86_64_SYSOPS_SEAM_H

#include <stdint.h>

#include "sysops_seam.h"

struct arch_aspace;

namespace kickos
{
    namespace testfix
    {
        // INVLPG's address, then the watched space's leaf at that address as the INVLPG saw it.
        extern char const* const OP_INVLPG;
        extern char const* const OP_INVLPG_LEAF;
        extern char const* const OP_RENDEZVOUS;

        // Rebuild the firmware root, adopt it through aspace_init, then clear the record.
        // Spaces left open by an earlier case keep their residency rows.
        void sysops_reset();

        void watch_space(struct arch_aspace* space);

        // Frame-free count at the last peer rendezvous, for reclamation-order checks.
        uint32_t frames_freed_at_rendezvous();

        // Split the firmware's map over the RAM array down to a leaf at `leaf_level` (3 for
        // 1 GiB, 2 for 2 MiB, 1 for 4 KiB), optionally under a five-level root, and set the user
        // bit on every entry from the root to that leaf but the one at `clear_level` (0 for
        // none).
        void firmware_user_chain(int leaf_level, int clear_level, bool five_levels);
        // Adopt the firmware root again, as it now stands.
        void adopt();
    }
}

#endif
