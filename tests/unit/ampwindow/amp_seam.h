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
        // Run at every raise, before it is recorded; reset clears it.
        extern void (*g_on_raise)();
        // A delivered call is seated and answered empty on the spot, inside the service pass.
        extern bool g_deliver_answers;
        // As g_deliver_answers, but the answer carries the call's own payload.
        extern bool g_deliver_echoes;
        // A send or call arriving on this port is held in its slot, as for a receiver that never
        // comes, and its record's token kept in g_held; PORT_MAX holds none.
        extern uint32_t g_hold_port;
        constexpr uint32_t HELD_MAX = 8u;
        extern uint32_t g_held[HELD_MAX];
        extern uint32_t g_held_count;

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
        extern bool g_reply_refused;  // the reply carried REPLY_REFUSED
        extern uint8_t g_reply_first; // payload[0] read through the hold, or 0 at zero length
        extern uint32_t g_reply_hold;  // the hold the last routing handed a caller that took it

        void reset();
    }
}

#endif
