// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host system-operation hooks for the ARM64 map editor. Record maintenance operands, peer masks
// and call order; no hardware TLB is modeled. Keep this header independent of GTest.

#ifndef KICKOS_TESTS_UNIT_MAPEXEC_ASPACE_SYSOPS_SEAM_H
#define KICKOS_TESTS_UNIT_MAPEXEC_ASPACE_SYSOPS_SEAM_H

#include <stdint.h>

#include "sysops_seam.h"

namespace kickos
{
    namespace testfix
    {
        extern char const* const OP_DSB_ISHST;
        extern char const* const OP_DSB_ISH;
        extern char const* const OP_ISB;
        extern char const* const OP_TLBI_PAGE_IS;
        extern char const* const OP_TLBI_PAGE_LOCAL;
        extern char const* const OP_TLBI_ALL_IS;
        extern char const* const OP_TLBI_ALL_LOCAL;
        extern char const* const OP_WRITE_TTBR0;
        extern char const* const OP_RENDEZVOUS;

        // Control the reported ASID width. Reset restores A53 values.
        // ASIDBits=0 means 8 bits; 2 means 16; other values are reserved.
        void set_tcr(uint64_t tcr);
        void set_mmfr0(uint64_t mmfr0);
        uint64_t tcr_a53();
        uint64_t tcr_a53_without_as();
        uint64_t mmfr0_with_asid_bits(uint64_t field);

        // The reserved-root form: activation fills a per-core cell and leaves the register.
        void set_root_deferred(bool on);
        uint64_t user_ttbr0_of(uint32_t core);
        void set_user_ttbr0(uint32_t core, uint64_t ttbr);

        // Puts every core on no translation, empties the frame pool and clears the record.
        void sysops_reset();

        // Frame-free count at the last whole-TLB invalidate, for reclamation-order checks.
        uint32_t frames_freed_at_sweep();
    }
}

#endif
