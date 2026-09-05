// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/ampwindow.h>

#if KICKOS_AMP_NODE

#include <kickos/arch/amp_shared.h>

#include <kickos/endpoint.h>
#include <kickos/kruntime.h>

namespace kickos
{
    namespace amp
    {
        namespace
        {
            // THE ONE OBJECT BOTH SIDES WRITE.
            KICKOS_AMP_SHARED("window") Window g_window;

            // Which ports each node has minted. NOT in the window: it is what a far port is
            // validated AGAINST, so a far side able to write it would validate itself. Under
            // the shared image every core of the partition runs this file over this one array,
            // which is why window_init stores a whole row at a time and never a bit at a time.
            uint32_t g_minted[NODE_MAX];

            // Shared, unlike the two records above: a count is a report and no validation
            // reads one, so a peer's row may be that peer's own writing.
            KICKOS_AMP_SHARED("counts") Counts g_counts[NODE_MAX];

            // Consecutive DEPTH verdicts per [class][receiver][sender]. NOT in the window
            // either: it is what bounds how long a far side may keep a ring dead.
            //
            // Per class: node_service runs the depth clause whatever the call ring holds, so a
            // row shared with the reply ring would be cleared on every service pass and that
            // ring's bound never reached (docs/design-multicore.md N6f).
            uint32_t g_depth_strikes[static_cast<unsigned>(Class::CLASS_MAX)][NODE_MAX][NODE_MAX];

            uint32_t& strikes_of(Class cls, uint32_t me, uint32_t from)
            {
                return g_depth_strikes[static_cast<unsigned>(cls)][me][from];
            }

            // This node's own view of each sender's CALL ring, and not in the window: a far
            // side able to write it would decide when its own slots are reclaimed.
            //
            // tail <= taken <= head. `taken` is the next slot to hand to a service; `released`
            // holds one bit per slot from the tail, set when that slot's reply has been sent.
            struct Inbox
            {
                uint32_t taken;
                uint32_t released;
            };
            static_assert(RING_SLOTS <= 32u, "one released bit per slot must fit the mask");
            // Per [receiver][sender]: under the shared image every core runs this file, so a
            // row keyed on the sender alone would be two nodes' view of two different rings.
            Inbox g_inbox[NODE_MAX][NODE_MAX];

            constexpr uint32_t RING_MASK = RING_SLOTS - 1u;

            // One record per slot of every ordered pair this node receives on, keyed as the
            // inbox is.
            Inbound g_inbound[NODE_MAX][NODE_MAX][RING_SLOTS];

            // Which of THIS node's endpoints each of its ports reaches, BIASED BY ONE so that
            // "bound to nothing" is the zero this table already starts at, and keyed by the
            // receiving node as the records are. The bias is what makes the table need no
            // seating: unbiased, a row nobody wrote would read as endpoint 0, which is a real
            // endpoint, and under the shared image a peer core is already draining these rings
            // before kmain could write its row.
            uint16_t g_port_ep1[NODE_MAX][PORT_MAX];

            uint32_t record_index(uint32_t me, uint32_t from, uint32_t slot)
            {
                return ((me * NODE_MAX) + from) * RING_SLOTS + (slot & RING_MASK);
            }

            // A RECORD'S TOKEN: the table index in the low half, the record's generation in
            // the high one. kickos::ThreadPool::far_reply_handle carries the pair through a
            // reply capability, whose own index field is exactly this low half wide.
            constexpr uint32_t RECORD_BITS = 16u;
            constexpr uint32_t RECORD_MASK = (1u << RECORD_BITS) - 1u;
            constexpr uint32_t RECORD_MAX = NODE_MAX * NODE_MAX * RING_SLOTS;
            static_assert(RECORD_MAX < RECORD_MASK,
                          "the index half must exclude the all-ones value, or a token could "
                          "collide with FAR_RECORD_NONE at some generation");
            static_assert(FAR_RECORD_NONE == 0xFFFFFFFFu,
                          "the refusal above is stated against this value");

            uint32_t record_token(uint32_t index, uint16_t gen)
            {
                return (static_cast<uint32_t>(gen) << RECORD_BITS) | index;
            }

            uint32_t token_index(uint32_t token)
            {
                return token & RECORD_MASK;
            }

            uint16_t token_gen(uint32_t token)
            {
                return static_cast<uint16_t>(token >> RECORD_BITS);
            }

            // The record `token` names, or nullptr. Total over the index AND the generation: a
            // token the record has moved past names a slot that died under its holder, and
            // answering with the slot's next tenant would complete a stranger's call.
            Inbound* record_of(uint32_t token)
            {
                uint32_t const index = token_index(token);
                if (index >= RECORD_MAX)
                {
                    return nullptr;
                }
                Inbound& r = (&g_inbound[0][0][0])[index];
                if (r.live == 0u or r.gen != token_gen(token))
                {
                    return nullptr;
                }
                return &r;
            }

            // The generation moves on EVERY death, so a token still naming this record is
            // refused rather than resolving to whatever seats here next.
            void record_drop(Inbound& r)
            {
                r.live = 0u;
                r.gen = static_cast<uint16_t>(r.gen + 1u);
            }

            // For the one event that destroys slots without replying to them: a record exists
            // only for a slot inside the held run, and a resynchronisation abandons all of it.
            void record_drop_pair(uint32_t me, uint32_t from)
            {
                for (uint32_t i = 0; i < RING_SLOTS; i++)
                {
                    if (g_inbound[me][from][i].live != 0u)
                    {
                        record_drop(g_inbound[me][from][i]);
                    }
                }
            }

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

            // The class is a property of the PORT and never of a flag beside it.
            Class class_of(uint32_t port)
            {
                if (port == PORT_REPLY)
                {
                    return Class::REPLY;
                }
                return Class::CALL;
            }

            // True where a reply reached a parked caller.
            bool dispatch_reply(uint32_t from, ReplyTag const& tag, void const* buf, uint32_t len)
            {
                if (endpoint_far_reply_deliver(from, tag, buf, len))
                {
                    return true;
                }
                count_up(g_counts[self()].reply_drop);
                return false;
            }

            // One taken CALL. Its slot is held until this returns, and the caller releases it.
            // TRUE where the slot is now a record and the reply releases it; false where the
            // taker still owns the slot and releases it itself.
            bool dispatch_call(uint32_t from, uint32_t port, ReplyTag const& tag,
                               void const* buf, uint32_t len, uint32_t slot)
            {
                if (port_endpoint(port) != EP_BOUND_NONE)
                {
                    return endpoint_far_call_deliver(from, port, tag, buf, len, slot);
                }
                if (port == PORT_ECHO)
                {
                    (void)send(from, PORT_REPLY, tag, buf, len);
                }
                return false;
            }
        }

        Ring& ring_for(Class cls, uint32_t to, uint32_t from)
        {
            return g_window.inbox[static_cast<unsigned>(cls)][to][from];
        }

        void port_bind(uint32_t port, uint16_t endpoint)
        {
            uint32_t const me = self();
            if (port >= PORT_MAX or me >= NODE_MAX)
            {
                return;
            }
            // EP_BOUND_NONE itself wraps to zero here, so binding the sentinel unbinds.
            g_port_ep1[me][port] = static_cast<uint16_t>(endpoint + 1u);
        }

        uint16_t port_endpoint(uint32_t port)
        {
            uint32_t const me = self();
            if (port >= PORT_MAX or me >= NODE_MAX)
            {
                return EP_BOUND_NONE;
            }
            uint16_t const biased = g_port_ep1[me][port];
            if (biased == 0u)
            {
                return EP_BOUND_NONE;
            }
            return static_cast<uint16_t>(biased - 1u);
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
                // A zero row and not node 0's: the index comes straight from userspace, so a
                // real peer's row returned here would read as that peer's answer.
                static Counts const none = {};
                return none;
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

            Ring& r = ring_for(class_of(port), to, me);
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

            ring(to);
            return Sent::OK;
        }

        namespace
        {
            // The shared half of both takes: the far head believed or refused, with the strike
            // bound that keeps a refusal from owning the ring for the life of the image.
            //
            // A resynchronisation loses every held record of that sender and owns their death.
            // Left standing, such a record refuses a seat to every later call landing on its
            // masked slot, and its holder's release would land one wrap later on a DIFFERENT
            // call whose reply is still owed. The generation is what makes the death visible to
            // a holder this cannot reach.
            bool depth_ok(Class cls, uint32_t me, uint32_t from, Ring& r, uint32_t tail,
                          uint32_t head)
            {
                uint32_t& strikes = strikes_of(cls, me, from);
                if (outstanding(head, tail) <= RING_SLOTS)
                {
                    strikes = 0;
                    return true;
                }
                count_up(g_counts[me].depth);
                // The tail does not move: no slot has been identified to drop, and advancing on
                // an index this node just refused to believe would trust it after all.
                strikes = strikes + 1u;
                if (strikes >= DEPTH_STRIKES)
                {
                    strikes = 0;
                    r.tail.v.store(head);
                    if (cls == Class::CALL)
                    {
                        g_inbox[me][from].taken = head;
                        g_inbox[me][from].released = 0;
                        record_drop_pair(me, from);
                    }
                    count_up(g_counts[me].depth_reset);
                }
                return false;
            }

            // Bounds every field of a slot that this node will spend. The tag is bounded by
            // nothing: no arm of this file spends it.
            Verdict slot_ok(Class cls, uint32_t me, Slot const& s)
            {
                if (s.len > SLOT_BYTES)
                {
                    count_up(g_counts[me].length);
                    return Verdict::LENGTH;
                }
                if (not port_minted(me, s.port))
                {
                    count_up(g_counts[me].port);
                    return Verdict::PORT;
                }
                // A ring carries ONE class. Without this a peer publishing replies into the
                // call ring takes a record for each, and nothing ever replies to a reply, so
                // those slots are held for the life of the image.
                if (class_of(s.port) != cls)
                {
                    count_up(g_counts[me].wrong_class);
                    return Verdict::CLASS;
                }
                return Verdict::TOOK;
            }
        }

        Verdict take_reply(uint32_t from, void* out, uint32_t* out_len, uint32_t* out_port,
                           ReplyTag* out_tag)
        {
            uint32_t const me = self();
            // The local node included: nothing drains a self-ring, so there is nothing in one
            // to take.
            if (from >= NODE_MAX or from == me)
            {
                return Verdict::EMPTY;
            }

            Ring& r = ring_for(Class::REPLY, me, from);
            uint32_t const tail = r.tail.v.load();
            // FAR: the producer owns this index, and it is NEVER used as one.
            uint32_t const head = r.head.v.load();
            if (outstanding(head, tail) == 0u)
            {
                return Verdict::EMPTY;
            }
            if (not depth_ok(Class::REPLY, me, from, r, tail, head))
            {
                return Verdict::DEPTH;
            }

            // The slot index comes from this node's OWN tail.
            Slot const& s = r.slot[tail & RING_MASK];
            Verdict const v = slot_ok(Class::REPLY, me, s);
            if (v != Verdict::TOOK)
            {
                r.tail.v.store(tail + 1u);
                return v;
            }
            uint32_t const len = s.len;
            if (len != 0u)
            {
                kmemcpy(out, s.payload, len);
            }
            *out_len = len;
            *out_port = s.port;
            *out_tag = s.tag;
            r.tail.v.store(tail + 1u);
            count_up(g_counts[me].took);
            return Verdict::TOOK;
        }

        void release_call(uint32_t from, uint32_t slot)
        {
            uint32_t const me = self();
            if (from >= NODE_MAX or from == me)
            {
                return;
            }
            Ring& r = ring_for(Class::CALL, me, from);
            Inbox& ib = g_inbox[me][from];
            uint32_t tail = r.tail.v.load();
            // The held run is tail..taken, so a masked index names exactly one of its slots
            // and an index outside it maps past the run rather than into it.
            uint32_t const held = outstanding(ib.taken, tail);
            uint32_t const off = (slot - tail) & RING_MASK;
            if (off >= held)
            {
                return; // not a slot this node is holding
            }
            ib.released = ib.released | (1u << off);
            while ((ib.released & 1u) != 0u and tail != ib.taken)
            {
                ib.released = ib.released >> 1;
                tail = tail + 1u;
            }
            r.tail.v.store(tail);
        }

        Verdict take_call(uint32_t from, void* out, uint32_t* out_len, uint32_t* out_port,
                          ReplyTag* out_tag, uint32_t* out_slot)
        {
            uint32_t const me = self();
            if (from >= NODE_MAX or from == me)
            {
                return Verdict::EMPTY;
            }

            Ring& r = ring_for(Class::CALL, me, from);
            uint32_t const tail = r.tail.v.load();
            uint32_t const head = r.head.v.load();
            if (not depth_ok(Class::CALL, me, from, r, tail, head))
            {
                return Verdict::DEPTH;
            }

            // Held plus unread is the far head over this node's tail, which the depth clause
            // above bounds at RING_SLOTS: a ring with every slot held has nothing unread.
            Inbox& ib = g_inbox[me][from];
            if (outstanding(head, ib.taken) == 0u)
            {
                return Verdict::EMPTY;
            }
            uint32_t const at = ib.taken;
            Slot const& s = r.slot[at & RING_MASK];
            Verdict const v = slot_ok(Class::CALL, me, s);
            ib.taken = at + 1u;
            if (v != Verdict::TOOK)
            {
                // Dropped, and its slot released at once: a malformed publication may not hold
                // a record, having no reply to release it.
                release_call(from, at & RING_MASK);
                return v;
            }
            uint32_t const len = s.len;
            if (len != 0u)
            {
                kmemcpy(out, s.payload, len);
            }
            *out_len = len;
            *out_port = s.port;
            *out_tag = s.tag;
            *out_slot = at & RING_MASK;
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
            // are not an exit either. What the bounds give up is nothing: a publication made
            // after this call started rings the doorbell itself, and the raise is latched while
            // the handler runs, so the next entry takes it.
            //
            // The REPLY ring first, and that order is the contract: a reply wakes a parked
            // caller and waits on no service thread, so a burst of inbound calls may not
            // delay one.
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
                    Verdict const v = take_reply(from, buf, &len, &port, &tag);
                    if (v == Verdict::EMPTY or v == Verdict::DEPTH)
                    {
                        break;
                    }
                    done++;
                    if (v != Verdict::TOOK)
                    {
                        continue;
                    }
                    (void)dispatch_reply(from, tag, buf, len);
                }
            }

            done = 0;
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
                    uint32_t slot = 0;
                    ReplyTag tag = {};
                    Verdict const v = take_call(from, buf, &len, &port, &tag, &slot);
                    if (v == Verdict::EMPTY or v == Verdict::DEPTH)
                    {
                        break;
                    }
                    done++;
                    if (v != Verdict::TOOK)
                    {
                        continue;
                    }
                    if (not dispatch_call(from, port, tag, buf, len, slot))
                    {
                        release_call(from, slot);
                    }
                }
            }
        }

#if defined(KICKOS_ENABLE_SELFTEST)
        namespace
        {
            // Both indices and this node's own view, so a forge starts from a ring holding
            // nothing. The records go with the run: this abandons the slots they are.
            void forge_reset(Class cls, Ring& r, uint32_t me, uint32_t from)
            {
                r.head.v.store(0u);
                r.tail.v.store(0u);
                strikes_of(cls, me, from) = 0;
                if (cls == Class::CALL)
                {
                    g_inbox[me][from].taken = 0u;
                    g_inbox[me][from].released = 0u;
                    record_drop_pair(me, from);
                }
            }
        }

        Verdict forge_and_take(uint32_t from, uint32_t port, ReplyTag const& tag, uint32_t len,
                               uint32_t head_jump)
        {
            uint32_t const me = self();
            if (from >= NODE_MAX or from == me)
            {
                return Verdict::EMPTY;
            }
            Ring& r = ring_for(class_of(port), me, from);
            forge_reset(class_of(port), r, me, from);

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
            uint32_t got_slot = 0;
            ReplyTag got_tag = {};
            if (class_of(port) == Class::REPLY)
            {
                return take_reply(from, buf, &got_len, &got_port, &got_tag);
            }
            Verdict const v = take_call(from, buf, &got_len, &got_port, &got_tag, &got_slot);
            if (v == Verdict::TOOK)
            {
                release_call(from, got_slot);
            }
            return v;
        }

        bool forge_reply(uint32_t from, ReplyTag const& tag)
        {
            uint32_t const me = self();
            if (from >= NODE_MAX or from == me)
            {
                return false;
            }
            Ring& r = ring_for(Class::REPLY, me, from);
            forge_reset(Class::REPLY, r, me, from);

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
            if (take_reply(from, buf, &got_len, &got_port, &got_tag) != Verdict::TOOK)
            {
                return false;
            }
            return dispatch_reply(from, got_tag, buf, got_len);
        }

        bool forge_publish(uint32_t from, uint32_t port, ReplyTag const& tag)
        {
            uint32_t const me = self();
            if (from >= NODE_MAX or from == me)
            {
                return false;
            }
            Ring& r = ring_for(class_of(port), me, from);
            forge_reset(class_of(port), r, me, from);

            constexpr uint32_t CALL_LEN = 8u;
            Slot& s = r.slot[0];
            s.port = port;
            s.tag = tag;
            s.len = CALL_LEN;
            for (uint32_t i = 0; i < CALL_LEN; i++)
            {
                s.payload[i] = static_cast<uint8_t>(0xB0u + i);
            }
            r.head.v.store(1u);
            return true;
        }

        bool forge_drain_held(uint32_t from)
        {
            uint32_t const me = self();
            if (from >= NODE_MAX or from == me)
            {
                return false;
            }
            Ring& r = ring_for(Class::CALL, me, from);
            uint32_t const tail_was = r.tail.v.load();
            node_service();
            return r.tail.v.load() == tail_was;
        }

        Verdict forge_depth_recovery(uint32_t from)
        {
            uint32_t const me = self();
            if (from >= NODE_MAX or from == me)
            {
                return Verdict::EMPTY;
            }
            Ring& r = ring_for(Class::CALL, me, from);
            forge_reset(Class::CALL, r, me, from);

            // A depth no ring can hold, left standing across every take below.
            r.head.v.store(RING_SLOTS + 2u);
            uint8_t buf[SLOT_BYTES];
            uint32_t got_len = 0;
            uint32_t got_port = PORT_MAX;
            uint32_t got_slot = 0;
            ReplyTag got_tag = {};
            for (uint32_t i = 0; i < DEPTH_STRIKES; i++)
            {
                (void)take_call(from, buf, &got_len, &got_port, &got_tag, &got_slot);
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
            Verdict const v = take_call(from, buf, &got_len, &got_port, &got_tag, &got_slot);
            if (v == Verdict::TOOK)
            {
                release_call(from, got_slot);
            }
            return v;
        }

        Verdict forge_reply_depth_recovery(uint32_t from)
        {
            uint32_t const me = self();
            if (from >= NODE_MAX or from == me)
            {
                return Verdict::EMPTY;
            }
            Ring& r = ring_for(Class::REPLY, me, from);
            forge_reset(Class::REPLY, r, me, from);

            // A depth no ring can hold, left standing across every service below.
            r.head.v.store(RING_SLOTS + 2u);
            // The doorbell's own body, once per strike: it breaks out of this pair's reply
            // drain on the first DEPTH, so a pass is worth exactly one strike, and it then runs
            // the call ring's depth clause for the same pair.
            for (uint32_t i = 0; i < DEPTH_STRIKES; i++)
            {
                node_service();
            }

            // The far side going back to publishing properly: one reply at the head the reset
            // left the tail on. A ring still wedged answers DEPTH here.
            uint32_t const head = r.head.v.load();
            Slot& s = r.slot[head & RING_MASK];
            s.port = PORT_REPLY;
            s.tag = REPLY_TAG_NONE;
            s.len = 1u;
            s.payload[0] = 0x5Au;
            r.head.v.store(head + 1u);

            uint8_t buf[SLOT_BYTES];
            uint32_t got_len = 0;
            uint32_t got_port = PORT_MAX;
            ReplyTag got_tag = {};
            return take_reply(from, buf, &got_len, &got_port, &got_tag);
        }

        namespace
        {
            // One well-formed publication at an ABSOLUTE ring index, and the head moved to
            // match.
            void publish_one(Ring& r, uint32_t at, uint32_t port, uint8_t fill)
            {
                constexpr uint32_t BODY = 4u;
                Slot& s = r.slot[at & RING_MASK];
                s.port = port;
                s.tag = ReplyTag{};
                s.len = BODY;
                for (uint32_t i = 0; i < BODY; i++)
                {
                    s.payload[i] = static_cast<uint8_t>(fill + i);
                }
                r.head.v.store(at + 1u);
            }
        }

        uint32_t forge_reset_record(uint32_t from)
        {
            uint32_t const me = self();
            if (from >= NODE_MAX or from == me)
            {
                return 0u;
            }
            Ring& r = ring_for(Class::CALL, me, from);
            forge_reset(Class::CALL, r, me, from);

            uint8_t buf[SLOT_BYTES];
            uint32_t got_len = 0;
            uint32_t got_port = PORT_MAX;
            uint32_t got_slot = 0;
            ReplyTag got_tag = {};

            // One held call, taken through the real path so its slot really is held: the tail
            // stays behind `taken` and its release is owed.
            publish_one(r, 0u, PORT_ECHO, 0xE0u);
            if (take_call(from, buf, &got_len, &got_port, &got_tag, &got_slot) != Verdict::TOOK)
            {
                return 0u;
            }
            uint32_t const first = inbound_seat(from, got_slot, REPLY_TAG_NONE);
            if (first == FAR_RECORD_NONE)
            {
                return 0u;
            }

            // The resynchronisation, under the held slot: a depth the ring cannot hold, left
            // standing for the whole strike bound.
            //
            // THE HEAD JUMP MUST BE A WHOLE MULTIPLE OF RING_SLOTS. The tail the reset adopts
            // is that head, so the adopted tail masks back onto the slot the record above names
            // at any ring width; anything else is an arm that passes while testing nothing.
            r.head.v.store(2u * RING_SLOTS);
            for (uint32_t i = 0; i < DEPTH_STRIKES; i++)
            {
                (void)take_call(from, buf, &got_len, &got_port, &got_tag, &got_slot);
            }
            uint32_t answer = 0u;
            if (inbound_at(first) == nullptr)
            {
                answer = answer | 1u;
            }

            // The far side publishing properly again, onto the slot the abandoned record names.
            uint32_t const wrapped = r.head.v.load();
            publish_one(r, wrapped, PORT_ECHO, 0xE1u);
            if (take_call(from, buf, &got_len, &got_port, &got_tag, &got_slot) != Verdict::TOOK)
            {
                return answer;
            }
            uint32_t const held = got_slot;
            uint32_t const second = inbound_seat(from, held, REPLY_TAG_NONE);
            if (second != FAR_RECORD_NONE)
            {
                answer = answer | 2u;
                if (second != first)
                {
                    answer = answer | 4u;
                }
            }

            // The abandoned token spent last. The tail is the witness: a release accepted here
            // would hand the peer back the slot the call above is still being served on.
            uint32_t const tail_was = r.tail.v.load();
            uint8_t const body[4] = {0xF0u, 0xF1u, 0xF2u, 0xF3u};
            inbound_reply(first, body, sizeof(body));
            if (r.tail.v.load() == tail_was)
            {
                answer = answer | 8u;
            }

            if (second != FAR_RECORD_NONE)
            {
                inbound_forget(second);
            }
            // Forgotten rather than answered, so the slot stays the taker's to release.
            release_call(from, held);
            forge_reset(Class::CALL, r, me, from);
            return answer | 16u;
        }

        Sent forge_tail_and_send(uint32_t to, uint32_t tail_jump)
        {
            uint32_t const me = self();
            if (to >= NODE_MAX or to == me)
            {
                return Sent::NODE;
            }
            Ring& r = ring_for(Class::CALL, to, me);
            uint32_t const head_was = r.head.v.load();
            uint32_t const tail_was = r.tail.v.load();
            r.head.v.store(0u);
            r.tail.v.store(0u - tail_jump);

            uint8_t pattern[8] = {};
            ReplyTag const tag = {};
            Sent const rc = send(to, PORT_ECHO, tag, pattern, sizeof(pattern));

            r.head.v.store(head_was);
            r.tail.v.store(tail_was);
            return rc;
        }
#endif

        uint32_t inbound_seat(uint32_t from, uint32_t slot, ReplyTag const& tag)
        {
            uint32_t const me = self();
            if (from >= NODE_MAX or me >= NODE_MAX)
            {
                return FAR_RECORD_NONE;
            }
            Inbound& r = g_inbound[me][from][slot & RING_MASK];
            if (r.live != 0u)
            {
                return FAR_RECORD_NONE;
            }
            r.tag = tag;
            r.from = static_cast<uint8_t>(from);
            r.slot = static_cast<uint8_t>(slot & RING_MASK);
            r.live = 1u;
            // The generation is not touched here: it counts deaths, so a seat inherits the
            // count the last death left, which a holder of the dead record does not carry.
            return record_token(record_index(me, from, slot), r.gen);
        }

        Inbound const* inbound_at(uint32_t token)
        {
            return record_of(token);
        }

        void inbound_forget(uint32_t token)
        {
            Inbound* const r = record_of(token);
            if (r == nullptr)
            {
                return;
            }
            record_drop(*r);
        }

        void inbound_reply(uint32_t token, void const* payload, uint32_t len)
        {
            Inbound* const r = record_of(token);
            if (r == nullptr)
            {
                // A resynchronisation ate this record: the masked index it carried names a
                // later wrap's call by now, whose reply is still owed.
                return;
            }
            uint32_t const from = r->from;
            uint32_t const slot = r->slot;
            ReplyTag const tag = r->tag;
            // Freed before the send, so a send that refuses cannot leave the record standing
            // with its slot already gone.
            record_drop(*r);
            (void)send(from, PORT_REPLY, tag, payload, len);
            release_call(from, slot);
        }

        void window_init(void)
        {
            uint32_t const me = self();

            // The mint is the one table-wide write, and each row is stored WHOLE rather than a
            // bit at a time: every node runs this loop over the same build constants, so a
            // complete row is the same row whatever order two nodes' stores land in, while
            // `|=` is a read-modify-write two nodes can interleave into a lost bit.
            for (uint32_t node = 0; node < NODE_MAX; node++)
            {
                // The window layer's own two, on every row.
                uint32_t mask = (1u << PORT_ECHO) | (1u << PORT_REPLY);
                // And the partition's, each on the row of the node that serves it. Peers derive
                // the same rows from the same list, which is what lets a sender validate a far
                // port against its own copy.
                for (uint32_t i = 0; i < PORT_COUNT; i++)
                {
                    if (PORT_NODE[i] == node and PORT_PORT[i] < PORT_MAX)
                    {
                        mask = mask | (1u << PORT_PORT[i]);
                    }
                }
                g_minted[node] = mask;
            }

            // This node's row and no other's. The tail is the only per-node field not already
            // at its identity here, and it lives in the region every node writes: a warm start
            // can present a non-zero one, and adopting it is what stops this node handing out
            // a slot the peer still believes outstanding.
            for (uint32_t from = 0; from < NODE_MAX; from++)
            {
                g_inbox[me][from].taken =
                    g_window.inbox[static_cast<unsigned>(Class::CALL)][me][from].tail.v.load();
                g_inbox[me][from].released = 0u;
            }

            // The peer's half of the bring-up pairing: a sender that found this node unseatable
            // published anyway and skipped only the raise, so what it sent is in a ring with no
            // notice coming. The fence is required and is NOT the one the seating store
            // carries: seating then reading is a store then a load on both sides, and without a
            // full barrier on each the two may both read the older value.
            arch_ipi_fence();
            node_service();
        }
    }
}

extern "C" void kickos_amp_node_service(void)
{
    ::kickos::amp::node_service();
}

#endif
