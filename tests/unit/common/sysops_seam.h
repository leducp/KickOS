// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The half of a translating backend's host seam that no arch owns: the operation record, the
// simulated core, the mid-edit hook and the frame pool. Each backend's seam records its own
// operations through it. Keep this header independent of GTest.

#ifndef KICKOS_TESTS_UNIT_COMMON_SYSOPS_SEAM_H
#define KICKOS_TESTS_UNIT_COMMON_SYSOPS_SEAM_H

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    namespace testfix
    {
        constexpr size_t SYSOPS_GRANULE = 4096;
        constexpr size_t SYSOPS_FRAMES = 64;

        // Recorded operation and operand, or zero for an operation without one. Tags compare
        // by pointer, so a backend records and a gate compares the same named constant.
        struct SysOp
        {
            char const* tag;
            uint64_t arg;
        };

        size_t ops_count();

        // Clear the operation trace without resetting machine state.
        void ops_clear();
        SysOp op_at(size_t i);

        // Count matching operations or return the last index; -1 means absent.
        size_t ops_with(char const* tag);
        int last_index_of(char const* tag);

        // Return the nth operand, or zero if absent. Assert the count before calling.
        uint64_t arg_of_nth(char const* tag, size_t n);

        // Select the simulated core, which arch_cpu_id answers.
        void set_cpu(uint32_t core);
        uint32_t cpu();

        // Invoke once at the next by-address invalidation, between peer sampling and
        // notification.
        void arm_mid_edit(void (*fn)());

        uint32_t frames_allocated();
        uint32_t frames_freed();

        // Remaining successful allocations before failure. Reset restores full capacity.
        void set_frame_budget(uint32_t frames);

        // For the backend seams. Frame 0 is never handed out: 0 is the pool's failure answer.
        void sysop_record(char const* tag, uint64_t arg);
        void sysop_fire_mid_edit();
        void sysops_reset_common(size_t first_frame);
    }
}

#endif
