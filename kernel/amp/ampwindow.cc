// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/ampwindow.h>

#if KICKOS_AMP_NODE

#include <kickos/endpoint.h>
#include <kickos/kruntime.h>

namespace kickos
{
    namespace amp
    {
        namespace
        {
            // THE ONE OBJECT BOTH SIDES WRITE. Everything else in this file is the reading
            // node's own record and no far side reaches it.
            Window g_window;

            // Which ports each node has minted. NOT in the window: it is what a far port is
            // validated AGAINST, so a far side able to write it would validate itself.
            uint32_t g_minted[NODE_MAX];

            Counts g_counts[NODE_MAX];

            // Consecutive DEPTH verdicts per [receiver][sender]. NOT in the window either: it
            // is what bounds how long a far side may keep a ring dead.
            uint32_t g_depth_strikes[NODE_MAX][NODE_MAX];

            constexpr uint32_t RING_MASK = RING_SLOTS - 1u;

            // One writer per row, so a load and a store rather than an increment
            // (tests/static/check_atomic_rmw.sh).
            void count_up(Atomic<uint32_t, Order::RELAXED>& c)
            {
                c.store(c.load() + 1u);
            }

            // How many slots the producer has published that the consumer has not taken.
            // Modular, so a wrap costs nothing. The CALLER decides whether the answer is
            // credible: one of the two indices is always the far side's.
            uint32_t outstanding(uint32_t head, uint32_t tail)
            {
                return head - tail;
            }

            // True where a reply reached a parked caller.
            bool dispatch(uint32_t from, uint32_t port, ReplyTag const& tag, void const* buf,
                          uint32_t len)
            {
                // A reply is never echoed: two nodes would trade the same payload for good.
                if (port == PORT_ECHO)
                {
                    (void)send(from, PORT_REPLY, tag, buf, len);
                    return false;
                }
                if (port != PORT_REPLY)
                {
                    return false;
                }
                if (endpoint_far_reply_deliver(from, tag, buf, len))
                {
                    return true;
                }
                count_up(g_counts[self()].reply_drop);
                return false;
            }
        }

        Ring& ring_for(uint32_t to, uint32_t from)
        {
            return g_window.inbox[to][from];
        }

        void port_mint(uint32_t port)
        {
            if (port >= PORT_MAX)
            {
                return;
            }
            for (uint32_t node = 0; node < NODE_MAX; node++)
            {
                g_minted[node] = g_minted[node] | (1u << port);
            }
        }

        bool port_minted(uint32_t node, uint32_t port)
        {
            if (node >= NODE_MAX or port >= PORT_MAX)
            {
                return false;
            }
            return (g_minted[node] & (1u << port)) != 0u;
        }

        Counts const& counts(uint32_t node)
        {
            if (node >= NODE_MAX)
            {
                return g_counts[0];
            }
            return g_counts[node];
        }

        Sent send(uint32_t to, uint32_t port, ReplyTag const& tag, void const* payload,
                  uint32_t len)
        {
            uint32_t const me = self();
            // The local node included: node_service skips its own self-ring, so a self-send
            // is four slots nothing will ever take.
            if (to >= NODE_MAX or to == me)
            {
                count_up(g_counts[me].send_refused);
                return Sent::NODE;
            }
            if (port >= PORT_MAX)
            {
                count_up(g_counts[me].send_refused);
                return Sent::PORT;
            }
            if (len > SLOT_BYTES)
            {
                count_up(g_counts[me].send_refused);
                return Sent::LENGTH;
            }

            Ring& r = ring_for(to, me);
            uint32_t const head = r.head.v.load();
            // FAR: the consumer owns this index. A producer that believed it would compute a
            // free-slot count out of it and overwrite slots the consumer is still reading.
            uint32_t const tail = r.tail.v.load();
            uint32_t const used = outstanding(head, tail);
            if (used > RING_SLOTS)
            {
                count_up(g_counts[me].send_refused);
                return Sent::DEPTH;
            }
            if (used == RING_SLOTS)
            {
                count_up(g_counts[me].send_refused);
                return Sent::FULL;
            }

            // The slot index comes from the producer's OWN head, never from the far tail.
            Slot& s = r.slot[head & RING_MASK];
            s.port = port;
            s.tag = tag;
            s.len = len;
            if (len != 0u)
            {
                kmemcpy(s.payload, payload, len);
            }
            // Every slot write above must stay above this store: nothing enforces that order,
            // and a peer acquiring this index reads whatever the slot holds when it does.
            r.head.v.store(head + 1u);
            count_up(g_counts[me].sent);

            // A NODE INDEX SPENT AS A CORE MASK, which holds only where node equals core, the
            // shared-image posture. The own-image posture is REFUSED AT CONFIGURE for this
            // line's sake (CMakeLists.txt), so no build reaches it with the two apart.
            arch_ipi_send(1u << to);
            return Sent::OK;
        }

        Verdict take(uint32_t from, void* out, uint32_t* out_len, uint32_t* out_port,
                     ReplyTag* out_tag)
        {
            uint32_t const me = self();
            // The local node included: nothing drains a self-ring, so there is nothing in one
            // to take.
            if (from >= NODE_MAX or from == me)
            {
                return Verdict::EMPTY;
            }

            Ring& r = ring_for(me, from);
            uint32_t const tail = r.tail.v.load();
            // FAR: the producer owns this index, and it is NEVER used as one. What it is used
            // for is a count, and a count that exceeds the ring says the far side is not
            // publishing the way a producer does.
            uint32_t const head = r.head.v.load();
            uint32_t const used = outstanding(head, tail);
            if (used == 0u)
            {
                return Verdict::EMPTY;
            }
            if (used > RING_SLOTS)
            {
                count_up(g_counts[me].depth);
                // The tail does not move: no slot has been identified to drop, and advancing
                // on an index this node just refused to believe would trust it after all.
                // BOUNDED, though: a far side that keeps this depth standing would otherwise
                // own the ring for the life of the image, so after DEPTH_STRIKES the tail is
                // resynchronised and whatever the ring held is lost. The far head reaches the
                // tail here and nothing else, and a tail is only ever spent modulo RING_SLOTS.
                g_depth_strikes[me][from] = g_depth_strikes[me][from] + 1u;
                if (g_depth_strikes[me][from] >= DEPTH_STRIKES)
                {
                    g_depth_strikes[me][from] = 0;
                    r.tail.v.store(head);
                    count_up(g_counts[me].depth_reset);
                }
                return Verdict::DEPTH;
            }
            g_depth_strikes[me][from] = 0;

            // The slot index comes from this node's OWN tail.
            Slot const& s = r.slot[tail & RING_MASK];
            uint32_t const len = s.len;
            uint32_t const port = s.port;
            ReplyTag const tag = s.tag;

            // Both fields checked below are the far side's writing, so each is bounded before
            // it is spent. The tag is bounded by nothing: no arm of this file spends it.
            if (len > SLOT_BYTES)
            {
                r.tail.v.store(tail + 1u);
                count_up(g_counts[me].length);
                return Verdict::LENGTH;
            }
            if (not port_minted(me, port))
            {
                r.tail.v.store(tail + 1u);
                count_up(g_counts[me].port);
                return Verdict::PORT;
            }

            if (len != 0u)
            {
                kmemcpy(out, s.payload, len);
            }
            *out_len = len;
            *out_port = port;
            *out_tag = tag;
            r.tail.v.store(tail + 1u);
            count_up(g_counts[me].took);
            return Verdict::TOOK;
        }

        void node_service(void)
        {
            uint32_t const me = self();
            count_up(g_counts[me].serviced);

            // BOUNDED BOTH WAYS. This body runs with this core's interrupts masked, so a peer
            // refilling its ring as the tail advances would otherwise keep the core in the
            // handler indefinitely, and a malformed slot is dropped and counted, so refusals
            // are not an exit either. What the bounds give up is nothing: a publication made after
            // this call started rings the doorbell itself, and the raise is latched while the
            // handler runs, so the next entry takes it.
            uint32_t done = 0;
            for (uint32_t from = 0; from < NODE_MAX and done < SERVICE_PER_CALL; from++)
            {
                if (from == me)
                {
                    continue;
                }
                for (uint32_t i = 0; i < SERVICE_PER_SENDER and done < SERVICE_PER_CALL; i++)
                {
                    uint8_t buf[SLOT_BYTES];
                    uint32_t len = 0;
                    uint32_t port = PORT_MAX;
                    ReplyTag tag = {};
                    Verdict const v = take(from, buf, &len, &port, &tag);
                    if (v == Verdict::EMPTY or v == Verdict::DEPTH)
                    {
                        break;
                    }
                    done++;
                    if (v != Verdict::TOOK)
                    {
                        continue;
                    }
                    (void)dispatch(from, port, tag, buf, len);
                }
            }
        }

#if defined(KICKOS_ENABLE_SELFTEST)
        Verdict forge_and_take(uint32_t from, uint32_t port, ReplyTag const& tag, uint32_t len,
                               uint32_t head_jump)
        {
            uint32_t const me = self();
            if (from >= NODE_MAX or from == me)
            {
                return Verdict::EMPTY;
            }
            Ring& r = ring_for(me, from);
            r.head.v.store(0u);
            r.tail.v.store(0u);
            g_depth_strikes[me][from] = 0;

            Slot& s = r.slot[0];
            s.port = port;
            s.tag = tag;
            s.len = len;
            for (uint32_t i = 0; i < SLOT_BYTES; i++)
            {
                s.payload[i] = static_cast<uint8_t>(i);
            }
            r.head.v.store(1u + head_jump);

            uint8_t buf[SLOT_BYTES];
            uint32_t got_len = 0;
            uint32_t got_port = PORT_MAX;
            ReplyTag got_tag = {};
            return take(from, buf, &got_len, &got_port, &got_tag);
        }

        bool forge_reply(uint32_t from, ReplyTag const& tag)
        {
            uint32_t const me = self();
            if (from >= NODE_MAX or from == me)
            {
                return false;
            }
            Ring& r = ring_for(me, from);
            r.head.v.store(0u);
            r.tail.v.store(0u);
            g_depth_strikes[me][from] = 0;

            constexpr uint32_t REPLY_LEN = 8u;
            Slot& s = r.slot[0];
            s.port = PORT_REPLY;
            s.tag = tag;
            s.len = REPLY_LEN;
            for (uint32_t i = 0; i < REPLY_LEN; i++)
            {
                s.payload[i] = static_cast<uint8_t>(0xC0u + i);
            }
            r.head.v.store(1u);

            uint8_t buf[SLOT_BYTES];
            uint32_t got_len = 0;
            uint32_t got_port = PORT_MAX;
            ReplyTag got_tag = {};
            if (take(from, buf, &got_len, &got_port, &got_tag) != Verdict::TOOK)
            {
                return false;
            }
            return dispatch(from, got_port, got_tag, buf, got_len);
        }

        Verdict forge_depth_recovery(uint32_t from)
        {
            uint32_t const me = self();
            if (from >= NODE_MAX or from == me)
            {
                return Verdict::EMPTY;
            }
            Ring& r = ring_for(me, from);
            r.head.v.store(0u);
            r.tail.v.store(0u);
            g_depth_strikes[me][from] = 0;

            // A depth no ring can hold, left standing across every take below.
            r.head.v.store(RING_SLOTS + 2u);
            uint8_t buf[SLOT_BYTES];
            uint32_t got_len = 0;
            uint32_t got_port = PORT_MAX;
            ReplyTag got_tag = {};
            for (uint32_t i = 0; i < DEPTH_STRIKES; i++)
            {
                (void)take(from, buf, &got_len, &got_port, &got_tag);
            }

            // The far side going back to publishing properly: one slot at the head the reset
            // left the tail on. A ring still wedged answers DEPTH here.
            uint32_t const head = r.head.v.load();
            Slot& s = r.slot[head & RING_MASK];
            s.port = PORT_ECHO;
            s.tag = ReplyTag{};
            s.len = 1u;
            s.payload[0] = 0x5Au;
            r.head.v.store(head + 1u);
            return take(from, buf, &got_len, &got_port, &got_tag);
        }

        Sent forge_tail_and_send(uint32_t to, uint32_t tail_jump)
        {
            uint32_t const me = self();
            if (to >= NODE_MAX or to == me)
            {
                return Sent::NODE;
            }
            Ring& r = ring_for(to, me);
            r.head.v.store(0u);
            r.tail.v.store(0u - tail_jump);

            uint8_t pattern[8] = {};
            ReplyTag const tag = {};
            Sent const rc = send(to, PORT_ECHO, tag, pattern, sizeof(pattern));

            r.head.v.store(0u);
            r.tail.v.store(0u);
            return rc;
        }
#endif

        void window_init(void)
        {
            port_mint(PORT_ECHO);
            port_mint(PORT_REPLY);
        }
    }
}

extern "C" void kickos_amp_node_service(void)
{
    ::kickos::amp::node_service();
}

#endif
