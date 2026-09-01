// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The observable half of the seam kernel/sync/klock.cc is compiled against here. `g_core` is
// what arch_cpu_id answers, so an arm speaks as whichever core it names; the counters record
// what the lock's brackets did, which is how an arm reads whether an owed reschedule was
// honoured and at which release rather than only that the bracket returned.

#ifndef KICKOS_TESTS_UNIT_RESCHEDOWED_RESCHED_SEAM_H
#define KICKOS_TESTS_UNIT_RESCHEDOWED_RESCHED_SEAM_H

#include <stdint.h>

#include <kickos/instance.h>

namespace kickos
{
    namespace reschedfix
    {
        constexpr uint32_t CORES = KICKOS_KERNEL_CORES;

        // Which core arch_cpu_id answers. An arm sets it around the call it wants attributed.
        extern uint32_t g_core;

        // Raises each core restored to itself, counted where the doorbell would be raised.
        extern uint32_t g_raised[CORES];

        // Cross-core lock acquisitions and releases the real klock.cc performed.
        extern uint32_t g_acquired;
        extern uint32_t g_released;

        // Acquisitions outstanding at the instant a raise was restored, kept at its high-water
        // mark. Nonzero means the release restored the raise while still holding the word.
        extern uint32_t g_held_at_raise;

        // Run from inside the raise stub, once, on its next call. An arm arms this to be a peer
        // asking again at the instant the release is restoring the raise.
        extern void (*g_raise_action)();

        // Cross-core doorbells the ask raised, and the union of the masks they named.
        extern uint32_t g_sends;
        extern uint32_t g_sent_mask;

        // Whether the ask ALREADY STOOD against each core at the instant the doorbell carrying
        // it was raised, sampled inside the raise. A zero for a core the raise named is the
        // publish landing after its own edge: the raise is what a consumer absorbs, so an ask
        // published behind it can be absorbed before it exists.
        extern uint32_t g_owed_at_send[CORES];

        // Total raises restored across every core.
        uint32_t raise_total();

        void reset();
    }
}

#endif
