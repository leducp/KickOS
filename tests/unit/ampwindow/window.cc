// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The validation a node owes the memory another node writes. An arm here IS the far side at
// the instant it chooses: producer to publish, consumer to take, and by hand where the
// subject is a publication send() cannot produce.
//
// THE COUNTERS ARE READ AS DELTAS: they are static in the real translation unit and no arm
// can seat them, so comparing absolutes would depend on the order GoogleTest ran the arms in.
// FIELD BY FIELD, never a struct copy: the row is relaxed atomics, which do not copy.

#include "amp_seam.h"

#include <kickos/ampwindow.h>

#include <gtest/gtest.h>

namespace
{
    namespace amp = kickos::amp;
    namespace fix = kickos::ampfix;

    constexpr uint32_t NODE_A = 0; // the consumer
    constexpr uint32_t NODE_B = 1; // the producer
    constexpr uint32_t NODE_C = 2; // a third node whose ring stays empty

    // A port inside the mint width that nothing minted.
    constexpr uint32_t PORT_UNMINTED = 7u;

    constexpr uint8_t OUT_FILL = 0xAAu;
    constexpr uint32_t LEN_SENTINEL = 0xDEADBEEFu;
    constexpr uint32_t PORT_SENTINEL = 0xFEEDFACEu;
    constexpr amp::ReplyTag TAG_SENTINEL = {0xF00DF00Du, 0xF00DBEEFu};

    // What the arms whose subject is not the tag publish. NON-ZERO on both fields, so a slot
    // reset leaves neither of them by accident.
    constexpr amp::ReplyTag TAG_CARRIED = {0x11112222u, 0x33334444u};

    // What publish_raw leaves in a malformed slot, distinct from the poison above: a refusal
    // that wrote the tag through would show up as this.
    constexpr amp::ReplyTag TAG_FORGED = {0x0BADF00Du, 0x0BADBEEFu};

    amp::Sent send_tagged_as(uint32_t me, uint32_t to, uint32_t port, amp::ReplyTag const& tag,
                             void const* payload, uint32_t len)
    {
        uint32_t const was = fix::g_node;
        fix::g_node = me;
        amp::Sent const s = amp::send(to, port, tag, payload, len);
        fix::g_node = was;
        return s;
    }

    amp::Sent send_as(uint32_t me, uint32_t to, uint32_t port, void const* payload, uint32_t len)
    {
        return send_tagged_as(me, to, port, TAG_CARRIED, payload, len);
    }

    uint32_t g_last_slot = 0;

    amp::Verdict take_tagged_as(uint32_t me, uint32_t from, void* out, uint32_t* out_len,
                                uint32_t* out_port, amp::ReplyTag* out_tag)
    {
        uint32_t const was = fix::g_node;
        fix::g_node = me;
        amp::Verdict const v = amp::take_call(from, out, out_len, out_port, out_tag, &g_last_slot);
        if (v == amp::Verdict::TOOK)
        {
            amp::release_call(from, g_last_slot);
        }
        fix::g_node = was;
        return v;
    }

    // A take that HOLDS its slot, for the arms that exercise the record bound.
    amp::Verdict take_held_as(uint32_t me, uint32_t from, uint32_t* out_slot)
    {
        uint8_t buf[amp::SLOT_BYTES];
        uint32_t len = 0;
        uint32_t port = amp::PORT_MAX;
        amp::ReplyTag tag = {};
        uint32_t const was = fix::g_node;
        fix::g_node = me;
        amp::Verdict const v = amp::take_call(from, buf, &len, &port, &tag, out_slot);
        fix::g_node = was;
        return v;
    }

    void release_as(uint32_t me, uint32_t from, uint32_t slot)
    {
        uint32_t const was = fix::g_node;
        fix::g_node = me;
        amp::release_call(from, slot);
        fix::g_node = was;
    }

    amp::Verdict take_reply_as(uint32_t me, uint32_t from, void* out, uint32_t* out_len,
                               uint32_t* out_port, amp::ReplyTag* out_tag)
    {
        uint32_t const was = fix::g_node;
        fix::g_node = me;
        amp::Verdict const v = amp::take_reply(from, out, out_len, out_port, out_tag);
        fix::g_node = was;
        return v;
    }

    amp::Verdict take_as(uint32_t me, uint32_t from, void* out, uint32_t* out_len,
                         uint32_t* out_port)
    {
        amp::ReplyTag scratch = {};
        return take_tagged_as(me, from, out, out_len, out_port, &scratch);
    }

    // The four out-parameters of take(), pre-poisoned: a verdict other than TOOK must leave
    // all four exactly as they came. An arm reaching take through take_as leaves `tag`
    // poisoned by construction, that wrapper keeping a scratch of its own.
    struct Taken
    {
        uint8_t buf[amp::SLOT_BYTES];
        uint32_t len;
        uint32_t port;
        amp::ReplyTag tag;

        Taken()
        {
            for (uint32_t i = 0; i < amp::SLOT_BYTES; i++)
            {
                buf[i] = OUT_FILL;
            }
            len = LEN_SENTINEL;
            port = PORT_SENTINEL;
            tag = TAG_SENTINEL;
        }

        bool untouched() const
        {
            if (len != LEN_SENTINEL or port != PORT_SENTINEL)
            {
                return false;
            }
            if (tag.thread != TAG_SENTINEL.thread or tag.seq != TAG_SENTINEL.seq)
            {
                return false;
            }
            for (uint32_t i = 0; i < amp::SLOT_BYTES; i++)
            {
                if (buf[i] != OUT_FILL)
                {
                    return false;
                }
            }
            return true;
        }
    };

    // Publish one slot the way a MALFORMED far side would, with the head bump a well-formed
    // producer makes: the arms whose subject is a bad field cannot reach it through send().
    void publish_raw(uint32_t to, uint32_t from, uint32_t len, uint32_t port, uint8_t fill)
    {
        amp::Ring& r = amp::ring_for(amp::Class::CALL, to, from);
        uint32_t const head = r.head.v.load();
        amp::Slot& s = r.slot[head % amp::RING_SLOTS];
        s.len.store(len);
        s.port.store(port);
        s.tag = TAG_FORGED;
        for (uint32_t i = 0; i < amp::SLOT_BYTES; i++)
        {
            s.payload[i] = fill;
        }
        r.head.v.store(head + 1u);
    }

    // The port the four-node posture names NODE_A (KICKOS_AMP_PORT_NODE_LIST=0,1 beside
    // KICKOS_AMP_PORT_PORT_LIST=2,3), so window_init mints it on NODE_A's row alone.
    constexpr uint32_t PORT_SERVED_A = 2u;

    // A bind for the length of one arm. window_init does NOT reseat the port table, so a bind
    // left standing decides the verdict of every arm that runs after it.
    struct BoundPort
    {
        uint32_t node;
        uint32_t port;

        BoundPort(uint32_t n, uint32_t p, uint16_t endpoint) : node(n), port(p)
        {
            uint32_t const was = fix::g_node;
            fix::g_node = node;
            amp::port_bind(port, endpoint);
            fix::g_node = was;
        }

        ~BoundPort()
        {
            uint32_t const was = fix::g_node;
            fix::g_node = node;
            amp::port_bind(port, amp::EP_BOUND_NONE);
            fix::g_node = was;
        }
    };

    void service_as(uint32_t me)
    {
        uint32_t const was = fix::g_node;
        fix::g_node = me;
        amp::node_service();
        fix::g_node = was;
    }

    // THE DOORBELL ITSELF, so an arm can run the passes a raise really produces and no others.
    // The seam records every raise as a core mask and a node index IS its core bit under this
    // fixture's posture, so the pending mask names nodes directly. `defer_mask` leaves a
    // node's latched bell pending, which is the whole of "the peer drains its replies late".
    //
    // BOUNDED, and the bound is the point: a mechanism that never rings makes an arm read a
    // failed claim rather than hang, and one that rings forever reads as the bound reached.
    constexpr uint32_t PUMP_PASSES_MAX = 64u;

    template <typename Handler>
    uint32_t pump_doorbells(uint32_t defer_mask, Handler handle)
    {
        uint32_t passes = 0;
        while (passes < PUMP_PASSES_MAX)
        {
            uint32_t const pending = fix::g_sent_mask & ~defer_mask;
            if (pending == 0u)
            {
                break;
            }
            fix::g_sent_mask = fix::g_sent_mask & ~pending;
            for (uint32_t n = 0; n < amp::NODE_MAX; n++)
            {
                if ((pending & (1u << n)) == 0u)
                {
                    continue;
                }
                handle(n);
                passes++;
            }
        }
        return passes;
    }

    uint32_t reply_ring_used(uint32_t to, uint32_t from)
    {
        amp::Ring const& r = amp::ring_for(amp::Class::REPLY, to, from);
        return r.head.v.load() - r.tail.v.load();
    }

    struct AmpWindow : public ::testing::Test
    {
        void SetUp() override { fix::reset(); }
        void TearDown() override { fix::reset(); }
    };

    TEST_F(AmpWindow, echo_crosses_and_leaves_the_ring_empty)
    {
        uint8_t const payload[5] = {0x10u, 0x11u, 0x12u, 0x13u, 0x14u};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, payload, 5u));
        EXPECT_EQ(1u, fix::g_sends);
        EXPECT_EQ(1u << NODE_A, fix::g_sent_mask);

        Taken t;
        ASSERT_EQ(amp::Verdict::TOOK, take_as(NODE_A, NODE_B, t.buf, &t.len, &t.port));
        EXPECT_EQ(5u, t.len);
        EXPECT_EQ(amp::PORT_ECHO, t.port);
        for (uint32_t i = 0; i < 5u; i++)
        {
            EXPECT_EQ(payload[i], t.buf[i]) << "payload byte " << i;
        }
        // Past the message: the copy is bounded by the length, not by the slot.
        EXPECT_EQ(OUT_FILL, t.buf[5]);

        Taken again;
        EXPECT_EQ(amp::Verdict::EMPTY, take_as(NODE_A, NODE_B, again.buf, &again.len,
                                               &again.port));
        EXPECT_TRUE(again.untouched());
    }

    TEST_F(AmpWindow, ring_holds_exactly_ring_slots)
    {
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            uint8_t const payload[1] = {static_cast<uint8_t>(0x40u + i)};
            EXPECT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, payload, 1u))
                << "send " << i;
        }
        uint8_t const over[1] = {0xFFu};
        EXPECT_EQ(amp::Sent::FULL, send_as(NODE_B, NODE_A, amp::PORT_ECHO, over, 1u));
        EXPECT_EQ(amp::RING_SLOTS, amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).head.v.load());

        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            Taken t;
            ASSERT_EQ(amp::Verdict::TOOK, take_as(NODE_A, NODE_B, t.buf, &t.len, &t.port))
                << "take " << i;
            EXPECT_EQ(1u, t.len);
            EXPECT_EQ(static_cast<uint8_t>(0x40u + i), t.buf[0]) << "order at " << i;
        }
        Taken drained;
        EXPECT_EQ(amp::Verdict::EMPTY, take_as(NODE_A, NODE_B, drained.buf, &drained.len,
                                               &drained.port));
    }

    TEST_F(AmpWindow, indices_wrap_past_the_mask)
    {
        uint32_t const rounds = amp::RING_SLOTS * 3u + 1u;
        for (uint32_t i = 0; i < rounds; i++)
        {
            uint8_t const payload[2] = {static_cast<uint8_t>(i), static_cast<uint8_t>(i ^ 0x5Au)};
            ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, payload, 2u))
                << "round " << i;
            Taken t;
            ASSERT_EQ(amp::Verdict::TOOK, take_as(NODE_A, NODE_B, t.buf, &t.len, &t.port))
                << "round " << i;
            EXPECT_EQ(2u, t.len) << "round " << i;
            EXPECT_EQ(payload[0], t.buf[0]) << "round " << i;
            EXPECT_EQ(payload[1], t.buf[1]) << "round " << i;
        }
        EXPECT_EQ(rounds, amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).tail.v.load());
    }

    TEST_F(AmpWindow, receive_refuses_a_far_head_deeper_than_the_ring)
    {
        amp::Ring& r = amp::ring_for(amp::Class::CALL, NODE_A, NODE_B);
        ASSERT_EQ(0u, r.tail.v.load());
        r.head.v.store(amp::RING_SLOTS + 1u);

        uint32_t const was_depth = amp::counts(NODE_A).depth;
        uint32_t const was_took = amp::counts(NODE_A).took;
        Taken t;
        EXPECT_EQ(amp::Verdict::DEPTH, take_as(NODE_A, NODE_B, t.buf, &t.len, &t.port));
        EXPECT_TRUE(t.untouched());
        EXPECT_EQ(0u, r.tail.v.load());
        EXPECT_EQ(was_depth + 1u, amp::counts(NODE_A).depth);
        EXPECT_EQ(was_took, amp::counts(NODE_A).took);
    }

    TEST_F(AmpWindow, send_refuses_a_far_tail_deeper_than_the_ring)
    {
        amp::Ring& r = amp::ring_for(amp::Class::CALL, NODE_A, NODE_B);
        ASSERT_EQ(0u, r.head.v.load());
        // Modular: a tail this far behind a head of zero names RING_SLOTS + 1 outstanding.
        r.tail.v.store(0u - (amp::RING_SLOTS + 1u));
        r.slot[0].payload[0] = 0x5Cu;

        uint32_t const was_refused = amp::counts(NODE_B).send_refused;
        uint32_t const was_sent = amp::counts(NODE_B).sent;
        uint8_t const payload[4] = {1u, 2u, 3u, 4u};
        EXPECT_EQ(amp::Sent::DEPTH, send_as(NODE_B, NODE_A, amp::PORT_ECHO, payload, 4u));
        EXPECT_EQ(0u, r.head.v.load());
        EXPECT_EQ(0u, r.slot[0].len.load());
        EXPECT_EQ(amp::PORT_MAX, r.slot[0].port.load());
        EXPECT_EQ(0x5Cu, r.slot[0].payload[0]);
        EXPECT_EQ(was_refused + 1u, amp::counts(NODE_B).send_refused);
        EXPECT_EQ(was_sent, amp::counts(NODE_B).sent);
        EXPECT_EQ(0u, fix::g_sends);
    }

    TEST_F(AmpWindow, an_overlong_slot_is_dropped_and_the_ring_survives)
    {
        publish_raw(NODE_A, NODE_B, amp::SLOT_BYTES + 1u, amp::PORT_ECHO, 0x77u);

        uint32_t const was_length = amp::counts(NODE_A).length;
        Taken bad;
        EXPECT_EQ(amp::Verdict::LENGTH, take_as(NODE_A, NODE_B, bad.buf, &bad.len, &bad.port));
        EXPECT_TRUE(bad.untouched());
        EXPECT_EQ(was_length + 1u, amp::counts(NODE_A).length);
        // The drop is deliberate: a slot left standing would wedge the ring for good.
        EXPECT_EQ(1u, amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).tail.v.load());

        uint8_t const payload[3] = {0x21u, 0x22u, 0x23u};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, payload, 3u));
        Taken good;
        ASSERT_EQ(amp::Verdict::TOOK, take_as(NODE_A, NODE_B, good.buf, &good.len, &good.port));
        EXPECT_EQ(3u, good.len);
        EXPECT_EQ(amp::PORT_ECHO, good.port);
        EXPECT_EQ(payload[0], good.buf[0]);
        EXPECT_EQ(payload[2], good.buf[2]);
    }

    TEST_F(AmpWindow, a_slot_naming_no_minted_port_is_dropped)
    {
        publish_raw(NODE_A, NODE_B, 4u, PORT_UNMINTED, 0x66u);
        publish_raw(NODE_A, NODE_B, 4u, amp::PORT_MAX, 0x65u);

        uint32_t const was_port = amp::counts(NODE_A).port;
        uint32_t const was_took = amp::counts(NODE_A).took;
        Taken inside;
        EXPECT_EQ(amp::Verdict::PORT, take_as(NODE_A, NODE_B, inside.buf, &inside.len,
                                              &inside.port));
        EXPECT_TRUE(inside.untouched());
        EXPECT_EQ(1u, amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).tail.v.load());

        Taken beyond;
        EXPECT_EQ(amp::Verdict::PORT, take_as(NODE_A, NODE_B, beyond.buf, &beyond.len,
                                              &beyond.port));
        EXPECT_TRUE(beyond.untouched());
        EXPECT_EQ(2u, amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).tail.v.load());

        EXPECT_EQ(was_port + 2u, amp::counts(NODE_A).port);
        EXPECT_EQ(was_took, amp::counts(NODE_A).took);

        uint8_t const payload[1] = {0x33u};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, payload, 1u));
        Taken good;
        EXPECT_EQ(amp::Verdict::TOOK, take_as(NODE_A, NODE_B, good.buf, &good.len, &good.port));
        EXPECT_EQ(0x33u, good.buf[0]);
    }

    TEST_F(AmpWindow, a_zero_length_message_is_took_and_not_empty)
    {
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_REPLY, nullptr, 0u));

        Taken t;
        ASSERT_EQ(amp::Verdict::TOOK,
                  take_reply_as(NODE_A, NODE_B, t.buf, &t.len, &t.port, &t.tag));
        EXPECT_EQ(0u, t.len);
        EXPECT_EQ(amp::PORT_REPLY, t.port);
        // Nothing was copied, the length being what bounds the copy.
        EXPECT_EQ(OUT_FILL, t.buf[0]);
    }

    TEST_F(AmpWindow, port_minted_is_total)
    {
        EXPECT_TRUE(amp::port_minted(NODE_A, amp::PORT_ECHO));
        EXPECT_TRUE(amp::port_minted(NODE_B, amp::PORT_REPLY));
        EXPECT_FALSE(amp::port_minted(NODE_A, PORT_UNMINTED));

        EXPECT_FALSE(amp::port_minted(NODE_A, amp::PORT_MAX));
        EXPECT_FALSE(amp::port_minted(NODE_A, 0xFFFFFFFFu));
        EXPECT_FALSE(amp::port_minted(amp::NODE_MAX, amp::PORT_ECHO));
        EXPECT_FALSE(amp::port_minted(0xFFFFFFFFu, amp::PORT_ECHO));
    }

    TEST_F(AmpWindow, the_sender_is_the_ring_and_not_a_field)
    {
        uint8_t const to_b[1] = {0xB1u};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_A, NODE_B, amp::PORT_ECHO, to_b, 1u));
        uint8_t const to_a[1] = {0xA1u};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, to_a, 1u));

        Taken wrong;
        EXPECT_EQ(amp::Verdict::EMPTY, take_as(NODE_A, NODE_C, wrong.buf, &wrong.len,
                                               &wrong.port));
        EXPECT_TRUE(wrong.untouched());

        // Past the last node: `from` is bounded before it indexes, and one past the row is a
        // ring this arm has deliberately filled.
        Taken past;
        EXPECT_EQ(amp::Verdict::EMPTY, take_as(NODE_A, amp::NODE_MAX, past.buf, &past.len,
                                               &past.port));
        EXPECT_TRUE(past.untouched());

        Taken right;
        ASSERT_EQ(amp::Verdict::TOOK, take_as(NODE_A, NODE_B, right.buf, &right.len,
                                              &right.port));
        EXPECT_EQ(1u, right.len);
        EXPECT_EQ(0xA1u, right.buf[0]);
    }

    TEST_F(AmpWindow, send_refuses_the_local_node)
    {
        uint32_t const was_refused = amp::counts(NODE_A).send_refused;
        uint8_t const payload[2] = {0x41u, 0x42u};
        EXPECT_EQ(amp::Sent::NODE, send_as(NODE_A, NODE_A, amp::PORT_ECHO, payload, 2u));
        EXPECT_EQ(was_refused + 1u, amp::counts(NODE_A).send_refused);
        // NOTHING PUBLISHED AND NO DOORBELL. node_service skips its own self-ring, so four
        // accepted self-sends would fill it and no verdict would ever say so.
        EXPECT_EQ(0u, amp::ring_for(amp::Class::CALL, NODE_A, NODE_A).head.v.load());
        EXPECT_EQ(0u, fix::g_sends);
        // The take side answers for the same ring: EMPTY, not a read of a ring nothing drains.
        Taken t;
        EXPECT_EQ(amp::Verdict::EMPTY, take_as(NODE_A, NODE_A, t.buf, &t.len, &t.port));
        EXPECT_TRUE(t.untouched());
    }

    // A refused depth does not advance the tail, so the refusal alone would hand a far side
    // the ring for the life of the image. The strike bound is what makes it temporary.
    TEST_F(AmpWindow, a_wedged_ring_recovers_after_the_strike_bound)
    {
        amp::Ring& r = amp::ring_for(amp::Class::CALL, NODE_A, NODE_C);
        // Seat the strike row: it is static in the real translation unit and fix::reset()
        // cannot reach it, and any take that is not DEPTH clears it.
        uint8_t const seed[1] = {0x5Au};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_C, NODE_A, amp::PORT_ECHO, seed, 1u));
        Taken clear;
        ASSERT_EQ(amp::Verdict::TOOK, take_as(NODE_A, NODE_C, clear.buf, &clear.len,
                                              &clear.port));

        uint32_t const was_reset = amp::counts(NODE_A).depth_reset;
        uint32_t const was_depth = amp::counts(NODE_A).depth;
        uint32_t const tail = r.tail.v.load();
        r.head.v.store(tail + amp::RING_SLOTS + 1u);
        for (uint32_t i = 0; i < amp::DEPTH_STRIKES; i++)
        {
            Taken t;
            EXPECT_EQ(amp::Verdict::DEPTH, take_as(NODE_A, NODE_C, t.buf, &t.len, &t.port));
            EXPECT_TRUE(t.untouched());
        }
        EXPECT_EQ(was_depth + amp::DEPTH_STRIKES, amp::counts(NODE_A).depth);
        EXPECT_EQ(was_reset + 1u, amp::counts(NODE_A).depth_reset);
        // The tail took the far head, so the ring reads EMPTY rather than dead. The far index
        // reached the tail and nothing else: it is spent modulo RING_SLOTS wherever it lands.
        EXPECT_EQ(r.head.v.load(), r.tail.v.load());
        Taken empty;
        EXPECT_EQ(amp::Verdict::EMPTY, take_as(NODE_A, NODE_C, empty.buf, &empty.len,
                                               &empty.port));

        // And a far side that goes back to publishing properly is served again.
        uint8_t const payload[2] = {0x31u, 0x32u};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_C, NODE_A, amp::PORT_ECHO, payload, 2u));
        Taken good;
        ASSERT_EQ(amp::Verdict::TOOK, take_as(NODE_A, NODE_C, good.buf, &good.len,
                                              &good.port));
        EXPECT_EQ(2u, good.len);
        EXPECT_EQ(0x31u, good.buf[0]);
    }

    // The strike bound is per RING and not per ordered pair: node_service runs the depth
    // clause on the call ring whatever that ring holds, so a bound shared between the two
    // classes is cleared on every service pass and the reply ring never reaches its own. N6f
    // states that bound as the whole of a wedged reply ring's recovery.
    TEST_F(AmpWindow, a_call_take_does_not_spend_the_reply_rings_strike_bound)
    {
        amp::Ring& reply = amp::ring_for(amp::Class::REPLY, NODE_A, NODE_B);

        // Seat the reply ring's strike row: it is static in the real translation unit and
        // fix::reset() cannot reach it. An EMPTY reply ring answers before the depth clause,
        // so only a take that really reaches one clears it.
        uint8_t const seed[1] = {0x5Au};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_REPLY, seed, 1u));
        Taken seeded;
        ASSERT_EQ(amp::Verdict::TOOK, take_reply_as(NODE_A, NODE_B, seeded.buf, &seeded.len,
                                                    &seeded.port, &seeded.tag));

        uint32_t const was_reset = amp::counts(NODE_A).depth_reset;
        reply.head.v.store(reply.tail.v.load() + amp::RING_SLOTS + 1u);

        // One strike short of the bound, each followed by the call take node_service makes on
        // the same pair: that ring holds nothing, so its own depth clause is satisfied.
        for (uint32_t i = 1u; i < amp::DEPTH_STRIKES; i++)
        {
            Taken t;
            EXPECT_EQ(amp::Verdict::DEPTH, take_reply_as(NODE_A, NODE_B, t.buf, &t.len,
                                                         &t.port, &t.tag));
            EXPECT_TRUE(t.untouched());
            Taken idle;
            EXPECT_EQ(amp::Verdict::EMPTY, take_as(NODE_A, NODE_B, idle.buf, &idle.len,
                                                   &idle.port));
        }
        EXPECT_EQ(was_reset, amp::counts(NODE_A).depth_reset);

        // The bound's own strike, which none of the call takes above may have spent.
        Taken last;
        EXPECT_EQ(amp::Verdict::DEPTH, take_reply_as(NODE_A, NODE_B, last.buf, &last.len,
                                                     &last.port, &last.tag));
        EXPECT_EQ(was_reset + 1u, amp::counts(NODE_A).depth_reset);
        EXPECT_EQ(reply.head.v.load(), reply.tail.v.load());

        // And the ring runs again.
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_REPLY, seed, 1u));
        Taken good;
        EXPECT_EQ(amp::Verdict::TOOK, take_reply_as(NODE_A, NODE_B, good.buf, &good.len,
                                                    &good.port, &good.tag));
        EXPECT_EQ(1u, good.len);
        EXPECT_EQ(0x5Au, good.buf[0]);
    }

    // The other direction of the same keying: strikes the reply ring earned may not
    // resynchronise the call ring. That reset abandons every slot the pair holds and every
    // record with them, so a call ring is resynchronised on its own strikes or on none.
    TEST_F(AmpWindow, reply_ring_strikes_do_not_resynchronise_the_call_ring)
    {
        amp::Ring& call = amp::ring_for(amp::Class::CALL, NODE_A, NODE_B);
        amp::Ring& reply = amp::ring_for(amp::Class::REPLY, NODE_A, NODE_B);

        uint8_t const seed[1] = {0x5Au};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_REPLY, seed, 1u));
        Taken seeded;
        ASSERT_EQ(amp::Verdict::TOOK, take_reply_as(NODE_A, NODE_B, seeded.buf, &seeded.len,
                                                    &seeded.port, &seeded.tag));

        // One held call, so what a resynchronisation of the call ring abandons is something
        // this arm can read off the tail.
        uint8_t const payload[2] = {0x71u, 0x72u};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, payload, 2u));
        uint32_t held = 0;
        ASSERT_EQ(amp::Verdict::TOOK, take_held_as(NODE_A, NODE_B, &held));
        uint32_t const call_tail = call.tail.v.load();

        // The reply ring one strike short of its bound.
        reply.head.v.store(reply.tail.v.load() + amp::RING_SLOTS + 1u);
        for (uint32_t i = 1u; i < amp::DEPTH_STRIKES; i++)
        {
            Taken t;
            EXPECT_EQ(amp::Verdict::DEPTH, take_reply_as(NODE_A, NODE_B, t.buf, &t.len,
                                                         &t.port, &t.tag));
        }

        // A depth on the call ring now is that ring's FIRST and not its fourth.
        uint32_t const was_reset = amp::counts(NODE_A).depth_reset;
        call.head.v.store(call.tail.v.load() + amp::RING_SLOTS + 1u);
        Taken t;
        EXPECT_EQ(amp::Verdict::DEPTH, take_as(NODE_A, NODE_B, t.buf, &t.len, &t.port));
        EXPECT_EQ(was_reset, amp::counts(NODE_A).depth_reset);
        EXPECT_EQ(call_tail, call.tail.v.load());

        // The held slot is still this node's to release, the tail never having left it.
        release_as(NODE_A, NODE_B, held);
        EXPECT_EQ(call_tail + 1u, call.tail.v.load());
    }

    // The tag is the one far field no arm of the window spends, so what it owes is a
    // byte-for-byte crossing and nothing else.
    TEST_F(AmpWindow, a_reply_tag_crosses_byte_for_byte)
    {
        amp::ReplyTag const sent = {0x89ABCDEFu, 0x01234567u};
        uint8_t const payload[3] = {0x51u, 0x52u, 0x53u};
        ASSERT_EQ(amp::Sent::OK,
                  send_tagged_as(NODE_B, NODE_A, amp::PORT_ECHO, sent, payload, 3u));

        Taken t;
        ASSERT_EQ(amp::Verdict::TOOK,
                  take_tagged_as(NODE_A, NODE_B, t.buf, &t.len, &t.port, &t.tag));
        EXPECT_EQ(sent.thread, t.tag.thread);
        EXPECT_EQ(sent.seq, t.tag.seq);
        EXPECT_EQ(3u, t.len);
        EXPECT_EQ(amp::PORT_ECHO, t.port);
    }

    TEST_F(AmpWindow, a_verdict_other_than_took_leaves_the_tag_with_the_other_three)
    {
        // EMPTY: nothing was published at all.
        Taken empty;
        EXPECT_EQ(amp::Verdict::EMPTY,
                  take_tagged_as(NODE_A, NODE_B, empty.buf, &empty.len, &empty.port, &empty.tag));
        EXPECT_TRUE(empty.untouched());

        // LENGTH and PORT: a slot IS there, and it carries a tag distinct from the poison.
        publish_raw(NODE_A, NODE_B, amp::SLOT_BYTES + 1u, amp::PORT_ECHO, 0x77u);
        Taken overlong;
        EXPECT_EQ(amp::Verdict::LENGTH,
                  take_tagged_as(NODE_A, NODE_B, overlong.buf, &overlong.len, &overlong.port,
                                 &overlong.tag));
        EXPECT_TRUE(overlong.untouched());

        publish_raw(NODE_A, NODE_B, 4u, PORT_UNMINTED, 0x66u);
        Taken unminted;
        EXPECT_EQ(amp::Verdict::PORT,
                  take_tagged_as(NODE_A, NODE_B, unminted.buf, &unminted.len, &unminted.port,
                                 &unminted.tag));
        EXPECT_TRUE(unminted.untouched());

        // DEPTH: the far head is one the consumer refuses to believe.
        amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).head.v.store(
            amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).tail.v.load() + amp::RING_SLOTS + 1u);
        Taken deep;
        EXPECT_EQ(amp::Verdict::DEPTH,
                  take_tagged_as(NODE_A, NODE_B, deep.buf, &deep.len, &deep.port, &deep.tag));
        EXPECT_TRUE(deep.untouched());
    }

    // The reason the tag is handed back rather than left to be re-read, which is the port's
    // reason too: the slot is the producer's again the instant the tail advances.
    TEST_F(AmpWindow, the_tag_is_not_re_readable_from_the_slot_once_the_tail_moved)
    {
        amp::ReplyTag const first = {0x1111AAAAu, 0x2222BBBBu};
        uint8_t const payload[1] = {0x93u};
        ASSERT_EQ(amp::Sent::OK,
                  send_tagged_as(NODE_B, NODE_A, amp::PORT_ECHO, first, payload, 1u));
        Taken t;
        ASSERT_EQ(amp::Verdict::TOOK,
                  take_tagged_as(NODE_A, NODE_B, t.buf, &t.len, &t.port, &t.tag));

        // One ring's worth more, so the last of them lands back on the slot just taken.
        amp::ReplyTag const second = {0x3333CCCCu, 0x4444DDDDu};
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            ASSERT_EQ(amp::Sent::OK,
                      send_tagged_as(NODE_B, NODE_A, amp::PORT_ECHO, second, payload, 1u))
                << "refill " << i;
        }

        amp::Ring const& r = amp::ring_for(amp::Class::CALL, NODE_A, NODE_B);
        EXPECT_EQ(second.thread, r.slot[0].tag.thread);
        EXPECT_EQ(second.seq, r.slot[0].tag.seq);
        // The taker holds its own copy, which the refill did not reach.
        EXPECT_EQ(first.thread, t.tag.thread);
        EXPECT_EQ(first.seq, t.tag.seq);
    }

    // node_service is BOUNDED per sender and per call, and these are what say the bounds do
    // not truncate the honest path: one call empties a FULL ring from every peer. A static
    // ring set cannot exceed either bound, so what an arm can pin is the floor.
    TEST_F(AmpWindow, one_service_call_drains_a_full_ring_from_every_sender)
    {
        uint8_t const payload[3] = {0x71u, 0x72u, 0x73u};
        uint32_t sent = 0;
        for (uint32_t from = 0; from < amp::NODE_MAX; from++)
        {
            if (from == NODE_A)
            {
                continue;
            }
            for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
            {
                ASSERT_EQ(amp::Sent::OK,
                          send_as(from, NODE_A, amp::PORT_ECHO, payload, 3u));
                sent++;
            }
        }
        ASSERT_EQ(amp::RING_SLOTS * (amp::NODE_MAX - 1u), sent);

        uint32_t const was_took = amp::counts(NODE_A).took;
        uint32_t const was_serviced = amp::counts(NODE_A).serviced;
        fix::g_node = NODE_A;
        amp::node_service();
        fix::g_node = 0;
        EXPECT_EQ(was_serviced + 1u, amp::counts(NODE_A).serviced);
        EXPECT_EQ(was_took + sent, amp::counts(NODE_A).took);
        // Every inbox empty, and the echo published back into each sender's own reply ring.
        for (uint32_t from = 0; from < amp::NODE_MAX; from++)
        {
            if (from == NODE_A)
            {
                continue;
            }
            amp::Ring const& in = amp::ring_for(amp::Class::CALL, NODE_A, from);
            EXPECT_EQ(in.head.v.load(), in.tail.v.load()) << "inbox from " << from;
            EXPECT_EQ(amp::RING_SLOTS,
                      amp::ring_for(amp::Class::REPLY, from, NODE_A).head.v.load())
                << "replies to " << from;
        }
    }

    // A REPLY IS ROUTED, NEVER ECHOED, and the endpoint layer is handed the RING it arrived
    // on beside the tag: that ring is the fifth validation clause's whole input, so a route
    // that dropped it would leave any node able to complete any node's call.
    TEST_F(AmpWindow, a_reply_is_routed_to_the_endpoint_layer_with_its_ring_and_tag)
    {
        uint8_t const payload[2] = {0x5Eu, 0x5Fu};
        fix::g_reply_answer = true;
        uint32_t const was_drop = amp::counts(NODE_A).reply_drop;
        ASSERT_EQ(amp::Sent::OK,
                  send_tagged_as(NODE_B, NODE_A, amp::PORT_REPLY, TAG_CARRIED, payload, 2u));
        fix::g_node = NODE_A;
        amp::node_service();
        fix::g_node = 0;
        EXPECT_EQ(1u, fix::g_replies);
        EXPECT_EQ(NODE_B, fix::g_reply_from);
        EXPECT_EQ(TAG_CARRIED.thread, fix::g_reply_thread);
        EXPECT_EQ(TAG_CARRIED.seq, fix::g_reply_seq);
        EXPECT_EQ(2u, fix::g_reply_len);
        EXPECT_EQ(0x5Eu, fix::g_reply_first);
        // Delivered, so nothing is counted, and no echo went back to the sender.
        EXPECT_EQ(was_drop, amp::counts(NODE_A).reply_drop);
        EXPECT_EQ(0u, amp::ring_for(amp::Class::CALL, NODE_B, NODE_A).head.v.load());
    }

    // A REFUSED REPLY IS COUNTED, which is what separates one the validation rejected from
    // one that never arrived.
    TEST_F(AmpWindow, a_reply_the_endpoint_layer_refuses_is_counted_and_dropped)
    {
        uint8_t const payload[1] = {0x60u};
        fix::g_reply_answer = false;
        uint32_t const was_drop = amp::counts(NODE_A).reply_drop;
        ASSERT_EQ(amp::Sent::OK,
                  send_tagged_as(NODE_B, NODE_A, amp::PORT_REPLY, TAG_CARRIED, payload, 1u));
        fix::g_node = NODE_A;
        amp::node_service();
        fix::g_node = 0;
        EXPECT_EQ(1u, fix::g_replies);
        EXPECT_EQ(was_drop + 1u, amp::counts(NODE_A).reply_drop);
        // The tail advanced anyway: a refused reply may not wedge the ring.
        amp::Ring const& in = amp::ring_for(amp::Class::CALL, NODE_A, NODE_B);
        EXPECT_EQ(in.head.v.load(), in.tail.v.load());
    }

    // AN ECHO REACHES THE ENDPOINT LAYER NEVER, whatever the tag it carries.
    TEST_F(AmpWindow, an_echo_is_not_routed_to_the_endpoint_layer)
    {
        uint8_t const payload[1] = {0x61u};
        fix::g_reply_answer = true;
        ASSERT_EQ(amp::Sent::OK,
                  send_tagged_as(NODE_B, NODE_A, amp::PORT_ECHO, TAG_CARRIED, payload, 1u));
        fix::g_node = NODE_A;
        amp::node_service();
        fix::g_node = 0;
        EXPECT_EQ(0u, fix::g_replies);
        EXPECT_EQ(1u, amp::ring_for(amp::Class::REPLY, NODE_B, NODE_A).head.v.load());
    }

    // --- The call ring's slot IS the record ------------------------------------------------
    // Outstanding inbound calls from one peer are RING_SLOTS because the slot is not reclaimed
    // until the reply is sent, and a take that meets the bound refuses WITHOUT moving the tail,
    // so the producer is refused at its own send.

    TEST_F(AmpWindow, a_held_call_does_not_move_the_tail_and_the_bound_refuses_the_next)
    {
        uint8_t const payload[1] = {0x11u};
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, payload, 1u));
        }
        uint32_t slot[amp::RING_SLOTS] = {};
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            ASSERT_EQ(amp::Verdict::TOOK, take_held_as(NODE_A, NODE_B, &slot[i]));
        }
        EXPECT_EQ(0u, amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).tail.v.load());

        EXPECT_EQ(amp::Sent::FULL, send_as(NODE_B, NODE_A, amp::PORT_ECHO, payload, 1u));

        // The consumer never has to refuse for want of a record: held plus unread is the far
        // head over this node's tail, which the depth clause already bounds at RING_SLOTS, so a
        // ring with every slot held has nothing unread in it and the take reads EMPTY.
        uint32_t spare = 0xFFFFFFFFu;
        EXPECT_EQ(amp::Verdict::EMPTY, take_held_as(NODE_A, NODE_B, &spare));
        EXPECT_EQ(0u, amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).tail.v.load());
    }

    TEST_F(AmpWindow, releasing_the_oldest_admits_exactly_one_more)
    {
        uint8_t const payload[1] = {0x22u};
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, payload, 1u));
        }
        uint32_t slot[amp::RING_SLOTS] = {};
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            ASSERT_EQ(amp::Verdict::TOOK, take_held_as(NODE_A, NODE_B, &slot[i]));
        }
        release_as(NODE_A, NODE_B, slot[0]);
        EXPECT_EQ(1u, amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).tail.v.load());

        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, payload, 1u));
        EXPECT_EQ(amp::Sent::FULL, send_as(NODE_B, NODE_A, amp::PORT_ECHO, payload, 1u));
    }

    TEST_F(AmpWindow, a_reply_out_of_order_reclaims_in_order)
    {
        uint8_t const payload[1] = {0x33u};
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, payload, 1u));
        }
        uint32_t slot[amp::RING_SLOTS] = {};
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            ASSERT_EQ(amp::Verdict::TOOK, take_held_as(NODE_A, NODE_B, &slot[i]));
        }
        // The NEWEST released first buys nothing: the oldest is what the tail stands on.
        release_as(NODE_A, NODE_B, slot[amp::RING_SLOTS - 1u]);
        EXPECT_EQ(0u, amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).tail.v.load());

        // The oldest released moves the tail by one and not past the slot still held.
        release_as(NODE_A, NODE_B, slot[0]);
        EXPECT_EQ(1u, amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).tail.v.load());

        // The two between it and the one already released close the whole run at once.
        release_as(NODE_A, NODE_B, slot[1]);
        release_as(NODE_A, NODE_B, slot[2]);
        EXPECT_EQ(amp::RING_SLOTS, amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).tail.v.load());
    }

    // --- A ring carries one class ----------------------------------------------------------
    // Without this a peer publishing replies into the CALL ring takes a record for each, and
    // nothing ever replies to a reply, so those slots are held for the life of the image.

    TEST_F(AmpWindow, a_reply_published_on_the_call_ring_is_refused_and_dropped)
    {
        uint8_t const payload[1] = {0x44u};
        amp::Ring& r = amp::ring_for(amp::Class::CALL, NODE_A, NODE_B);
        amp::Slot& s = r.slot[0];
        s.port.store(amp::PORT_REPLY);
        s.tag = TAG_CARRIED;
        s.len.store(1u);
        s.payload[0] = payload[0];
        r.head.v.store(1u);

        uint32_t const was_class = amp::counts(NODE_A).wrong_class;
        uint32_t held = 0xFFFFFFFFu;
        EXPECT_EQ(amp::Verdict::CLASS, take_held_as(NODE_A, NODE_B, &held));
        // Dropped rather than held: its slot is back and the ring is not one short for good.
        EXPECT_EQ(1u, r.tail.v.load());
        EXPECT_EQ(was_class + 1u, amp::counts(NODE_A).wrong_class);
    }

    TEST_F(AmpWindow, a_call_published_on_the_reply_ring_is_refused_and_dropped)
    {
        amp::Ring& r = amp::ring_for(amp::Class::REPLY, NODE_A, NODE_B);
        amp::Slot& s = r.slot[0];
        s.port.store(amp::PORT_ECHO);
        s.tag = TAG_CARRIED;
        s.len.store(1u);
        s.payload[0] = 0x55u;
        r.head.v.store(1u);

        uint8_t buf[amp::SLOT_BYTES];
        uint32_t len = 0;
        uint32_t port = amp::PORT_MAX;
        amp::ReplyTag tag = {};
        uint32_t const was_class = amp::counts(NODE_A).wrong_class;
        EXPECT_EQ(amp::Verdict::CLASS, take_reply_as(NODE_A, NODE_B, buf, &len, &port, &tag));
        EXPECT_EQ(1u, r.tail.v.load());
        EXPECT_EQ(was_class + 1u, amp::counts(NODE_A).wrong_class);
    }

    TEST_F(AmpWindow, a_send_lands_on_the_ring_its_port_names)
    {
        uint8_t const payload[1] = {0x66u};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, payload, 1u));
        EXPECT_EQ(1u, amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).head.v.load());
        EXPECT_EQ(0u, amp::ring_for(amp::Class::REPLY, NODE_A, NODE_B).head.v.load());

        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_REPLY, payload, 1u));
        EXPECT_EQ(1u, amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).head.v.load());
        EXPECT_EQ(1u, amp::ring_for(amp::Class::REPLY, NODE_A, NODE_B).head.v.load());
    }

    // A CALL THE ENDPOINT LAYER REFUSED IS ANSWERED, not merely dropped. The seam's
    // endpoint_far_call_deliver answers false for every call, which is what every refusal arm
    // past the receiver's pop looks like from here; the far caller is parked on this answer and
    // under KOS_TIMEOUT_NONE nothing else can wake it.
    TEST_F(AmpWindow, a_call_the_endpoint_layer_refuses_is_answered_with_an_empty_reply)
    {
        BoundPort bound(NODE_A, PORT_SERVED_A, 0u);
        uint8_t const payload[4] = {0x91u, 0x92u, 0x93u, 0x94u};
        ASSERT_EQ(amp::Sent::OK,
                  send_tagged_as(NODE_B, NODE_A, PORT_SERVED_A, TAG_CARRIED, payload, 4u));

        amp::Ring& back = amp::ring_for(amp::Class::REPLY, NODE_B, NODE_A);
        ASSERT_EQ(0u, back.head.v.load());
        fix::g_sent_mask = 0;
        service_as(NODE_A);

        ASSERT_EQ(1u, back.head.v.load()) << "the refusal published no answer at all";
        amp::Slot const& answered = back.slot[0];
        EXPECT_EQ(0u, answered.len) << "a refusal answers zero bytes, never the request back";
        EXPECT_EQ(static_cast<uint32_t>(amp::PORT_REPLY), answered.port);
        EXPECT_EQ(TAG_CARRIED.thread, answered.tag.thread);
        EXPECT_EQ(TAG_CARRIED.seq, answered.tag.seq);
        // The caller's own doorbell, or the answer sits in a ring with no notice coming.
        EXPECT_EQ(1u << NODE_B, fix::g_sent_mask & (1u << NODE_B));
        // And the call slot came back, so the ring does not fill behind the refusal.
        amp::Ring const& in = amp::ring_for(amp::Class::CALL, NODE_A, NODE_B);
        EXPECT_EQ(in.head.v.load(), in.tail.v.load());
    }

    // EVERY refusal and not the first: the answer belongs to the one exit that decides the
    // slot did not become a record, so a ring's worth of calls owes a ring's worth of answers.
    TEST_F(AmpWindow, every_refused_call_of_a_full_ring_is_answered)
    {
        BoundPort bound(NODE_A, PORT_SERVED_A, 0u);
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            uint8_t const payload[1] = {static_cast<uint8_t>(0xA0u + i)};
            amp::ReplyTag const tag = {0x2000u + i, 0x30000u + i};
            ASSERT_EQ(amp::Sent::OK,
                      send_tagged_as(NODE_B, NODE_A, PORT_SERVED_A, tag, payload, 1u))
                << "call " << i;
        }
        service_as(NODE_A);

        amp::Ring& back = amp::ring_for(amp::Class::REPLY, NODE_B, NODE_A);
        ASSERT_EQ(amp::RING_SLOTS, back.head.v.load());
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            EXPECT_EQ(0u, back.slot[i].len) << "answer " << i;
            EXPECT_EQ(0x2000u + i, back.slot[i].tag.thread) << "answer " << i;
            EXPECT_EQ(0x30000u + i, back.slot[i].tag.seq) << "answer " << i;
        }
        amp::Ring const& in = amp::ring_for(amp::Class::CALL, NODE_A, NODE_B);
        EXPECT_EQ(in.head.v.load(), in.tail.v.load());
    }

    // A MINTED PORT BOUND TO NOTHING is the same wedge and gets the same answer: holding the
    // slot for a service that may arrive fills the ring behind it.
    TEST_F(AmpWindow, a_call_on_a_minted_port_bound_to_nothing_is_answered)
    {
        ASSERT_EQ(amp::EP_BOUND_NONE, amp::port_endpoint(PORT_SERVED_A));
        uint8_t const payload[2] = {0x95u, 0x96u};
        ASSERT_EQ(amp::Sent::OK,
                  send_tagged_as(NODE_B, NODE_A, PORT_SERVED_A, TAG_CARRIED, payload, 2u));
        service_as(NODE_A);

        amp::Ring& back = amp::ring_for(amp::Class::REPLY, NODE_B, NODE_A);
        ASSERT_EQ(1u, back.head.v.load());
        EXPECT_EQ(0u, back.slot[0].len);
        EXPECT_EQ(TAG_CARRIED.thread, back.slot[0].tag.thread);
        EXPECT_EQ(TAG_CARRIED.seq, back.slot[0].tag.seq);
    }

    // The control on the arms above: the one class of taken call that answers its own payload
    // still does, so the single answering exit did not turn every service into a refusal.
    TEST_F(AmpWindow, an_echo_answered_through_the_service_still_carries_its_payload)
    {
        uint8_t const payload[3] = {0x97u, 0x98u, 0x99u};
        ASSERT_EQ(amp::Sent::OK,
                  send_tagged_as(NODE_B, NODE_A, amp::PORT_ECHO, TAG_CARRIED, payload, 3u));
        service_as(NODE_A);

        amp::Ring& back = amp::ring_for(amp::Class::REPLY, NODE_B, NODE_A);
        ASSERT_EQ(1u, back.head.v.load());
        EXPECT_EQ(3u, back.slot[0].len);
        EXPECT_EQ(0x97u, back.slot[0].payload[0]);
        EXPECT_EQ(0x99u, back.slot[0].payload[2]);
        EXPECT_EQ(TAG_CARRIED.seq, back.slot[0].tag.seq);
    }

    // THE VALIDATED REPLY SEQUENCE IS THE WHOLE OF Thread::call_seq. Narrower and a caller's
    // 256th retry brings the sequence round again while it still holds the tag, so a late
    // reply lands on a call the caller has already given up on.
    TEST_F(AmpWindow, a_reply_sequence_carries_the_whole_callers_field)
    {
        EXPECT_EQ(0xFFFFu, amp::REPLY_SEQ_MASK);
        EXPECT_EQ(0x1234u, amp::reply_seq(0xABCD1234u));
        // The bits a narrower width would drop, which is what makes two calls alias.
        EXPECT_NE(amp::reply_seq(0x0034u), amp::reply_seq(0x1234u));
    }

    // A NODE THE PARTITION DOES NOT HOLD HAS NO CORE, and the answer says so rather than
    // naming the primary: every core-indexed backend refuses an index at this width, so the
    // refusal travels and no caller owes a range check of its own.
    TEST_F(AmpWindow, a_node_outside_the_partition_maps_to_no_core)
    {
        EXPECT_EQ(static_cast<uint32_t>(KICKOS_DOORBELL_CORES), amp::core_of(amp::NODE_MAX));
        EXPECT_EQ(static_cast<uint32_t>(KICKOS_DOORBELL_CORES), amp::core_of(0xFFFFFFFFu));
        EXPECT_NE(amp::core_of(NODE_A), amp::core_of(amp::NODE_MAX))
            << "a node the partition does not hold answered the primary's core";
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // OWNERSHIP, not arithmetic: the arm above already covers the refusal. What is asserted
    // here is that producing it costs the peer nothing, the ring the forge drives being the
    // self-ring no node produces into and no service drains. A forged consumer index under a
    // live consumer is what this arm exists to keep out.
    TEST_F(AmpWindow, the_send_forge_leaves_the_peers_ring_and_its_held_record_alone)
    {
        // NODE_B holding a call from NODE_A: a record live on the slot, and BOTH indices off
        // zero, which is what a reset to zero has to be told apart from. The first call is
        // taken and released to move the tail, the second is held.
        uint8_t const payload[4] = {0xC0u, 0xC1u, 0xC2u, 0xC3u};
        uint8_t out[amp::SLOT_BYTES];
        uint32_t len = 0;
        uint32_t port = amp::PORT_MAX;
        uint32_t slot = 0;
        amp::ReplyTag tag = {};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_A, NODE_B, amp::PORT_ECHO, payload, 4u));
        ASSERT_EQ(amp::Verdict::TOOK, take_as(NODE_B, NODE_A, out, &len, &port));

        ASSERT_EQ(amp::Sent::OK, send_as(NODE_A, NODE_B, amp::PORT_ECHO, payload, 4u));
        fix::g_node = NODE_B;
        ASSERT_EQ(amp::Verdict::TOOK,
                  amp::take_call(NODE_A, out, &len, &port, &tag, &slot));
        uint32_t const token = amp::inbound_seat(NODE_A, slot, TAG_CARRIED);
        ASSERT_NE(amp::FAR_RECORD_NONE, token);

        amp::Ring& peer = amp::ring_for(amp::Class::CALL, NODE_B, NODE_A);
        uint32_t const head_was = peer.head.v.load();
        uint32_t const tail_was = peer.tail.v.load();
        ASSERT_NE(0u, head_was);
        ASSERT_NE(0u, tail_was);

        fix::g_node = NODE_A;
        fix::g_sends = 0;
        EXPECT_EQ(amp::Sent::DEPTH, amp::forge_tail_and_send(NODE_B, amp::RING_SLOTS + 1u));

        EXPECT_EQ(head_was, peer.head.v.load())
            << "the forge wrote the head of a ring a peer is reading";
        EXPECT_EQ(tail_was, peer.tail.v.load())
            << "the forge wrote the consumer's own index, which the consumer owns: a peer "
               "observing it resynchronises or discards real traffic";
        EXPECT_NE(nullptr, amp::inbound_at(token))
            << "the forge dropped the record the peer is still serving that call on";
        EXPECT_EQ(0u, fix::g_sends) << "a refused send rang the doorbell";

        amp::Ring& own = amp::ring_for(amp::Class::CALL, NODE_A, NODE_A);
        EXPECT_EQ(0u, own.head.v.load()) << "the forge left its own ring holding a publication";
        EXPECT_EQ(0u, own.tail.v.load());

        fix::g_node = NODE_B;
        amp::inbound_forget(token);
        amp::release_call(NODE_A, slot);
        fix::g_node = NODE_A;
    }
#endif

    // --- A far head that moved BACKWARD -----------------------------------------------------
    // The unread test is modular, so a head that regressed to between the tail and `taken`,
    // a peer restarting its ring, reads as a huge unread count. What the node would then
    // hand out is a slot the producer never wrote: zeroed, that is a well-formed zero-length
    // echo carrying tag 0,0.

    TEST_F(AmpWindow, a_far_head_behind_taken_is_refused_and_the_real_message_still_arrives)
    {
        uint8_t const payload[1] = {0x61u};
        for (uint32_t i = 0; i < 2u; i++)
        {
            ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, payload, 1u));
        }
        uint32_t slot[2] = {};
        for (uint32_t i = 0; i < 2u; i++)
        {
            ASSERT_EQ(amp::Verdict::TOOK, take_held_as(NODE_A, NODE_B, &slot[i]));
        }

        amp::Ring& r = amp::ring_for(amp::Class::CALL, NODE_A, NODE_B);
        ASSERT_EQ(2u, r.head.v.load());
        // BETWEEN the tail and `taken`, which is the shape head - tail cannot see.
        r.head.v.store(1u);

        uint32_t const was_depth = amp::counts(NODE_A).depth;
        uint32_t const was_took = amp::counts(NODE_A).took;
        Taken t;
        EXPECT_EQ(amp::Verdict::DEPTH,
                  take_tagged_as(NODE_A, NODE_B, t.buf, &t.len, &t.port, &t.tag));
        EXPECT_TRUE(t.untouched());
        EXPECT_EQ(was_depth + 1u, amp::counts(NODE_A).depth);
        EXPECT_EQ(was_took, amp::counts(NODE_A).took);
        EXPECT_EQ(0u, r.tail.v.load());

        // `taken` did not run past the head: the peer back at the index it left, the message it
        // publishes there is the one delivered. A cursor that had advanced would skip it.
        r.head.v.store(2u);
        uint8_t const third[1] = {0x62u};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, third, 1u));
        uint32_t got = 0xFFFFFFFFu;
        uint8_t buf[amp::SLOT_BYTES];
        uint32_t len = 0;
        uint32_t port = amp::PORT_MAX;
        amp::ReplyTag tag = {};
        fix::g_node = NODE_A;
        ASSERT_EQ(amp::Verdict::TOOK, amp::take_call(NODE_B, buf, &len, &port, &tag, &got));
        fix::g_node = 0;
        EXPECT_EQ(2u % amp::RING_SLOTS, got);
        EXPECT_EQ(1u, len);
        EXPECT_EQ(amp::PORT_ECHO, port);
        EXPECT_EQ(third[0], buf[0]);
    }

    TEST_F(AmpWindow, a_regressed_head_resynchronises_and_kills_every_held_record)
    {
        uint8_t const payload[1] = {0x63u};
        for (uint32_t i = 0; i < 2u; i++)
        {
            ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, payload, 1u));
        }
        uint32_t token[2] = {};
        for (uint32_t i = 0; i < 2u; i++)
        {
            uint32_t slot = 0;
            ASSERT_EQ(amp::Verdict::TOOK, take_held_as(NODE_A, NODE_B, &slot));
            fix::g_node = NODE_A;
            token[i] = amp::inbound_seat(NODE_B, slot, TAG_CARRIED);
            fix::g_node = 0;
            ASSERT_NE(amp::FAR_RECORD_NONE, token[i]);
        }

        amp::Ring& r = amp::ring_for(amp::Class::CALL, NODE_A, NODE_B);
        amp::Ring const& rr = amp::ring_for(amp::Class::REPLY, NODE_B, NODE_A);
        uint32_t const reply_head_was = rr.head.v.load();
        r.head.v.store(1u);
        uint32_t const was_reset = amp::counts(NODE_A).depth_reset;
        uint32_t const was_unsent = amp::counts(NODE_A).reply_unsent;
        Taken t;
        for (uint32_t i = 0; i < amp::DEPTH_STRIKES; i++)
        {
            EXPECT_EQ(amp::Verdict::DEPTH,
                      take_tagged_as(NODE_A, NODE_B, t.buf, &t.len, &t.port, &t.tag))
                << "strike " << i;
        }
        EXPECT_EQ(was_reset + 1u, amp::counts(NODE_A).depth_reset);
        EXPECT_EQ(1u, r.tail.v.load());

        // EVERY RECORD THE RESYNCHRONISATION ABANDONED IS DEAD, and its generation moved: a
        // record left standing would answer the next call landing on the same masked slot.
        EXPECT_EQ(nullptr, amp::inbound_at(token[0]));
        EXPECT_EQ(nullptr, amp::inbound_at(token[1]));

        // AND EVERY CALLER THOSE RECORDS NAMED IS ANSWERED, one empty PORT_REPLY each carrying
        // its tag: the far caller has no deadline under KOS_TIMEOUT_NONE, so a record dropped
        // in silence is a thread parked for the life of the image. The answers' own bytes are
        // lost, the service holding each capability being about to find its token dead, which
        // is what reply_unsent counts.
        ASSERT_EQ(reply_head_was + 2u, rr.head.v.load());
        for (uint32_t i = 0; i < 2u; i++)
        {
            amp::Slot const& s = rr.slot[(reply_head_was + i) & (amp::RING_SLOTS - 1u)];
            EXPECT_EQ(0u, s.len.load()) << "answer " << i;
            EXPECT_EQ(amp::PORT_REPLY, s.port.load()) << "answer " << i;
            EXPECT_EQ(TAG_CARRIED.thread, s.tag.thread) << "answer " << i;
            EXPECT_EQ(TAG_CARRIED.seq, s.tag.seq) << "answer " << i;
        }
        EXPECT_EQ(was_unsent + 2u, amp::counts(NODE_A).reply_unsent);

        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, payload, 1u));
        uint32_t fresh_slot = 0;
        ASSERT_EQ(amp::Verdict::TOOK, take_held_as(NODE_A, NODE_B, &fresh_slot));
        EXPECT_EQ(1u % amp::RING_SLOTS, fresh_slot);
        fix::g_node = NODE_A;
        uint32_t const fresh = amp::inbound_seat(NODE_B, fresh_slot, TAG_CARRIED);
        fix::g_node = 0;
        ASSERT_NE(amp::FAR_RECORD_NONE, fresh);
        // The same masked slot, so the same table row: only the generation tells them apart.
        EXPECT_NE(token[1], fresh);
        EXPECT_NE(nullptr, amp::inbound_at(fresh));
        // FORGOTTEN, or this record outlives the arm: fix::reset does not reseat the record
        // table, so a live one left here refuses the seat every later arm asks for at the same
        // masked slot and its verdict then depends on the order GoogleTest ran them in.
        fix::g_node = NODE_A;
        amp::inbound_forget(fresh);
        fix::g_node = 0;
        release_as(NODE_A, NODE_B, fresh_slot);
    }

    // --- A reply slot is reserved before a call is taken -------------------------------------
    // A call slot is freed at reply PUBLISH, not at reply take, so the reply ring's occupancy
    // is NOT bounded by the call slots this node holds. Without an admission test a peer with
    // more callers than RING_SLOTS gets a reply refused at publish, and a refused reply is a
    // loss no path can retry.

    TEST_F(AmpWindow, a_call_is_left_unread_while_its_reply_has_no_slot)
    {
        // The reply ring toward NODE_B full of replies NODE_B has not drained.
        uint8_t const answer[1] = {0x70u};
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            ASSERT_EQ(amp::Sent::OK,
                      send_as(NODE_A, NODE_B, amp::PORT_REPLY, answer, 1u)) << "reply " << i;
        }
        uint8_t const call[2] = {0x71u, 0x72u};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, call, 2u));

        uint32_t const was_reserve = amp::counts(NODE_A).reply_reserve;
        uint32_t const was_took = amp::counts(NODE_A).took;
        Taken t;
        EXPECT_EQ(amp::Verdict::RESERVE,
                  take_tagged_as(NODE_A, NODE_B, t.buf, &t.len, &t.port, &t.tag));
        EXPECT_TRUE(t.untouched());
        EXPECT_EQ(was_reserve + 1u, amp::counts(NODE_A).reply_reserve);
        EXPECT_EQ(was_took, amp::counts(NODE_A).took);
        // LEFT UNREAD rather than dropped: the tail did not move and neither did the cursor.
        EXPECT_EQ(0u, amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).tail.v.load());

        // One reply drained at the peer, and the call is admitted with its payload intact.
        uint8_t rbuf[amp::SLOT_BYTES];
        uint32_t rlen = 0;
        uint32_t rport = amp::PORT_MAX;
        amp::ReplyTag rtag = {};
        ASSERT_EQ(amp::Verdict::TOOK,
                  take_reply_as(NODE_B, NODE_A, rbuf, &rlen, &rport, &rtag));

        Taken good;
        ASSERT_EQ(amp::Verdict::TOOK,
                  take_tagged_as(NODE_A, NODE_B, good.buf, &good.len, &good.port, &good.tag));
        EXPECT_EQ(2u, good.len);
        EXPECT_EQ(amp::PORT_ECHO, good.port);
        EXPECT_EQ(call[0], good.buf[0]);
        EXPECT_EQ(call[1], good.buf[1]);
    }

    // THE POSITIVE FORM OF THE SAME CLAIM, and the one the defect was found by: every call the
    // peer published is ANSWERED. A peer that drains its reply ring late used to lose the reply
    // to the call it published after the ring filled, and then to strand that call instead.
    //
    // NOT ONE SERVICE PASS IS TAKEN BY HAND. Every pass below is one pump_doorbells found a
    // raise for, so an arm that passes says a real doorbell would have carried it. An earlier
    // form of this arm called node_service after the drain and supplied the very edge the tree
    // had no source for, which is what hid the strand.
    TEST_F(AmpWindow, every_call_is_answered_even_when_the_peer_drains_its_replies_late)
    {
        constexpr uint32_t CALLS = amp::RING_SLOTS + 1u;
        uint8_t const body[1] = {0x80u};

        // NODE_B's own doorbell handler: its callers are what a reply wakes, and this fixture
        // has no endpoint layer to wake, so the drain is spelled out and the tag recorded.
        uint32_t seen = 0;
        uint32_t got[CALLS + 1u] = {};
        auto drain_b = [&]() {
            while (seen <= CALLS)
            {
                uint8_t buf[amp::SLOT_BYTES];
                uint32_t len = 0;
                uint32_t port = amp::PORT_MAX;
                amp::ReplyTag tag = {};
                if (take_reply_as(NODE_B, NODE_A, buf, &len, &port, &tag) != amp::Verdict::TOOK)
                {
                    return;
                }
                got[seen] = tag.seq;
                seen++;
            }
        };
        auto handle = [&](uint32_t n) {
            if (n == NODE_B)
            {
                drain_b();
                return;
            }
            service_as(n);
        };

        // NODE_B's bell LEFT PENDING for the whole publishing phase: that is the peer draining
        // late, and it is what fills the reply ring NODE_A must answer into.
        uint32_t passes = 0;
        uint32_t published = 0;
        uint32_t spins = 0;
        while (published < CALLS and spins < 4u * CALLS)
        {
            spins++;
            amp::ReplyTag const tag = {0x900u + published, 0xA00u + published};
            if (send_tagged_as(NODE_B, NODE_A, amp::PORT_ECHO, tag, body, 1u) == amp::Sent::OK)
            {
                published++;
            }
            passes += pump_doorbells(1u << NODE_B, handle);
        }
        ASSERT_EQ(CALLS, published) << "the call ring never took the last publication";
        ASSERT_EQ(0u, seen) << "the peer drained early, so no call was ever held at RESERVE";
        // The state the strand needs: a call left unread and no raise pending for NODE_A.
        ASSERT_EQ(0u, fix::g_sent_mask & (1u << NODE_A));
        ASSERT_NE(amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).head.v.load(),
                  amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).tail.v.load());

        // The peer's latched bell answered at last. NOTHING ELSE IS DRIVEN: the raise the drain
        // makes is the only thing that can bring NODE_A back to the call it refused.
        passes += pump_doorbells(0u, handle);
        ASSERT_LT(passes, PUMP_PASSES_MAX) << "the doorbells never settled";

        ASSERT_EQ(CALLS, seen) << "a call refused at RESERVE was never serviced again";
        for (uint32_t i = 0; i < CALLS; i++)
        {
            EXPECT_EQ(0xA00u + i, got[i]) << "reply " << i << " carries another call's tag";
        }
    }

    // THE MECHANISM, on its own. Taking a reply advances the tail the PUBLISHER measures its
    // room to answer against, so the take owes that publisher a raise.
    TEST_F(AmpWindow, a_reply_taken_rings_the_node_whose_answers_the_slot_belonged_to)
    {
        uint8_t const answer[1] = {0x76u};
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            ASSERT_EQ(amp::Sent::OK,
                      send_as(NODE_A, NODE_B, amp::PORT_REPLY, answer, 1u)) << "reply " << i;
        }
        uint8_t const call[1] = {0x77u};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, call, 1u));
        Taken t;
        ASSERT_EQ(amp::Verdict::RESERVE,
                  take_tagged_as(NODE_A, NODE_B, t.buf, &t.len, &t.port, &t.tag));

        uint8_t rbuf[amp::SLOT_BYTES];
        uint32_t rlen = 0;
        uint32_t rport = amp::PORT_MAX;
        amp::ReplyTag rtag = {};

        // AN EMPTY TAKE MOVES NO TAIL AND MUST RING NOBODY, or two nodes ping-pong on rings
        // that hold nothing.
        fix::g_sends = 0;
        fix::g_sent_mask = 0;
        ASSERT_EQ(amp::Verdict::EMPTY,
                  take_reply_as(NODE_A, NODE_B, rbuf, &rlen, &rport, &rtag));
        EXPECT_EQ(0u, fix::g_sends);

        // And the take that DOES free a slot rings the node that owes the answer.
        ASSERT_EQ(amp::Verdict::TOOK,
                  take_reply_as(NODE_B, NODE_A, rbuf, &rlen, &rport, &rtag));
        EXPECT_EQ(1u, fix::g_sends);
        EXPECT_EQ(1u << NODE_A, fix::g_sent_mask);
    }

    // WHY THE RAISE CANNOT BE GATED ON THE RING GOING FROM FULL TO NOT FULL. The admission test
    // is `used + owed >= RING_SLOTS`, and `owed` is the SERVING node's own state, which the
    // draining node cannot see. So a drain that admits a refused call need never have left a
    // ring the taker saw as full: here the reply ring holds two and the taker owes two, the
    // take is refused, and one drain admits it with the ring's own occupancy never above two.
    TEST_F(AmpWindow, a_drain_admits_a_refused_call_without_the_reply_ring_ever_being_full)
    {
        static_assert(amp::RING_SLOTS >= 4u,
                      "the shape needs two answers published and two owed beside them");
        uint8_t const body[1] = {0x78u};
        uint8_t const answer[2] = {0x79u, 0x7Au};

        // Two calls answered, so two replies sit in the ring and both call slots are back.
        // The answer is published and the slot released by hand, which is what inbound_reply
        // does, so the arm needs no record and cannot be refused a seat by a live one.
        for (uint32_t i = 0; i < 2u; i++)
        {
            ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, body, 1u))
                << "answered call " << i;
            uint32_t slot = 0;
            ASSERT_EQ(amp::Verdict::TOOK, take_held_as(NODE_A, NODE_B, &slot))
                << "answered call " << i;
            ASSERT_EQ(amp::Sent::OK,
                      send_as(NODE_A, NODE_B, amp::PORT_REPLY, answer, sizeof(answer)))
                << "answered call " << i;
            release_as(NODE_A, NODE_B, slot);
        }
        ASSERT_EQ(2u, reply_ring_used(NODE_B, NODE_A));
        ASSERT_EQ(amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).head.v.load(),
                  amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).tail.v.load());

        // Two more taken and HELD, so the taker owes two answers it has not published.
        uint32_t held[2] = {0u, 0u};
        for (uint32_t i = 0; i < 2u; i++)
        {
            ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, body, 1u))
                << "held call " << i;
            ASSERT_EQ(amp::Verdict::TOOK, take_held_as(NODE_A, NODE_B, &held[i]))
                << "held call " << i;
        }

        // The fifth call, refused with the reply ring holding two of four.
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, body, 1u));
        uint32_t const was_reserve = amp::counts(NODE_A).reply_reserve;
        Taken t;
        ASSERT_EQ(amp::Verdict::RESERVE,
                  take_tagged_as(NODE_A, NODE_B, t.buf, &t.len, &t.port, &t.tag));
        EXPECT_EQ(was_reserve + 1u, amp::counts(NODE_A).reply_reserve);
        // THE WHOLE CLAIM: the ring a full-to-not-full gate would watch was never full.
        EXPECT_EQ(2u, reply_ring_used(NODE_B, NODE_A));

        // One drain, and it rings NODE_A even though it left the ring no emptier than
        // not-full.
        uint8_t rbuf[amp::SLOT_BYTES];
        uint32_t rlen = 0;
        uint32_t rport = amp::PORT_MAX;
        amp::ReplyTag rtag = {};
        fix::g_sends = 0;
        fix::g_sent_mask = 0;
        ASSERT_EQ(amp::Verdict::TOOK,
                  take_reply_as(NODE_B, NODE_A, rbuf, &rlen, &rport, &rtag));
        EXPECT_EQ(1u << NODE_A, fix::g_sent_mask);

        Taken good;
        EXPECT_EQ(amp::Verdict::TOOK,
                  take_tagged_as(NODE_A, NODE_B, good.buf, &good.len, &good.port, &good.tag));
        release_as(NODE_A, NODE_B, held[0]);
        release_as(NODE_A, NODE_B, held[1]);
    }

    // A REFUSED ANSWER LOSES ITS BYTES AND NOT ITS CALLER, and the loss is counted apart from
    // the caller's own refused call. The take reserved a slot for this answer, so nothing but a
    // peer that regressed its reply tail under the reservation reaches this; the ring is held
    // full by hand here because no well-behaved producer can present that state.
    TEST_F(AmpWindow, an_answer_the_reply_ring_refuses_is_counted_and_its_caller_is_still_owed)
    {
        uint8_t const body[1] = {0x7Bu};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, body, 1u));
        uint32_t slot = 0;
        ASSERT_EQ(amp::Verdict::TOOK, take_held_as(NODE_A, NODE_B, &slot));
        fix::g_node = NODE_A;
        uint32_t const token = amp::inbound_seat(NODE_B, slot, TAG_CARRIED);
        fix::g_node = 0;
        ASSERT_NE(amp::FAR_RECORD_NONE, token);

        // The reservation withdrawn behind the take: the ring the answer is owed to, full.
        uint8_t const filler[1] = {0x7Cu};
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            ASSERT_EQ(amp::Sent::OK,
                      send_as(NODE_A, NODE_B, amp::PORT_REPLY, filler, 1u)) << "filler " << i;
        }

        uint32_t const was_unsent = amp::counts(NODE_A).reply_unsent;
        uint8_t const answer[2] = {0x7Du, 0x7Eu};
        fix::g_node = NODE_A;
        amp::inbound_reply(token, answer, sizeof(answer));
        fix::g_node = 0;
        EXPECT_EQ(was_unsent + 1u, amp::counts(NODE_A).reply_unsent);
        // THE SLOT IS NOT BACK. It is the record of a caller still owed an answer, and handing
        // it to the peer would let a later call land on it with that obligation standing.
        EXPECT_NE(amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).head.v.load(),
                  amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).tail.v.load());
        // The token dies either way: the reply capability was spent to reach the publication.
        EXPECT_EQ(nullptr, amp::inbound_at(token));
    }

    // AND THE OBLIGATION IS DISCHARGED BY THE RAISE THE PEER'S OWN DRAIN MAKES, which is the
    // only source production has for it: nothing rescans a reply ring's tail. The answer's
    // bytes are gone, so what arrives is the wire's own refusal shape carrying the tag.
    TEST_F(AmpWindow, a_deferred_answer_is_discharged_by_the_raise_the_peers_drain_makes)
    {
        uint8_t const body[1] = {0x82u};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, body, 1u));
        uint32_t slot = 0;
        ASSERT_EQ(amp::Verdict::TOOK, take_held_as(NODE_A, NODE_B, &slot));
        fix::g_node = NODE_A;
        uint32_t const token = amp::inbound_seat(NODE_B, slot, TAG_CARRIED);
        fix::g_node = 0;
        ASSERT_NE(amp::FAR_RECORD_NONE, token);

        uint8_t const filler[1] = {0x83u};
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            ASSERT_EQ(amp::Sent::OK,
                      send_as(NODE_A, NODE_B, amp::PORT_REPLY, filler, 1u)) << "filler " << i;
        }
        uint32_t const was_unsent = amp::counts(NODE_A).reply_unsent;
        uint8_t const answer[2] = {0x84u, 0x85u};
        fix::g_node = NODE_A;
        amp::inbound_reply(token, answer, sizeof(answer));
        fix::g_node = 0;
        ASSERT_EQ(was_unsent + 1u, amp::counts(NODE_A).reply_unsent);

        amp::Ring const& rr = amp::ring_for(amp::Class::REPLY, NODE_B, NODE_A);
        amp::Ring const& rc = amp::ring_for(amp::Class::CALL, NODE_A, NODE_B);
        // NOTHING LATCHED FROM THE SETUP, so one raise drives everything below and the arm
        // supplies no pass of its own.
        fix::g_sent_mask = 0u;
        {
            Taken t;
            ASSERT_EQ(amp::Verdict::TOOK,
                      take_reply_as(NODE_B, NODE_A, t.buf, &t.len, &t.port, &t.tag));
        }
        ASSERT_NE(0u, fix::g_sent_mask & (1u << NODE_A))
            << "the drain rang nobody, so nothing can reach the deferred answer";

        uint32_t const head_was = rr.head.v.load();
        uint32_t const passes = pump_doorbells(0u, [](uint32_t n) { service_as(n); });
        ASSERT_LT(passes, PUMP_PASSES_MAX) << "the doorbells never settled";

        // ONE publication, at the head the deferral left standing, zero length, carrying the
        // record's tag verbatim.
        ASSERT_EQ(head_was + 1u, rr.head.v.load());
        amp::Slot const& s = rr.slot[head_was & (amp::RING_SLOTS - 1u)];
        EXPECT_EQ(0u, s.len.load());
        EXPECT_EQ(amp::PORT_REPLY, s.port.load());
        EXPECT_EQ(TAG_CARRIED.thread, s.tag.thread);
        EXPECT_EQ(TAG_CARRIED.seq, s.tag.seq);
        // And only then is the slot the peer's again.
        EXPECT_EQ(rc.head.v.load(), rc.tail.v.load());
        // The discharge counts no second loss: the bytes were counted lost once.
        EXPECT_EQ(was_unsent + 1u, amp::counts(NODE_A).reply_unsent);
    }

    // --- The deferral's own bound -------------------------------------------------------------
    // THE STATE IT EXISTS FOR, first. An exactly full reply ring is an outstanding count of
    // RING_SLOTS, which is what a well-formed consumer presents while it has read nothing, so
    // tail_believed BELIEVES it and no strike accrues: the producer's resynchronisation is
    // unreachable from here by design, destroying a slow peer's unread answers being worse than
    // waiting for it. So nothing about the two indices can recover this crossing, and a node
    // holding an answer for such a ring owns neither of them.
    TEST_F(AmpWindow, an_exactly_full_reply_ring_answers_FULL_and_accrues_no_tail_strike)
    {
        uint8_t const answer[1] = {0xB0u};
        // CREDIBLE PUBLICATIONS FIRST: the strike count is file-static and fix::reset does not
        // reach it, so these are what make the readings below count from zero.
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            ASSERT_EQ(amp::Sent::OK,
                      send_as(NODE_A, NODE_B, amp::PORT_REPLY, answer, 1u)) << "filler " << i;
        }
        amp::Ring const& rr = amp::ring_for(amp::Class::REPLY, NODE_B, NODE_A);
        uint32_t const head_was = rr.head.v.load();
        uint32_t const was_reset = amp::counts(NODE_A).tail_reset;

        // PAST THE STRIKE BOUND, so an arm that read FULL once cannot pass on a producer that
        // resynchronised at DEPTH_STRIKES: the verdict is the same at every attempt.
        for (uint32_t i = 0; i < amp::DEPTH_STRIKES + 1u; i++)
        {
            EXPECT_EQ(amp::Sent::FULL,
                      send_as(NODE_A, NODE_B, amp::PORT_REPLY, answer, 1u)) << "attempt " << i;
        }
        // NOTHING MOVED: not this node's own head, so the peer's unread answers all stand, and
        // not the counter that would have said one was destroyed.
        EXPECT_EQ(head_was, rr.head.v.load());
        EXPECT_EQ(was_reset, amp::counts(NODE_A).tail_reset);
    }

    // AND THE OBLIGATION IS GIVEN UP AT ITS OWN BOUND, which is what keeps the arm above from
    // being a wedge. NOT ONE SERVICE PASS IS TAKEN BY HAND AND THE PEER NEVER DRAINS: every
    // pass below is a doorbell NODE_C's own traffic raised, which is the only source production
    // has for one while this pair can publish nothing. An arm that supplied the drain instead
    // would pass over the very state that cannot recover without this bound.
    TEST_F(AmpWindow, a_deferred_answer_no_drain_ever_reaches_is_given_up_at_its_own_bound)
    {
        uint8_t const body[1] = {0xB4u};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, body, 1u));
        uint32_t slot = 0;
        ASSERT_EQ(amp::Verdict::TOOK, take_held_as(NODE_A, NODE_B, &slot));
        fix::g_node = NODE_A;
        uint32_t const token = amp::inbound_seat(NODE_B, slot, TAG_CARRIED);
        fix::g_node = 0;
        ASSERT_NE(amp::FAR_RECORD_NONE, token);

        uint8_t const filler[1] = {0xB5u};
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            ASSERT_EQ(amp::Sent::OK,
                      send_as(NODE_A, NODE_B, amp::PORT_REPLY, filler, 1u)) << "filler " << i;
        }
        amp::Ring const& rr = amp::ring_for(amp::Class::REPLY, NODE_B, NODE_A);
        amp::Ring const& rc = amp::ring_for(amp::Class::CALL, NODE_A, NODE_B);
        uint32_t const reply_head_was = rr.head.v.load();
        uint32_t const was_unsent = amp::counts(NODE_A).reply_unsent;
        uint32_t const was_reset = amp::counts(NODE_A).tail_reset;
        uint8_t const answer[2] = {0xB6u, 0xB7u};
        fix::g_node = NODE_A;
        amp::inbound_reply(token, answer, sizeof(answer));
        fix::g_node = 0;
        ASSERT_EQ(was_unsent + 1u, amp::counts(NODE_A).reply_unsent);
        ASSERT_NE(rc.head.v.load(), rc.tail.v.load()) << "the slot was never held back at all";

        // ONE PASS SHORT OF THE BOUND. NODE_B's bell is deferred for the whole arm, so its ring
        // stays exactly full and no drain of it is ever supplied.
        uint8_t const other[1] = {0xB8u};
        uint32_t passes = 0;
        for (uint32_t i = 1u; i < amp::DEFER_PASSES; i++)
        {
            fix::g_sent_mask = 0u;
            ASSERT_EQ(amp::Sent::OK, send_as(NODE_C, NODE_A, amp::PORT_REPLY, other, 1u))
                << "pass " << i;
            passes += pump_doorbells(1u << NODE_B, [](uint32_t n) { service_as(n); });
            EXPECT_NE(rc.head.v.load(), rc.tail.v.load()) << "released early, at pass " << i;
        }

        // AND THE PASS AT THE BOUND: the slot goes back to the peer and the record dies.
        fix::g_sent_mask = 0u;
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_C, NODE_A, amp::PORT_REPLY, other, 1u));
        passes += pump_doorbells(1u << NODE_B, [](uint32_t n) { service_as(n); });
        ASSERT_LT(passes, PUMP_PASSES_MAX) << "the doorbells never settled";
        EXPECT_EQ(rc.head.v.load(), rc.tail.v.load());

        // UNANSWERED AND NOT FORCED THROUGH, which is the whole difference from resynchronising
        // that tail: nothing was published into the peer's ring, its unread answers all stand,
        // and no far index was written.
        EXPECT_EQ(reply_head_was, rr.head.v.load());
        EXPECT_EQ(was_reset, amp::counts(NODE_A).tail_reset);
        // ONE LOSS FOR ONE ANSWER: the bytes were counted at the refusal, and what expiry gives
        // up past them is the wire refusal, which is not a second lost answer.
        EXPECT_EQ(was_unsent + 1u, amp::counts(NODE_A).reply_unsent);

        // AND THE CROSSING IS LIVE AGAIN behind it: the peer reads one answer, and the next call
        // it publishes is taken on the run the release gave back.
        {
            Taken t;
            ASSERT_EQ(amp::Verdict::TOOK,
                      take_reply_as(NODE_B, NODE_A, t.buf, &t.len, &t.port, &t.tag));
        }
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, body, 1u));
        Taken good;
        EXPECT_EQ(amp::Verdict::TOOK,
                  take_tagged_as(NODE_A, NODE_B, good.buf, &good.len, &good.port, &good.tag));
    }

    // --- What a resynchronisation owes an obligation already deferred ------------------------

    TEST_F(AmpWindow, a_resynchronisation_passes_over_an_obligation_already_deferred)
    {
        uint8_t const body[1] = {0xC0u};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, body, 1u));
        uint32_t slot = 0;
        ASSERT_EQ(amp::Verdict::TOOK, take_held_as(NODE_A, NODE_B, &slot));
        fix::g_node = NODE_A;
        uint32_t const token = amp::inbound_seat(NODE_B, slot, TAG_CARRIED);
        fix::g_node = 0;
        ASSERT_NE(amp::FAR_RECORD_NONE, token);

        uint8_t const filler[1] = {0xC1u};
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            ASSERT_EQ(amp::Sent::OK,
                      send_as(NODE_A, NODE_B, amp::PORT_REPLY, filler, 1u)) << "filler " << i;
        }
        uint32_t const was_unsent = amp::counts(NODE_A).reply_unsent;
        uint8_t const answer[2] = {0xC2u, 0xC3u};
        fix::g_node = NODE_A;
        amp::inbound_reply(token, answer, sizeof(answer));
        fix::g_node = 0;
        ASSERT_EQ(was_unsent + 1u, amp::counts(NODE_A).reply_unsent);

        // THE RING GIVEN ROOM, so a second answer for this record COULD be published: what
        // keeps one from being is the record's own state and not a ring with no slot.
        amp::Ring const& rr = amp::ring_for(amp::Class::REPLY, NODE_B, NODE_A);
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            Taken t;
            ASSERT_EQ(amp::Verdict::TOOK,
                      take_reply_as(NODE_B, NODE_A, t.buf, &t.len, &t.port, &t.tag))
                << "drain " << i;
        }
        ASSERT_EQ(0u, reply_ring_used(NODE_B, NODE_A));

        // The resynchronisation, under the deferred obligation. THE HEAD JUMP IS A WHOLE
        // MULTIPLE OF RING_SLOTS, so the tail it adopts masks back onto that record's own slot,
        // which is the case a masked index cannot tell from a fresh call.
        amp::Ring& rc = amp::ring_for(amp::Class::CALL, NODE_A, NODE_B);
        rc.head.v.store(2u * amp::RING_SLOTS);
        uint32_t const head_was = rr.head.v.load();
        uint32_t const was_reset = amp::counts(NODE_A).depth_reset;
        for (uint32_t i = 0; i < amp::DEPTH_STRIKES; i++)
        {
            Taken t;
            EXPECT_EQ(amp::Verdict::DEPTH,
                      take_tagged_as(NODE_A, NODE_B, t.buf, &t.len, &t.port, &t.tag))
                << "strike " << i;
        }
        ASSERT_EQ(was_reset + 1u, amp::counts(NODE_A).depth_reset);
        // NOTHING PUBLISHED AND NOTHING COUNTED for a record that already owed its answer: a
        // second empty reply would answer one caller twice and a second count would read as two
        // answers lost, and the generation bump beside it halves the ABA distance per pass.
        EXPECT_EQ(head_was, rr.head.v.load());
        EXPECT_EQ(was_unsent + 1u, amp::counts(NODE_A).reply_unsent);

        // ONE FRESH CALL TAKEN AND HELD FIRST, on the very masked slot the abandoned record
        // still names. Its own reply is owed, so it is what the discharge below must not
        // hand back.
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, body, 1u));
        uint32_t fresh = 0;
        ASSERT_EQ(amp::Verdict::TOOK, take_held_as(NODE_A, NODE_B, &fresh));
        ASSERT_EQ(slot, fresh) << "the head jump did not mask back onto the abandoned slot";
        uint32_t const call_tail_was = rc.tail.v.load();

        // AND THE OBLIGATION IS INTACT. The peer's own drain above latched the raise this pass
        // rides, and it publishes exactly one empty reply carrying the record's tag, which a
        // resynchronisation that had already answered it could not produce.
        ASSERT_NE(0u, fix::g_sent_mask & (1u << NODE_A));
        uint32_t const passes = pump_doorbells(1u << NODE_B, [](uint32_t n) { service_as(n); });
        ASSERT_LT(passes, PUMP_PASSES_MAX) << "the doorbells never settled";
        ASSERT_EQ(head_was + 1u, rr.head.v.load());
        amp::Slot const& s = rr.slot[head_was & (amp::RING_SLOTS - 1u)];
        EXPECT_EQ(0u, s.len.load());
        EXPECT_EQ(amp::PORT_REPLY, s.port.load());
        EXPECT_EQ(TAG_CARRIED.thread, s.tag.thread);
        EXPECT_EQ(TAG_CARRIED.seq, s.tag.seq);
        EXPECT_EQ(was_unsent + 1u, amp::counts(NODE_A).reply_unsent);
        // AND THAT ANSWER IS ALL THE ABANDONED RECORD WAS OWED. release_call spends a MASKED
        // index against the run that stands NOW, so a discharge that released this record's
        // slot would give the peer back the slot this node is still being served on.
        EXPECT_EQ(call_tail_was, rc.tail.v.load());
        release_as(NODE_A, NODE_B, fresh);
    }

    // AND THE RESERVE WALL COUNTS IT while it stands. The clause above leaves taken = tail =
    // head with the released mask clear, so an obligation of that pair is owed a reply slot no
    // length of held run accounts for: counting the run alone admits one call too many, and the
    // answer to that one is a publication the send then refuses. This is how a GENERIC malformed
    // peer reaches the exactly-full state two arms up, rather than only a targeted one.
    TEST_F(AmpWindow, the_reserve_wall_counts_an_obligation_left_outside_the_held_run)
    {
        uint8_t const body[1] = {0xC8u};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, body, 1u));
        uint32_t slot = 0;
        ASSERT_EQ(amp::Verdict::TOOK, take_held_as(NODE_A, NODE_B, &slot));
        fix::g_node = NODE_A;
        uint32_t const token = amp::inbound_seat(NODE_B, slot, TAG_CARRIED);
        fix::g_node = 0;
        ASSERT_NE(amp::FAR_RECORD_NONE, token);

        uint8_t const filler[1] = {0xC9u};
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            ASSERT_EQ(amp::Sent::OK,
                      send_as(NODE_A, NODE_B, amp::PORT_REPLY, filler, 1u)) << "filler " << i;
        }
        uint8_t const answer[2] = {0xCAu, 0xCBu};
        fix::g_node = NODE_A;
        amp::inbound_reply(token, answer, sizeof(answer));
        fix::g_node = 0;

        // The whole ring given back, so what the wall below refuses is `owed` alone.
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            Taken t;
            ASSERT_EQ(amp::Verdict::TOOK,
                      take_reply_as(NODE_B, NODE_A, t.buf, &t.len, &t.port, &t.tag))
                << "drain " << i;
        }
        ASSERT_EQ(0u, reply_ring_used(NODE_B, NODE_A));

        amp::Ring& rc = amp::ring_for(amp::Class::CALL, NODE_A, NODE_B);
        rc.head.v.store(2u * amp::RING_SLOTS);
        uint32_t const was_reset = amp::counts(NODE_A).depth_reset;
        for (uint32_t i = 0; i < amp::DEPTH_STRIKES; i++)
        {
            Taken t;
            EXPECT_EQ(amp::Verdict::DEPTH,
                      take_tagged_as(NODE_A, NODE_B, t.buf, &t.len, &t.port, &t.tag))
                << "strike " << i;
        }
        ASSERT_EQ(was_reset + 1u, amp::counts(NODE_A).depth_reset);
        ASSERT_EQ(rc.head.v.load(), rc.tail.v.load()) << "the run outlived the reset";

        // A WHOLE RING'S WORTH OF CALLS against a whole ring's worth of free reply slots, and
        // the wall must stop the run ONE SHORT: the obligation the reset left outside owes the
        // last of them.
        uint32_t const was_reserve = amp::counts(NODE_A).reply_reserve;
        uint32_t took = 0;
        uint32_t held[amp::RING_SLOTS] = {};
        amp::Verdict last = amp::Verdict::EMPTY;
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, body, 1u))
                << "call " << i;
            last = take_held_as(NODE_A, NODE_B, &held[took]);
            if (last != amp::Verdict::TOOK)
            {
                break;
            }
            took++;
        }
        EXPECT_EQ(amp::RING_SLOTS - 1u, took);
        EXPECT_EQ(amp::Verdict::RESERVE, last);
        EXPECT_EQ(was_reserve + 1u, amp::counts(NODE_A).reply_reserve);
        for (uint32_t i = 0; i < took; i++)
        {
            release_as(NODE_A, NODE_B, held[i]);
        }
    }

    // --- The producer's own strike bound -----------------------------------------------------
    // The consumer resynchronises a far HEAD it has stopped believing; this is the same bound on
    // the far TAIL, and the index it moves is the one THIS node owns. Without it a peer that
    // regresses its reply tail wedges every answer this node owes it for the life of the image.

    TEST_F(AmpWindow, a_regressed_reply_tail_is_resynchronised_after_the_strike_bound)
    {
        uint8_t const answer[1] = {0x90u};
        // A CREDIBLE publication first: the strike count is file-static and fix::reset does not
        // reach it, so this is what makes the bound below count from zero.
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_A, NODE_B, amp::PORT_REPLY, answer, 1u));

        amp::Ring& rr = amp::ring_for(amp::Class::REPLY, NODE_B, NODE_A);
        // A tail no well-formed consumer stores: it only ever advances, and this node publishes
        // nothing past the ring's depth beyond the tail it last read.
        uint32_t const forged = 0u - (amp::RING_SLOTS + 2u);
        rr.head.v.store(0u);
        rr.tail.v.store(forged);

        uint32_t const was_reset = amp::counts(NODE_A).tail_reset;
        for (uint32_t i = 1; i < amp::DEPTH_STRIKES; i++)
        {
            EXPECT_EQ(amp::Sent::DEPTH,
                      send_as(NODE_A, NODE_B, amp::PORT_REPLY, answer, 1u)) << "strike " << i;
        }
        EXPECT_EQ(was_reset, amp::counts(NODE_A).tail_reset);
        EXPECT_EQ(0u, rr.head.v.load()) << "the head moved before the bound was reached";

        // THE STRIKE THAT REACHES THE BOUND PUBLISHES rather than refusing once more: the
        // answer that got there is the one the recovery exists to deliver.
        EXPECT_EQ(amp::Sent::OK, send_as(NODE_A, NODE_B, amp::PORT_REPLY, answer, 1u));
        EXPECT_EQ(was_reset + 1u, amp::counts(NODE_A).tail_reset);
        // AT the far tail it adopted, and one past it now: a recovery that kept its own head
        // would write a slot the consumer reads as already past.
        EXPECT_EQ(forged + 1u, rr.head.v.load());

        // The payoff, and the one claim a moved index alone does not make: the consumer reads
        // that answer where its own tail stands.
        Taken t;
        EXPECT_EQ(amp::Verdict::TOOK,
                  take_reply_as(NODE_B, NODE_A, t.buf, &t.len, &t.port, &t.tag));
        EXPECT_EQ(1u, t.len);
        EXPECT_EQ(0x90u, t.buf[0]);
    }

    // THE SECOND READER OF THAT TAIL, and the one that makes the recovery reachable at all: a
    // node whose every take is refused for want of a reply slot has nothing of its own to
    // publish, so a bound taken only at the publication would never be reached.
    TEST_F(AmpWindow, a_regressed_reply_tail_is_resynchronised_at_the_admission_test)
    {
        uint8_t const answer[1] = {0x92u};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_A, NODE_B, amp::PORT_REPLY, answer, 1u));

        // A call waiting to be taken, and nothing at all for this node to publish.
        uint8_t const body[1] = {0x93u};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, body, 1u));

        amp::Ring& rr = amp::ring_for(amp::Class::REPLY, NODE_B, NODE_A);
        uint32_t const forged = 0u - (amp::RING_SLOTS + 2u);
        rr.head.v.store(0u);
        rr.tail.v.store(forged);

        uint32_t const was_reset = amp::counts(NODE_A).tail_reset;
        uint32_t slot = 0;
        for (uint32_t i = 1; i < amp::DEPTH_STRIKES; i++)
        {
            EXPECT_EQ(amp::Verdict::RESERVE, take_held_as(NODE_A, NODE_B, &slot))
                << "strike " << i;
        }
        EXPECT_EQ(was_reset, amp::counts(NODE_A).tail_reset);
        EXPECT_EQ(amp::Verdict::TOOK, take_held_as(NODE_A, NODE_B, &slot));
        EXPECT_EQ(was_reset + 1u, amp::counts(NODE_A).tail_reset);
        EXPECT_EQ(forged, rr.head.v.load());
        release_as(NODE_A, NODE_B, slot);
    }

    // AND THE BOUND IS THE REPLY RING'S ALONE. A refused CALL is answered to an application
    // that can act on it, so a wedged call ring costs no caller its answer; discarding an
    // outstanding call would strand a caller already parked on that publication, which nothing
    // on this node could then answer. So this ring stays refused past the bound, twice over.
    TEST_F(AmpWindow, a_regressed_call_ring_tail_is_never_resynchronised_by_the_producer)
    {
        amp::Ring& rc = amp::ring_for(amp::Class::CALL, NODE_B, NODE_A);
        uint32_t const forged = 0u - (amp::RING_SLOTS + 2u);
        rc.head.v.store(0u);
        rc.tail.v.store(forged);

        uint8_t const body[1] = {0x94u};
        uint32_t const was_reset = amp::counts(NODE_A).tail_reset;
        for (uint32_t i = 0; i < 2u * amp::DEPTH_STRIKES; i++)
        {
            EXPECT_EQ(amp::Sent::DEPTH,
                      send_as(NODE_A, NODE_B, amp::PORT_ECHO, body, 1u)) << "attempt " << i;
        }
        EXPECT_EQ(was_reset, amp::counts(NODE_A).tail_reset);
        EXPECT_EQ(0u, rc.head.v.load());
    }

    // A MALFORMED SLOT OWES NO REPLY, so the reserve may not hold one in place: leaving it
    // would let one bad publication wedge the ring for as long as the peer does not drain its
    // replies, which is a denial the receiving node may not accept from the far side.
    TEST_F(AmpWindow, a_malformed_slot_is_dropped_even_when_the_reply_ring_is_full)
    {
        uint8_t const answer[1] = {0x74u};
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            ASSERT_EQ(amp::Sent::OK,
                      send_as(NODE_A, NODE_B, amp::PORT_REPLY, answer, 1u)) << "reply " << i;
        }
        publish_raw(NODE_A, NODE_B, 4u, PORT_UNMINTED, 0x7Au);

        uint32_t const was_port = amp::counts(NODE_A).port;
        uint32_t const was_reserve = amp::counts(NODE_A).reply_reserve;
        Taken t;
        EXPECT_EQ(amp::Verdict::PORT,
                  take_tagged_as(NODE_A, NODE_B, t.buf, &t.len, &t.port, &t.tag));
        EXPECT_TRUE(t.untouched());
        EXPECT_EQ(was_port + 1u, amp::counts(NODE_A).port);
        EXPECT_EQ(was_reserve, amp::counts(NODE_A).reply_reserve);
        // THE ADVANCE IS THE DELIBERATE HALF: the slot is back and the ring is not one short.
        EXPECT_EQ(1u, amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).tail.v.load());
    }

    // And the reserve was not skipped along with the drop: the well-formed call BEHIND the
    // malformed one is still refused, and is still there to be taken once a reply drains.
    TEST_F(AmpWindow, a_well_formed_call_behind_a_dropped_one_still_meets_the_reserve)
    {
        uint8_t const answer[1] = {0x75u};
        for (uint32_t i = 0; i < amp::RING_SLOTS; i++)
        {
            ASSERT_EQ(amp::Sent::OK,
                      send_as(NODE_A, NODE_B, amp::PORT_REPLY, answer, 1u)) << "reply " << i;
        }
        publish_raw(NODE_A, NODE_B, amp::SLOT_BYTES + 1u, amp::PORT_ECHO, 0x7Bu);
        uint8_t const call[2] = {0x7Cu, 0x7Du};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, call, 2u));

        uint32_t const was_length = amp::counts(NODE_A).length;
        Taken bad;
        EXPECT_EQ(amp::Verdict::LENGTH,
                  take_tagged_as(NODE_A, NODE_B, bad.buf, &bad.len, &bad.port, &bad.tag));
        EXPECT_EQ(was_length + 1u, amp::counts(NODE_A).length);
        EXPECT_EQ(1u, amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).tail.v.load());

        uint32_t const was_reserve = amp::counts(NODE_A).reply_reserve;
        Taken held;
        EXPECT_EQ(amp::Verdict::RESERVE,
                  take_tagged_as(NODE_A, NODE_B, held.buf, &held.len, &held.port, &held.tag));
        EXPECT_TRUE(held.untouched());
        EXPECT_EQ(was_reserve + 1u, amp::counts(NODE_A).reply_reserve);
        EXPECT_EQ(1u, amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).tail.v.load());

        // THE CURSOR DID NOT MOVE: one reply drained at the peer and the same call is taken
        // with its payload, rather than skipped for good.
        uint8_t rbuf[amp::SLOT_BYTES];
        uint32_t rlen = 0;
        uint32_t rport = amp::PORT_MAX;
        amp::ReplyTag rtag = {};
        ASSERT_EQ(amp::Verdict::TOOK,
                  take_reply_as(NODE_B, NODE_A, rbuf, &rlen, &rport, &rtag));
        Taken good;
        ASSERT_EQ(amp::Verdict::TOOK,
                  take_tagged_as(NODE_A, NODE_B, good.buf, &good.len, &good.port, &good.tag));
        EXPECT_EQ(2u, good.len);
        EXPECT_EQ(amp::PORT_ECHO, good.port);
        EXPECT_EQ(call[0], good.buf[0]);
        EXPECT_EQ(call[1], good.buf[1]);
    }

    // --- The slot header is spent as it was validated ---------------------------------------
    // Every field of a slot is the far side's writing, and a producer with a stale tail may
    // rewrite one it wrongly believes free. A take loads the length and the port ONCE
    // (tests/static/check_amp_slot_snapshot.sh holds that) and hands them back, so what the
    // copy is bounded by is what was bounded.

    TEST_F(AmpWindow, the_slot_header_handed_back_survives_the_producer_rewriting_the_slot)
    {
        uint8_t const call[3] = {0x91u, 0x92u, 0x93u};
        ASSERT_EQ(amp::Sent::OK, send_as(NODE_B, NODE_A, amp::PORT_ECHO, call, 3u));

        uint8_t buf[amp::SLOT_BYTES];
        for (uint32_t i = 0; i < amp::SLOT_BYTES; i++)
        {
            buf[i] = OUT_FILL;
        }
        uint32_t len = 0;
        uint32_t port = amp::PORT_MAX;
        uint32_t slot = 0;
        amp::ReplyTag tag = {};
        fix::g_node = NODE_A;
        ASSERT_EQ(amp::Verdict::TOOK, amp::take_call(NODE_B, buf, &len, &port, &tag, &slot));
        fix::g_node = 0;

        // The malformed producer, on the slot this node is holding: a length no slot can hold
        // and a port nothing minted.
        amp::Slot& s = amp::ring_for(amp::Class::CALL, NODE_A, NODE_B).slot[slot];
        s.len.store(amp::SLOT_BYTES * 4u);
        s.port.store(PORT_UNMINTED);

        EXPECT_EQ(3u, len);
        EXPECT_EQ(amp::PORT_ECHO, port);
        EXPECT_EQ(TAG_CARRIED.thread, tag.thread);
        EXPECT_EQ(TAG_CARRIED.seq, tag.seq);
        for (uint32_t i = 0; i < 3u; i++)
        {
            EXPECT_EQ(call[i], buf[i]) << "payload byte " << i;
        }
        // Nothing past the validated length was ever copied.
        for (uint32_t i = 3u; i < amp::SLOT_BYTES; i++)
        {
            ASSERT_EQ(OUT_FILL, buf[i]) << "byte " << i << " past the message";
        }
        release_as(NODE_A, NODE_B, slot);
    }
}
