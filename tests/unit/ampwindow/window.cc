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

    amp::Verdict take_tagged_as(uint32_t me, uint32_t from, void* out, uint32_t* out_len,
                                uint32_t* out_port, amp::ReplyTag* out_tag)
    {
        uint32_t const was = fix::g_node;
        fix::g_node = me;
        amp::Verdict const v = amp::take(from, out, out_len, out_port, out_tag);
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
        amp::Ring& r = amp::ring_for(to, from);
        uint32_t const head = r.head.v.load();
        amp::Slot& s = r.slot[head % amp::RING_SLOTS];
        s.len = len;
        s.port = port;
        s.tag = TAG_FORGED;
        for (uint32_t i = 0; i < amp::SLOT_BYTES; i++)
        {
            s.payload[i] = fill;
        }
        r.head.v.store(head + 1u);
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
        EXPECT_EQ(amp::RING_SLOTS, amp::ring_for(NODE_A, NODE_B).head.v.load());

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
        EXPECT_EQ(rounds, amp::ring_for(NODE_A, NODE_B).tail.v.load());
    }

    TEST_F(AmpWindow, receive_refuses_a_far_head_deeper_than_the_ring)
    {
        amp::Ring& r = amp::ring_for(NODE_A, NODE_B);
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
        amp::Ring& r = amp::ring_for(NODE_A, NODE_B);
        ASSERT_EQ(0u, r.head.v.load());
        // Modular: a tail this far behind a head of zero names RING_SLOTS + 1 outstanding.
        r.tail.v.store(0u - (amp::RING_SLOTS + 1u));
        r.slot[0].payload[0] = 0x5Cu;

        uint32_t const was_refused = amp::counts(NODE_B).send_refused;
        uint32_t const was_sent = amp::counts(NODE_B).sent;
        uint8_t const payload[4] = {1u, 2u, 3u, 4u};
        EXPECT_EQ(amp::Sent::DEPTH, send_as(NODE_B, NODE_A, amp::PORT_ECHO, payload, 4u));
        EXPECT_EQ(0u, r.head.v.load());
        EXPECT_EQ(0u, r.slot[0].len);
        EXPECT_EQ(amp::PORT_MAX, r.slot[0].port);
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
        EXPECT_EQ(1u, amp::ring_for(NODE_A, NODE_B).tail.v.load());

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
        EXPECT_EQ(1u, amp::ring_for(NODE_A, NODE_B).tail.v.load());

        Taken beyond;
        EXPECT_EQ(amp::Verdict::PORT, take_as(NODE_A, NODE_B, beyond.buf, &beyond.len,
                                              &beyond.port));
        EXPECT_TRUE(beyond.untouched());
        EXPECT_EQ(2u, amp::ring_for(NODE_A, NODE_B).tail.v.load());

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
        ASSERT_EQ(amp::Verdict::TOOK, take_as(NODE_A, NODE_B, t.buf, &t.len, &t.port));
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
        EXPECT_EQ(0u, amp::ring_for(NODE_A, NODE_A).head.v.load());
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
        amp::Ring& r = amp::ring_for(NODE_A, NODE_C);
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
        amp::ring_for(NODE_A, NODE_B).head.v.store(
            amp::ring_for(NODE_A, NODE_B).tail.v.load() + amp::RING_SLOTS + 1u);
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

        amp::Ring const& r = amp::ring_for(NODE_A, NODE_B);
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
        static_assert(amp::SERVICE_PER_SENDER >= amp::RING_SLOTS,
                      "a bound below one ring drops a message a full ring already holds");
        static_assert(amp::SERVICE_PER_CALL >= amp::RING_SLOTS * (amp::NODE_MAX - 1u),
                      "a bound below every peer's full ring drops a whole sender's traffic");

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
            amp::Ring const& in = amp::ring_for(NODE_A, from);
            EXPECT_EQ(in.head.v.load(), in.tail.v.load()) << "inbox from " << from;
            EXPECT_EQ(amp::RING_SLOTS, amp::ring_for(from, NODE_A).head.v.load())
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
        EXPECT_EQ(0u, amp::ring_for(NODE_B, NODE_A).head.v.load());
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
        amp::Ring const& in = amp::ring_for(NODE_A, NODE_B);
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
        EXPECT_EQ(1u, amp::ring_for(NODE_B, NODE_A).head.v.load());
    }
}
