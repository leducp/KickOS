// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host system-operation and allocation hooks for the ARM64 map editor.
// Record maintenance operands, peer masks and call order; no hardware TLB
// is modeled. Keep this header independent of GTest.

#ifndef KICKOS_TESTS_UNIT_MAPEXEC_ASPACE_SYSOPS_SEAM_H
#define KICKOS_TESTS_UNIT_MAPEXEC_ASPACE_SYSOPS_SEAM_H

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    namespace testfix
    {
        // Recorded operation and operand, or zero for operations without an operand.
        struct SysOp
        {
            char const* tag;
            uint64_t arg;
        };

        // Use these shared tag pointers in recorders and comparisons.
        extern char const* const OP_DSB_ISHST;
        extern char const* const OP_DSB_ISH;
        extern char const* const OP_ISB;
        extern char const* const OP_TLBI_PAGE_IS;
        extern char const* const OP_TLBI_PAGE_LOCAL;
        extern char const* const OP_TLBI_ALL_IS;
        extern char const* const OP_TLBI_ALL_LOCAL;
        extern char const* const OP_WRITE_TTBR0;
        extern char const* const OP_RENDEZVOUS;

        size_t ops_count();

        // Clear the operation trace without resetting machine state.
        void ops_clear();
        SysOp op_at(size_t i);

        // Count matching operations or return the last index; -1 means absent.
        size_t ops_with(char const* tag);
        int last_index_of(char const* tag);

        // Select the simulated core and its TTBR0 register.
        void set_cpu(uint32_t core);
        uint32_t cpu();

        // Control the reported ASID width. Reset restores A53 values.
        // ASIDBits=0 means 8 bits; 2 means 16; other values are reserved.
        void set_tcr(uint64_t tcr);
        void set_mmfr0(uint64_t mmfr0);
        uint64_t tcr_a53();
        uint64_t tcr_a53_without_as();
        uint64_t mmfr0_with_asid_bits(uint64_t field);

        // Invoke once at the next by-address invalidation, between peer sampling
        // and notification.
        void arm_mid_edit(void (*fn)());

        // Puts every core on no translation, empties the frame pool and clears the record.
        void sysops_reset();

        uint32_t frames_allocated();
        uint32_t frames_freed();

        // Remaining successful allocations before failure. Reset restores full capacity.
        void set_frame_budget(uint32_t frames);
    }
}

#endif
