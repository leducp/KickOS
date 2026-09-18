// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Host hooks for the RV64 map editor. Record fences, satp writes and peer
// notifications without executing them. Interrupt masking is a no-op.
// The real backend performs table walks, descriptor updates, reclamation,
// residency tracking and slot allocation over a host array.
// Keep this header independent of GTest.

#ifndef KICKOS_TESTS_UNIT_MAPFENCE_RV64_SYSOPS_SEAM_H
#define KICKOS_TESTS_UNIT_MAPFENCE_RV64_SYSOPS_SEAM_H

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    namespace testfix
    {
        // Recorded operation and operand: VA, satp value, peer mask, or zero.
        struct SysOp
        {
            char const* tag;
            uint64_t arg;
        };

        // Use these shared tag pointers in recorders and comparisons.
        extern char const* const OP_FENCE_W_W;
        extern char const* const OP_SFENCE_PAGE;
        extern char const* const OP_SFENCE_ALL;
        extern char const* const OP_WRITE_SATP;
        extern char const* const OP_RENDEZVOUS;

        size_t ops_count();

        // Clear the operation trace without resetting machine state.
        void ops_clear();
        SysOp op_at(size_t i);

        // Count matching operations or return the last index; -1 means absent.
        size_t ops_with(char const* tag);
        int last_index_of(char const* tag);

        // Return the nth operand, or zero if absent. Assert the count before calling.
        uint64_t arg_of_nth(char const* tag, size_t n);

        // Select the simulated hart and its satp register.
        void set_cpu(uint32_t core);
        uint32_t cpu();

        // Invoke once at the next by-address fence, between peer sampling and notification.
        void arm_mid_edit(void (*fn)());

        // Reset registers, frame pool and trace, then run boot handover.
        // asid_bits sets the implemented low ASID bits before the boot probe runs.
        void sysops_reset(unsigned asid_bits = 16);

        uint32_t frames_allocated();
        uint32_t frames_freed();

        // Remaining successful allocations before failure. Reset restores full capacity.
        void set_frame_budget(uint32_t frames);

        // Bounds of the direct RAM window and virtual base of transient slots.
        // Transient pointers have no host backing and must not be dereferenced.
        uint64_t window_pa_hi();
        uintptr_t window_va();

        // Physical-to-host address offset supplied at handover.
        uintptr_t window_delta();

        // Frame-free count at the last peer notification, for reclamation-order checks.
        uint32_t frames_freed_at_rendezvous();
    }
}

#endif
