// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The observable half of the seam kernel/amp/ampwindow.cc is compiled against here. `g_node`
// is what arch_cpu_id answers, so one arm can be the producer and then the consumer of a ring.

#ifndef KICKOS_TESTS_UNIT_AMPWINDOW_AMP_SEAM_H
#define KICKOS_TESTS_UNIT_AMPWINDOW_AMP_SEAM_H

#include <stdint.h>

namespace kickos
{
    namespace ampfix
    {
        // Which node arch_cpu_id answers. An arm sets it around the call it wants attributed.
        extern uint32_t g_node;

        // Doorbells the window raised, and the union of the core masks they named.
        extern uint32_t g_sends;
        extern uint32_t g_sent_mask;

        // The ENDPOINT LAYER, stubbed: what the window handed it for the last PORT_REPLY it
        // routed, and what it was told to answer. The real body is kernel/syscall's and needs
        // a whole kernel; what an arm here can pin is that the window routes a reply to it
        // with the tag and the RING the publication carried, and counts a refusal.
        extern uint32_t g_replies;      // routings seen
        extern bool g_reply_answer;     // what the stub answers
        extern uint32_t g_reply_from;   // the ring it was told the reply arrived on
        extern uint32_t g_reply_thread; // the tag, verbatim
        extern uint32_t g_reply_seq;
        extern uint32_t g_reply_len;
        extern uint8_t g_reply_first; // payload[0], or 0 at zero length

        void reset();
    }
}

#endif
