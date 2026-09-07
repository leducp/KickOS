// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The machine the AArch64 stage-1 map editor edits against, on a host that is not AArch64:
// sysops_armv8a.h's operations, the frame pool the editor allocates tables from, the physical
// window they are reached through, and the instruction-side rendezvous whose debt this gate
// exists to read.
//
// EVERY MAINTENANCE OPERATION IS RECORDED IN ORDER. The claim carried here is an ORDER and a
// VALUE: the rendezvous follows the edits and the barrier that publishes them, and the peer set
// it carries is the one that stood BEFORE the first edit.
//
// Keep this header GTEST-FREE: it is included by a translation unit compiled with the arch
// sources' own definitions.

#ifndef KICKOS_TESTS_UNIT_MAPEXEC_ASPACE_SYSOPS_SEAM_H
#define KICKOS_TESTS_UNIT_MAPEXEC_ASPACE_SYSOPS_SEAM_H

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    namespace testfix
    {
        // One recorded system operation. `arg` is the operand for the by-address maintenance
        // and the peer mask for the rendezvous, and zero where the operation takes none.
        struct SysOp
        {
            char const* tag;
            uint64_t arg;
        };

        // The tags, compared by POINTER as well as by text: each recorder passes the one
        // constant below, so a comparison cannot pass on a coincidence of spelling.
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

        // Drops the record and nothing else, so an arm reads the call it is about rather than
        // the pre-state it built first.
        void ops_clear();
        SysOp op_at(size_t i);

        // How many of `tag` stand in the record, and the index of the last one. Index -1 where
        // there is none.
        size_t ops_with(char const* tag);
        int last_index_of(char const* tag);

        // The core the seam answers arch_cpu_id with. Each core has its own TTBR0_EL1.
        void set_cpu(uint32_t core);
        uint32_t cpu();

        // Runs once, on the FIRST by-address invalidate the next editing body issues, and then
        // disarms: an event that lands between the peer set being sampled and the rendezvous
        // being rung.
        void arm_mid_edit(void (*fn)());

        // Puts every core on no translation, empties the frame pool and clears the record.
        void sysops_reset();

        // Frames handed out and given back, so an arm can tell a table allocation from none.
        uint32_t frames_allocated();
        uint32_t frames_freed();

        // Frames the pool will still hand out before it answers 0. sysops_reset restores it to
        // the whole pool, so an arm that wants an allocation failure has to ask for one.
        void set_frame_budget(uint32_t frames);
    }
}

#endif
