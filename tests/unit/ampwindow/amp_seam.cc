// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The WHOLE seam between kernel/amp/ampwindow.cc and the rest of the image, re-derived with
//
//   nm --undefined-only <the object> | comm -23 - <its defined symbols>
//
// which is arch_cpu_id, arch_ipi_send, arch_ipi_fence, endpoint_far_call_deliver,
// endpoint_far_hold, endpoint_far_reply_deliver and endpoint_far_send_wake at four nodes;
// kmemcpy folds to memcpy where no address space is enforced. One thread of execution runs the
// arms, so the doorbell only records: servicing it here would echo a payload an arm has not yet
// inspected.

#include "amp_seam.h"

#include <kickos/ampwindow.h>
#include <kickos/irqlock.h>
#include <kickos/arch/arch.h>

static_assert(KICKOS_AMP_NODE,
              "this seam answers the AMP arm of the window; below it the whole translation "
              "unit compiles to nothing and no arm here is expressible");

namespace kickos
{
    namespace ampfix
    {
        uint32_t g_node = 0;
        uint32_t g_sends = 0;
        uint32_t g_sent_mask = 0;
        void (*g_on_raise)() = nullptr;
        bool g_deliver_answers = false;
        uint32_t g_hold_port = amp::PORT_MAX;
        uint32_t g_held[HELD_MAX] = {};
        uint32_t g_held_count = 0;
        uint32_t g_replies = 0;
        bool g_reply_answer = false;
        uint32_t g_reply_from = 0;
        uint32_t g_reply_thread = 0;
        uint32_t g_reply_seq = 0;
        uint32_t g_reply_len = 0;
        bool g_reply_refused = false;
        uint8_t g_reply_first = 0;
        uint32_t g_reply_hold = 0;

        void reset()
        {
            for (uint32_t i = 0; i < g_held_count; i++)
            {
                amp::inbound_forget(g_held[i]);
            }
            // THE WINDOW AND THE MINT LIVE IN THE REAL ampwindow.cc, so this reseats those:
            // an arm that left a slot outstanding would hand it to the next, whose verdict
            // would then depend on the order GoogleTest ran them in.
            for (unsigned cls = 0; cls < static_cast<unsigned>(amp::Class::CLASS_MAX); cls++)
            {
              for (uint32_t to = 0; to < amp::NODE_MAX; to++)
              {
                for (uint32_t from = 0; from < amp::NODE_MAX; from++)
                {
                    amp::Ring& r = amp::ring_for(static_cast<amp::Class>(cls), to, from);
                    for (uint32_t lane = 0; lane < amp::LANE_MAX; lane++)
                    {
                        r.head.v[lane].store(0u);
                        r.tail.v[lane].store(0u);
                    }
                    for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
                    {
                        r.slot[i].len.store(0u);
                        r.slot[i].port.store(amp::PORT_MAX);
                        r.slot[i].tag = amp::ReplyTag{};
                        r.slot[i].badge = 0u;
                        r.slot[i].withdrawn.store(0u);
                        for (uint32_t b = 0; b < amp::SLOT_BYTES; b++)
                        {
                            r.slot[i].payload[b] = 0u;
                        }
                    }
                }
              }
            }
            // ONCE PER NODE IDENTITY: window_init seats THIS node's own rows and no other's,
            // and the arms below consume at more than one.
            for (uint32_t n = 0; n < amp::NODE_MAX; n++)
            {
                g_node = n;
                amp::window_init();
            }
            g_node = 0;
            g_sends = 0;
            g_sent_mask = 0;
            g_on_raise = nullptr;
            g_deliver_answers = false;
            g_hold_port = amp::PORT_MAX;
            g_held_count = 0;
            g_replies = 0;
            g_reply_answer = false;
            g_reply_from = 0;
            g_reply_thread = 0;
            g_reply_seq = 0;
            g_reply_len = 0;
            g_reply_refused = false;
            g_reply_first = 0;
            g_reply_hold = 0;
        }
    }

    // No endpoint layer here: unless an arm asks for the answer, this refuses as an unbound
    // port does.
    bool endpoint_far_call_deliver(uint32_t from, uint32_t, amp::ReplyTag const& tag,
                                   uint32_t badge, uint32_t, uint32_t slot, Held)
    {
        if (not ampfix::g_deliver_answers)
        {
            return false;
        }
        uint32_t const record = amp::inbound_seat(from, slot, tag, badge);
        if (record == amp::FAR_RECORD_NONE)
        {
            return false;
        }
        amp::inbound_reply(record, nullptr, 0u);
        return true;
    }

    bool endpoint_far_hold(uint32_t from, uint32_t port, amp::ReplyTag const& tag,
                           uint32_t badge, uint32_t, uint32_t slot, Held)
    {
        if (port != ampfix::g_hold_port or ampfix::g_held_count == ampfix::HELD_MAX)
        {
            return false;
        }
        uint32_t const record = amp::inbound_seat(from, slot, tag, badge);
        if (record == amp::FAR_RECORD_NONE)
        {
            return false;
        }
        ampfix::g_held[ampfix::g_held_count] = record;
        ampfix::g_held_count++;
        return true;
    }

    void endpoint_far_send_wake(Held)
    {
    }

    void endpoint_far_held_withdrawn(Held)
    {
    }

    // A caller that takes the reply HOLDS it: releasing it is the arm's, as landing it is a
    // resumed caller's.
    bool endpoint_far_reply_deliver(uint32_t from, amp::ReplyTag const& tag, uint32_t hold,
                                    uint32_t len, bool refused, Held)
    {
        ampfix::g_replies++;
        ampfix::g_reply_from = from;
        ampfix::g_reply_thread = tag.thread;
        ampfix::g_reply_seq = tag.seq;
        ampfix::g_reply_len = len;
        ampfix::g_reply_refused = refused;
        ampfix::g_reply_first = 0;
        if (len != 0)
        {
            ampfix::g_reply_first = static_cast<uint8_t const*>(amp::hold_payload(hold))[0];
        }
        if (ampfix::g_reply_answer)
        {
            ampfix::g_reply_hold = hold;
        }
        return ampfix::g_reply_answer;
    }
}

extern "C"
{

#if KICKOS_NUM_CORES > 1
uint32_t arch_cpu_id(void)
{
    return kickos::ampfix::g_node;
}

void arch_ipi_send(uint32_t cores)
{
    if (kickos::ampfix::g_on_raise != nullptr)
    {
        kickos::ampfix::g_on_raise();
    }
    kickos::ampfix::g_sends++;
    kickos::ampfix::g_sent_mask |= cores;
}

// The host fixture runs one thread, so the pairing this orders has no second side here.
void arch_ipi_fence(void)
{
}
#endif

arch_irq_state_t arch_irq_save(void)
{
    return 0;
}

void arch_irq_restore(arch_irq_state_t)
{
}

}
