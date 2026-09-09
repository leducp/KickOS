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

            // Consecutive incredible far TAILs on the reply ring this node produces into, keyed
            // [receiver][sender]. NOT a cell of g_depth_strikes: under the shared image the cell
            // keyed [REPLY][to][me] is the one node `to` spends as that ring's consumer, so the
            // two sides would clear each other's count.
            uint32_t g_tail_strikes[NODE_MAX][NODE_MAX];

            uint32_t& tail_strikes_of(uint32_t to, uint32_t from)
            {
                return g_tail_strikes[to][from];
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
                r.pending = 0u;
                r.run_gone = 0u;
                r.gen = static_cast<uint16_t>(r.gen + 1u);
            }

            // A record whose answer the reply ring refused. The TOKEN dies as it does in any
            // other death; the record stays live so the call slot it names is not handed back
            // under an answer still owed on it, and so no later call seats on its masked slot.
            void record_defer(Inbound& r)
            {
                r.pending = 1u;
                r.defers = 0u;
                r.gen = static_cast<uint16_t>(r.gen + 1u);
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

            // TRUE where the far tail may be believed, on the reply ring toward `to`. Its two
            // readers are the publication of an answer and the admission test that reserves the
            // slot for one, and they must stop believing it on the same evidence.
            //
            // No well-formed peer can present an outstanding count above the ring's depth: its
            // tail only advances and this node never publishes more than RING_SLOTS past the
            // tail it last read. Adopting a far tail destroys this ring's unread answers, which
            // is what the strike bound is for.
            //
            // Only the index THIS NODE OWNS is written here; the far one it adopts is spent
            // modulo RING_SLOTS alone.
            bool tail_believed(uint32_t me, uint32_t to, Ring& r, uint32_t& head, uint32_t tail)
            {
                uint32_t& strikes = tail_strikes_of(to, me);
                if (outstanding(head, tail) <= RING_SLOTS)
                {
                    strikes = 0;
                    return true;
                }
                strikes = strikes + 1u;
                if (strikes < DEPTH_STRIKES)
                {
                    return false;
                }
                strikes = 0;
                head = tail;
                r.head.v.store(head);
                count_up(g_counts[me].tail_reset);
                return true;
            }

            // WHAT A NODE OWES A CALLER WHOSE ROUTE IT HAS STOPPED BELIEVING. Every live record
            // names a far caller parked on an answer this node is about to destroy, and under
            // KOS_TIMEOUT_NONE that caller has no deadline; the answer owed is the wire's only
            // refusal shape, an empty PORT_REPLY carrying its tag.
            //
            // MUST RUN BEFORE THE INDICES MOVE: the tag comes out of the record, and nothing
            // else on this node can name those callers once the records are gone.
            //
            // A RECORD ALREADY PENDING IS PASSED OVER, answer_deferred owning it from there:
            // answering it here would count one lost answer twice and bump its generation a
            // second time, halving the distance a stale token travels to collide.
            //
            // AND EVERY LIVE RECORD IS MARKED run_gone FIRST, whatever is then decided about
            // it: the caller has ALREADY cleared the run (see Inbound::run_gone).
            void record_answer_pair(uint32_t me, uint32_t from)
            {
                for (uint32_t i = 0; i < RING_SLOTS; i++)
                {
                    Inbound& r = g_inbound[me][from][i];
                    if (r.live == 0u)
                    {
                        continue;
                    }
                    r.run_gone = 1u;
                    if (r.pending != 0u)
                    {
                        continue;
                    }
                    ReplyTag const tag = r.tag;
                    count_up(g_counts[me].reply_unsent);
                    if (send(from, PORT_REPLY, tag, nullptr, 0u) == Sent::OK)
                    {
                        record_drop(r);
                        continue;
                    }
                    // The reply ring toward this caller is unbelievable too. The obligation
                    // outlives the record's token exactly as a refused answer's does, and the
                    // producer's own strike bound is what will make it sendable.
                    record_defer(r);
                }
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
            //
            // EVERY PATH THAT DID NOT BECOME A RECORD PUBLISHES AN ANSWER, AT THIS ONE SITE.
            // The far caller parks until its deadline, and under KOS_TIMEOUT_NONE there is no
            // deadline, so an arm that returned false without publishing leaks that thread for
            // the life of the image. endpoint_far_call_deliver refuses through several arms
            // past the point where its receiver came off recv_waiters and every one of them
            // reaches this bool; an answer written per arm would be a new leak for each arm
            // added.
            bool dispatch_call(uint32_t from, uint32_t port, ReplyTag const& tag,
                               void const* buf, uint32_t len, uint32_t slot)
            {
                uint32_t answer_len = 0u;
                if (port_endpoint(port) != EP_BOUND_NONE)
                {
                    if (endpoint_far_call_deliver(from, port, tag, buf, len, slot))
                    {
                        return true;
                    }
                }
                else if (port == PORT_ECHO)
                {
                    answer_len = len; // the window layer's own service: the payload comes back
                }
                (void)send(from, PORT_REPLY, tag, buf, answer_len);
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

        void count_deliver_fault(void)
        {
            count_up(g_counts[self()].deliver_fault);
        }

#if defined(KICKOS_ENABLE_SELFTEST)
        void app_alive_set(uint32_t mark)
        {
            g_counts[self()].app_alive.store(mark);
        }
#endif

        namespace
        {
            // A send once the ring is chosen. The ring is a parameter so the selftest forge can
            // run this arithmetic over a ring no node produces into and no service drains,
            // rather than over a live peer's.
            Sent send_on(Ring& r, uint32_t me, uint32_t to, uint32_t port, ReplyTag const& tag,
                         void const* payload, uint32_t len)
            {
                uint32_t head = r.head.v.load();
                // FAR: the consumer owns this index. A producer that believed it would compute
                // a free-slot count out of it and overwrite slots the consumer is still
                // reading.
                uint32_t const tail = r.tail.v.load();
                // THE BOUND IS THE REPLY RING'S ALONE (ampwindow.h, DEPTH_STRIKES): a refused
                // call is answered to an application that can act on it, where discarding an
                // outstanding one would strand a caller already parked on its publication.
                if (class_of(port) == Class::REPLY
                    and not tail_believed(me, to, r, head, tail))
                {
                    count_up(g_counts[me].send_refused);
                    return Sent::DEPTH;
                }
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
                s.port.store(port);
                s.tag = tag;
                s.len.store(len);
                if (len != 0u)
                {
                    kmemcpy(s.payload, payload, len);
                }
                // Every slot write above must stay above this store: nothing enforces that
                // order, and a peer acquiring this index reads whatever the slot holds when it
                // does.
                r.head.v.store(head + 1u);
                count_up(g_counts[me].sent);

                ring(to);
                return Sent::OK;
            }
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

            return send_on(ring_for(class_of(port), to, me), me, to, port, tag, payload, len);
        }

        namespace
        {
            // The shared half of both takes: the far head believed or refused, with the strike
            // bound that keeps a refusal from owning the ring for the life of the image.
            //
            // A resynchronisation loses every held record of that sender and owns their death,
            // answering each caller first (record_answer_pair). Left standing, such a record
            // refuses a seat to every later call landing on its masked slot, and its holder's
            // release would land one wrap later on a DIFFERENT call whose reply is still owed.
            // The generation is what makes the death visible to a holder this cannot reach.
            // `deepest` is the largest outstanding count any of this node's cursors reads out
            // of the far head. A head that regressed BEHIND `taken` wraps to a huge count there
            // while head - tail is still small, and the slots it would hand out were never
            // written: a zeroed one reads as a well-formed zero-length echo.
            bool depth_ok(Class cls, uint32_t me, uint32_t from, Ring& r, uint32_t head,
                          uint32_t deepest)
            {
                uint32_t& strikes = strikes_of(cls, me, from);
                if (deepest <= RING_SLOTS)
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
                        record_answer_pair(me, from);
                    }
                    count_up(g_counts[me].depth_reset);
                }
                return false;
            }

            // Bounds the SNAPSHOT of a slot header, never the slot: what is validated here and
            // what the caller spends must be the same words. The tag is bounded by nothing: no
            // arm of this file spends it.
            Verdict slot_ok(Class cls, uint32_t me, uint32_t len, uint32_t port)
            {
                if (len > SLOT_BYTES)
                {
                    count_up(g_counts[me].length);
                    return Verdict::LENGTH;
                }
                if (not port_minted(me, port))
                {
                    count_up(g_counts[me].port);
                    return Verdict::PORT;
                }
                // A ring carries ONE class. Without this a peer publishing replies into the
                // call ring takes a record for each, and nothing ever replies to a reply, so
                // those slots are held for the life of the image.
                if (class_of(port) != cls)
                {
                    count_up(g_counts[me].wrong_class);
                    return Verdict::CLASS;
                }
                return Verdict::TOOK;
            }
        }

        namespace
        {
            // TRUE WHERE ONE MORE CALL MAY BE TAKEN. Every taken call owes exactly one reply
            // and the reply ring toward that sender is the only place it can go, so a take
            // that leaves no slot for its own reply publishes one the send must refuse, and
            // the answer's own bytes are then lost, the reply capability being consumed by
            // then. What survives such a refusal is the obligation alone (inbound_reply).
            //
            // `held` is tail..taken, of which the slots already flagged in `released` have had
            // their reply sent and are counted in the ring's own occupancy instead.
            //
            // AND THE HELD RUN IS NOT THE WHOLE OF WHAT IS OWED. A `run_gone` record is counted
            // beside the run, no length of run accounting for it; a pending record still INSIDE
            // the run is left to the arithmetic below, its slot not released while it stands.
            bool reply_reserved(uint32_t me, uint32_t from, uint32_t held, uint32_t released)
            {
                uint32_t owed = 0;
                uint32_t replied = 0;
                for (uint32_t i = 0; i < RING_SLOTS; i++)
                {
                    Inbound const& r = g_inbound[me][from][i];
                    if (r.pending != 0u and r.run_gone != 0u)
                    {
                        owed = owed + 1u;
                    }
                    if ((released & (1u << i)) != 0u)
                    {
                        replied = replied + 1u;
                    }
                }
                if (held > replied)
                {
                    owed = owed + (held - replied);
                }
                Ring& rr = ring_for(Class::REPLY, from, me);
                uint32_t head = rr.head.v.load();
                // FAR: the peer owns this tail. THE SECOND READER OF IT, and the one that makes
                // the producer's recovery reachable with no answer pending: a node whose every
                // take is refused here has nothing of its own to publish, so a bound taken only
                // at the publication would never be reached.
                uint32_t const tail = rr.tail.v.load();
                if (not tail_believed(me, from, rr, head, tail))
                {
                    return false;
                }
                // Subtracted rather than added to `owed`, which a modular count near the top of
                // the range would carry past.
                uint32_t const used = outstanding(head, tail);
                if (used >= RING_SLOTS)
                {
                    return false;
                }
                return (RING_SLOTS - used) > owed;
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

            // ONE EXIT PAST THIS POINT, so the credit raise below cannot be reached around.
            Verdict v = Verdict::DEPTH;
            if (depth_ok(Class::REPLY, me, from, r, head, outstanding(head, tail)))
            {
                // The slot index comes from this node's OWN tail.
                Slot const& s = r.slot[tail & RING_MASK];
                // ONE LOAD EACH, and every clause below spends the snapshot: a re-load may
                // answer a producer's later writing, so a length re-read after it was bounded
                // is a length nothing bounded.
                uint32_t const len = s.len.load();
                uint32_t const port = s.port.load();
                v = slot_ok(Class::REPLY, me, len, port);
                if (v == Verdict::TOOK)
                {
                    if (len != 0u)
                    {
                        kmemcpy(out, s.payload, len);
                    }
                    *out_len = len;
                    *out_port = port;
                    *out_tag = s.tag;
                    count_up(g_counts[me].took);
                }
                // The copy stays ABOVE this store: the slot is the producer's again the
                // instant it lands.
                r.tail.v.store(tail + 1u);
            }

            // CREDIT RETURN, AND THE ONE RAISE IN THIS FILE WHOSE OMISSION WOULD COST
            // LIVENESS RATHER THAN LATENCY. `from` measures its room to answer against THIS
            // tail (reply_reserved), so an advance here is the only event that can admit a
            // call `from` left unread at RESERVE, and no publication of its own follows to
            // ring it. Every path that advances a reply tail in a live partition runs through
            // here, the depth resynchronisation included.
            //
            // The tail is re-read rather than derived: depth_ok may have adopted the far head.
            if (r.tail.v.load() != tail)
            {
                ring(from);
            }
            return v;
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
            Inbox& ib = g_inbox[me][from];
            uint32_t const tail = r.tail.v.load();
            uint32_t const head = r.head.v.load();
            // BOTH CURSORS, because they fail in different directions: head - tail catches a
            // head too far ahead, head - taken a head that moved backward into the held run.
            uint32_t deepest = outstanding(head, tail);
            uint32_t const unread = outstanding(head, ib.taken);
            if (unread > deepest)
            {
                deepest = unread;
            }
            if (not depth_ok(Class::CALL, me, from, r, head, deepest))
            {
                return Verdict::DEPTH;
            }

            // Held plus unread is the far head over this node's tail, which the depth clause
            // above bounds at RING_SLOTS: a ring with every slot held has nothing unread.
            if (unread == 0u)
            {
                return Verdict::EMPTY;
            }
            uint32_t const at = ib.taken;
            Slot const& s = r.slot[at & RING_MASK];
            // ONE LOAD EACH, as take_reply does and for the same reason.
            uint32_t const len = s.len.load();
            uint32_t const port = s.port.load();
            Verdict const v = slot_ok(Class::CALL, me, len, port);
            if (v != Verdict::TOOK)
            {
                // Dropped, and its slot released at once: a malformed publication may not hold
                // a record, having no reply to release it. UNCONDITIONALLY, and ahead of the
                // reserve below: a malformed slot owes no reply, and leaving one in place would
                // let one bad publication wedge the ring for as long as the peer does not drain.
                ib.taken = at + 1u;
                release_call(from, at & RING_MASK);
                return v;
            }
            // AFTER the verdict, because only a TOOK owes a reply. The cursor has NOT moved and
            // must not: a refusal that advanced it would skip this call for good.
            if (not reply_reserved(me, from, outstanding(at, tail), ib.released))
            {
                count_up(g_counts[me].reply_reserve);
                return Verdict::RESERVE;
            }
            ib.taken = at + 1u;
            if (len != 0u)
            {
                kmemcpy(out, s.payload, len);
            }
            *out_len = len;
            *out_port = port;
            *out_tag = s.tag;
            *out_slot = at & RING_MASK;
            count_up(g_counts[me].took);
            return Verdict::TOOK;
        }

        namespace
        {
            // THE ANSWERS A REFUSED PUBLICATION DEFERRED. What is retained is the obligation and
            // never the payload: an empty PORT_REPLY says "no answer", as true now as at the
            // refusal (docs/design-multicore.md N6e).
            //
            // A refusal here is not counted again: the answer's bytes were counted lost once.
            //
            // AND THE OBLIGATION EXPIRES AT DEFER_PASSES (ampwindow.h): at the bound the slot
            // goes back and the record dies UNANSWERED, which writes no far index and leaves
            // the peer's own unread answers alone.
            void answer_deferred(uint32_t me, uint32_t from)
            {
                for (uint32_t i = 0; i < RING_SLOTS; i++)
                {
                    Inbound& r = g_inbound[me][from][i];
                    if (r.pending == 0u)
                    {
                        continue;
                    }
                    if (send(from, PORT_REPLY, r.tag, nullptr, 0u) != Sent::OK)
                    {
                        r.defers = static_cast<uint8_t>(r.defers + 1u);
                        if (r.defers < DEFER_PASSES)
                        {
                            // RETURN AND NOT CONTINUE: every record of this pair publishes into
                            // the one ring ring_for(REPLY, from, me) names, so a refusal here
                            // refuses the rest this pass too and walking on would spend their
                            // strikes on a verdict already known.
                            return;
                        }
                    }
                    uint32_t const slot = r.slot;
                    bool const gone = r.run_gone != 0u;
                    record_drop(r);
                    // NO RELEASE FOR AN ABANDONED RUN. release_call spends a MASKED index
                    // against the run that stands NOW, so once a resynchronisation has moved
                    // the run, that index names a later wrap's call whose own reply is owed.
                    if (not gone)
                    {
                        release_call(from, slot);
                    }
                }
            }
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

            // BETWEEN THE TWO DRAINS. After the replies, whose drain frees the slot a deferred
            // answer needs; before the calls, whose admission the call slot it gives back pays.
            for (uint32_t from = 0; from < NODE_MAX; from++)
            {
                if (from == me)
                {
                    continue;
                }
                answer_deferred(me, from);
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
                    if (v == Verdict::EMPTY or v == Verdict::DEPTH or v == Verdict::RESERVE)
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
            // forge_reset is the only caller, so this stays inside the selftest guard: an
            // unguarded twin fails -Wunused-function on any AMP node built without the knob.
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

            // Both indices and this node's own view, so a forge starts from a ring holding
            // nothing. The records go with the run: this abandons the slots they are.
            void forge_reset(Class cls, Ring& r, uint32_t me, uint32_t from)
            {
                r.head.v.store(0u);
                r.tail.v.store(0u);
                strikes_of(cls, me, from) = 0;
                // The producer's count for the SAME ring, or a forge run four times finds the
                // bound already reached and reads a recovery as a refusal.
                tail_strikes_of(me, from) = 0;
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
            s.port.store(port);
            s.tag = tag;
            s.len.store(len);
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

        bool forge_reply(uint32_t from, ReplyTag const& tag, uint32_t len)
        {
            uint32_t const me = self();
            if (from >= NODE_MAX or from == me or len > SLOT_BYTES)
            {
                return false;
            }
            Ring& r = ring_for(Class::REPLY, me, from);
            forge_reset(Class::REPLY, r, me, from);

            Slot& s = r.slot[0];
            s.port.store(PORT_REPLY);
            s.tag = tag;
            s.len.store(len);
            for (uint32_t i = 0; i < len; i++)
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

        Verdict forge_class_take(uint32_t from)
        {
            uint32_t const me = self();
            if (from >= NODE_MAX or from == me)
            {
                return Verdict::EMPTY;
            }
            // The port and the length are both well-formed, so neither of their clauses can
            // see this: what is wrong is the RING the publication landed in.
            Ring& r = ring_for(Class::CALL, me, from);
            forge_reset(Class::CALL, r, me, from);

            Slot& s = r.slot[0];
            s.port.store(PORT_REPLY);
            s.tag = REPLY_TAG_NONE;
            s.len.store(1u);
            s.payload[0] = 0x5Au;
            r.head.v.store(1u);

            uint8_t buf[SLOT_BYTES];
            uint32_t got_len = 0;
            uint32_t got_port = PORT_MAX;
            uint32_t got_slot = 0;
            ReplyTag got_tag = {};
            Verdict const v = take_call(from, buf, &got_len, &got_port, &got_tag, &got_slot);
            if (v == Verdict::TOOK)
            {
                release_call(from, got_slot);
            }
            return v;
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
            s.port.store(port);
            s.tag = tag;
            s.len.store(CALL_LEN);
            for (uint32_t i = 0; i < CALL_LEN; i++)
            {
                s.payload[i] = static_cast<uint8_t>(0xB0u + i);
            }
            r.head.v.store(1u);
            return true;
        }

        // The VERDICT is returned raw and mapped to its ABI code by the one caller, so an arm
        // reading this reads it through verdict_code as every other forge does.
        Verdict forge_reserve_take(uint32_t from, uint32_t* out_bits)
        {
            *out_bits = 0u;
            uint32_t const me = self();
            if (from >= NODE_MAX or from == me)
            {
                return Verdict::EMPTY;
            }
            // DECLINED RATHER THAN FORGED against a node that runs a kernel of its own: this
            // holds the reply ring toward `from` FULL, which that node would drain out from
            // under the take, and the slots it would then route are this forge's and not a
            // caller's. A wrong answer is worse than no answer, so it says which it is.
            if (g_counts[from].serviced.load() != 0u)
            {
                return Verdict::EMPTY;
            }
            *out_bits = FORGE_RESERVE_RAN;

            // One well-formed call, unread. Nothing about it is malformed, so only the reserve
            // clause can refuse it.
            Ring& call = ring_for(Class::CALL, me, from);
            forge_reset(Class::CALL, call, me, from);
            Slot& s = call.slot[0];
            s.port.store(PORT_ECHO);
            s.tag = REPLY_TAG_NONE;
            s.len.store(1u);
            s.payload[0] = 0x5Au;
            call.head.v.store(1u);

            // The reply ring toward `from` with no slot free. THIS NODE'S OWN head is what
            // moves: the tail is `from`'s and is read but never written, so no far-owned index
            // is forged even here.
            Ring& reply = ring_for(Class::REPLY, from, me);
            uint32_t const tail_was = reply.tail.v.load();
            reply.head.v.store(tail_was + RING_SLOTS);

            uint8_t buf[SLOT_BYTES];
            uint32_t got_len = 0;
            uint32_t got_port = PORT_MAX;
            uint32_t got_slot = 0;
            ReplyTag got_tag = {};
            uint32_t const cursor_was = g_inbox[me][from].taken;
            Verdict const refused =
                take_call(from, buf, &got_len, &got_port, &got_tag, &got_slot);
            // THE PROPERTY THAT SEPARATES RESERVE FROM EVERY OTHER REFUSAL ON THIS RING: a
            // malformed slot is dropped and its cursor advances, this one is left to be taken.
            bool const cursor_held = g_inbox[me][from].taken == cursor_was;

            // ROOM, and the SAME call taken: a refusal that lost the call would satisfy the
            // refusal claim on its own.
            reply.head.v.store(tail_was);
            Verdict const then =
                take_call(from, buf, &got_len, &got_port, &got_tag, &got_slot);
            if (then == Verdict::TOOK)
            {
                release_call(from, got_slot);
            }

            if (cursor_held)
            {
                *out_bits = *out_bits | FORGE_RESERVE_CURSOR_HELD;
            }
            if (then == Verdict::TOOK)
            {
                *out_bits = *out_bits | FORGE_RESERVE_THEN_TOOK;
            }
            return refused;
        }

        uint32_t forge_reply_room(uint32_t to)
        {
            uint32_t const me = self();
            if (to >= NODE_MAX or to == me)
            {
                return 0u;
            }
            Ring& r = ring_for(Class::REPLY, to, me);
            // THE TAIL IS `to`'S AND THIS MOVES IT, which is licensed by that node running no
            // kernel of its own: nothing there will ever drain this ring, so an arm answering
            // one more call into it would be refused RESERVE for the life of the image. A node
            // that HAS serviced drains its own, and this only reports what it left.
            if (g_counts[to].serviced.load() == 0u)
            {
                r.tail.v.store(r.head.v.load());
            }
            uint32_t const used = outstanding(r.head.v.load(), r.tail.v.load());
            if (used >= RING_SLOTS)
            {
                return 0u;
            }
            return RING_SLOTS - used;
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
            s.port.store(PORT_ECHO);
            s.tag = ReplyTag{};
            s.len.store(1u);
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
            s.port.store(PORT_REPLY);
            s.tag = REPLY_TAG_NONE;
            s.len.store(1u);
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
                s.port.store(port);
                s.tag = ReplyTag{};
                s.len.store(BODY);
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

        // THE SELF-RING, and no peer's. What is under test is this node's own producer
        // arithmetic against a tail it did not write, so the ring only has to hold a garbage
        // tail: ring [me][me] is the one no node produces into (send refuses `to == me`) and no
        // service drains (node_service skips `from == me`), so nothing observes what is forged
        // here. A peer's ring would put a forged consumer index under a live consumer, which
        // could resynchronise it or make it discard real traffic.
        //
        // `to` still names the peer, so a refusal that stopped working would publish into the
        // dead ring and ring that peer's doorbell, which is the verdict this arm reads.
        Sent forge_tail_and_send(uint32_t to, uint32_t tail_jump)
        {
            uint32_t const me = self();
            if (to >= NODE_MAX or to == me)
            {
                return Sent::NODE;
            }
            Ring& r = ring_for(Class::CALL, me, me);
            r.head.v.store(0u);
            r.tail.v.store(0u - tail_jump);

            uint8_t pattern[8] = {};
            ReplyTag const tag = {};
            Sent const rc = send_on(r, me, to, PORT_ECHO, tag, pattern, sizeof(pattern));

            forge_reset(Class::CALL, r, me, me);
            return rc;
        }

        // THE SELF REPLY RING and no peer's, for the reason forge_tail_and_send gives: what is
        // forged here is a CONSUMER's index, and a peer's ring would put one under a live
        // consumer. `to` only names whose doorbell a publication that stopped being refused
        // would ring; the strike cell send_on spends is keyed off that node rather than off the
        // ring, so it is cleared on both sides of this and never left carrying a forge's count.
        uint32_t forge_tail_recovery(uint32_t to)
        {
            uint32_t const me = self();
            if (to >= NODE_MAX or to == me)
            {
                return 0u;
            }
            Ring& r = ring_for(Class::REPLY, me, me);
            uint32_t& strikes = tail_strikes_of(to, me);
            strikes = 0u;
            r.head.v.store(0u);
            // An outstanding count no ring can hold, which is a tail no well-formed consumer
            // stores: it only ever advances, and this node publishes nothing past the ring's
            // depth beyond the tail it last read.
            uint32_t const forged = 0u - (RING_SLOTS + 2u);
            r.tail.v.store(forged);

            uint8_t pattern[8] = {};
            ReplyTag const tag = REPLY_TAG_NONE;
            uint32_t answer = 0u;
            if (send_on(r, me, to, PORT_REPLY, tag, pattern, sizeof(pattern)) == Sent::DEPTH)
            {
                answer = answer | 1u;
            }
            // Strikes two through the bound's own, so the publication below is the one that
            // reaches it.
            for (uint32_t i = 2u; i < DEPTH_STRIKES; i++)
            {
                (void)send_on(r, me, to, PORT_REPLY, tag, pattern, sizeof(pattern));
            }
            uint32_t const reset_was = g_counts[me].tail_reset.load();
            if (send_on(r, me, to, PORT_REPLY, tag, pattern, sizeof(pattern)) == Sent::OK)
            {
                answer = answer | 2u;
            }
            // Published AT the far tail this node adopted, and one past it now: a recovery that
            // kept its own head would have written a slot the consumer reads as past.
            if (r.head.v.load() == forged + 1u)
            {
                answer = answer | 4u;
            }
            if (g_counts[me].tail_reset.load() == reset_was + 1u)
            {
                answer = answer | 8u;
            }

            strikes = 0u;
            forge_reset(Class::REPLY, r, me, me);
            return answer | 16u;
        }

        namespace
        {
            bool forge_pending_any(uint32_t me, uint32_t from)
            {
                for (uint32_t i = 0; i < RING_SLOTS; i++)
                {
                    if (g_inbound[me][from][i].pending != 0u)
                    {
                        return true;
                    }
                }
                return false;
            }

            // The publication a discharge or a resynchronisation owes: one slot on from where
            // the head stood, zero length, on the reply port, carrying `tag` verbatim.
            bool forge_answer_at(Ring const& reply, uint32_t head_was, ReplyTag const& tag)
            {
                if (reply.head.v.load() != head_was + 1u)
                {
                    return false;
                }
                Slot const& s = reply.slot[head_was & RING_MASK];
                if (s.len.load() != 0u or s.port.load() != PORT_REPLY)
                {
                    return false;
                }
                return s.tag.thread == tag.thread and s.tag.seq == tag.seq;
            }

            constexpr ReplyTag FORGE_ANSWER_TAG = {0xC1C2C3C4u, 0x0000C5C6u};
        }

        uint32_t forge_answer_defer(uint32_t from)
        {
            uint32_t const me = self();
            if (from >= NODE_MAX or from == me)
            {
                return 0u;
            }
            // DECLINED against a node that runs a kernel of its own: this holds the reply ring
            // toward `from` full and then gives it back, and that node drains it out from under
            // both stores.
            if (g_counts[from].serviced.load() != 0u)
            {
                return ANSWER_DEFER_DECLINED;
            }
            // A take reserves the slot this answer is then refused, so the ring must have room
            // BEFORE the take and none after it. On a node booted alone nothing else ever moves
            // this tail, so a preceding arm's answers would refuse the take outright.
            if (forge_reply_room(from) == 0u)
            {
                return ANSWER_DEFER_DECLINED;
            }
            Ring& r = ring_for(Class::CALL, me, from);
            forge_reset(Class::CALL, r, me, from);
            Ring& reply = ring_for(Class::REPLY, from, me);

            uint8_t buf[SLOT_BYTES];
            uint32_t got_len = 0;
            uint32_t got_port = PORT_MAX;
            uint32_t got_slot = 0;
            ReplyTag got_tag = {};

            publish_one(r, 0u, PORT_ECHO, 0xD0u);
            if (take_call(from, buf, &got_len, &got_port, &got_tag, &got_slot) != Verdict::TOOK)
            {
                return 0u;
            }
            uint32_t const token = inbound_seat(from, got_slot, FORGE_ANSWER_TAG);
            if (token == FAR_RECORD_NONE)
            {
                release_call(from, got_slot);
                return 0u;
            }

            // THE RESERVATION WITHDRAWN BEHIND THE TAKE, by THIS NODE'S OWN head alone: the
            // tail is `from`'s and is read but never written here, so the ring reads full with
            // no far-owned index forged.
            uint32_t const reply_head_was = reply.head.v.load();
            reply.head.v.store(reply.tail.v.load() + RING_SLOTS);

            uint32_t const tail_was = r.tail.v.load();
            uint32_t const unsent_was = g_counts[me].reply_unsent.load();
            uint8_t const body[4] = {0xD8u, 0xD9u, 0xDAu, 0xDBu};
            inbound_reply(token, body, sizeof(body));

            uint32_t answer = 0u;
            if (g_counts[me].reply_unsent.load() == unsent_was + 1u)
            {
                answer = answer | 1u;
            }
            // WHAT SEPARATES A DEFERRAL FROM THE LOSS IT REPLACES: the slot is still this
            // node's, so the caller it names is still owed an answer and no later call can
            // land on it.
            if (r.tail.v.load() == tail_was)
            {
                answer = answer | 2u;
            }
            if (inbound_at(token) == nullptr)
            {
                answer = answer | 4u;
            }

            // The ring given back with room, so one service pass is all the other half needs.
            reply.head.v.store(reply_head_was);
            return answer | 8u;
        }

        uint32_t forge_answer_discharge(uint32_t from)
        {
            uint32_t const me = self();
            if (from >= NODE_MAX or from == me)
            {
                return 0u;
            }
            Ring& call = ring_for(Class::CALL, me, from);
            Ring const& reply = ring_for(Class::REPLY, from, me);
            uint32_t const call_tail_was = call.tail.v.load();
            uint32_t const reply_head_was = reply.head.v.load();

            // THE DOORBELL'S OWN BODY, which is the only thing that discharges one in
            // production. It stands in for the raise a peer's drain or its next publication
            // carries, and for nothing this node owes itself: the discharge runs on every
            // service pass, so any doorbell reaches it.
            node_service();

            uint32_t answer = 0u;
            if (forge_answer_at(reply, reply_head_was, FORGE_ANSWER_TAG))
            {
                answer = answer | 1u;
            }
            if (call.tail.v.load() != call_tail_was)
            {
                answer = answer | 2u;
            }
            if (not forge_pending_any(me, from))
            {
                answer = answer | 4u;
            }
            return answer | 8u;
        }

        uint32_t forge_reset_answers(uint32_t from)
        {
            uint32_t const me = self();
            if (from >= NODE_MAX or from == me)
            {
                return 0u;
            }
            // DECLINED against a node that runs a kernel of its own, for the reason
            // forge_answer_defer gives and one more: that node produces into the call ring this
            // pushes an incredible head onto.
            if (g_counts[from].serviced.load() != 0u)
            {
                return RESET_ANSWERS_DECLINED;
            }
            if (forge_reply_room(from) == 0u)
            {
                return RESET_ANSWERS_DECLINED;
            }
            Ring& r = ring_for(Class::CALL, me, from);
            forge_reset(Class::CALL, r, me, from);
            Ring const& reply = ring_for(Class::REPLY, from, me);

            uint8_t buf[SLOT_BYTES];
            uint32_t got_len = 0;
            uint32_t got_port = PORT_MAX;
            uint32_t got_slot = 0;
            ReplyTag got_tag = {};

            publish_one(r, 0u, PORT_ECHO, 0xC0u);
            if (take_call(from, buf, &got_len, &got_port, &got_tag, &got_slot) != Verdict::TOOK)
            {
                return 0u;
            }
            uint32_t const token = inbound_seat(from, got_slot, FORGE_ANSWER_TAG);
            if (token == FAR_RECORD_NONE)
            {
                release_call(from, got_slot);
                return 0u;
            }

            uint32_t const reply_head_was = reply.head.v.load();
            uint32_t const reset_was = g_counts[me].depth_reset.load();
            uint32_t const unsent_was = g_counts[me].reply_unsent.load();

            // A depth the ring cannot hold, left standing for the whole strike bound. THE HEAD
            // JUMP IS A WHOLE MULTIPLE OF RING_SLOTS for the reason forge_reset_record states.
            r.head.v.store(2u * RING_SLOTS);
            for (uint32_t i = 0; i < DEPTH_STRIKES; i++)
            {
                (void)take_call(from, buf, &got_len, &got_port, &got_tag, &got_slot);
            }

            uint32_t answer = 0u;
            if (g_counts[me].depth_reset.load() == reset_was + 1u)
            {
                answer = answer | 1u;
            }
            if (inbound_at(token) == nullptr)
            {
                answer = answer | 2u;
            }
            if (forge_answer_at(reply, reply_head_was, FORGE_ANSWER_TAG))
            {
                answer = answer | 4u;
            }
            if (g_counts[me].reply_unsent.load() == unsent_was + 1u)
            {
                answer = answer | 8u;
            }

            forge_reset(Class::CALL, r, me, from);
            return answer | 16u;
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
            if (send(from, PORT_REPLY, tag, payload, len) == Sent::OK)
            {
                record_drop(*r);
                release_call(from, slot);
                return;
            }
            // NOT REACHABLE IN A LIVE PARTITION: the take reserved a slot in this very ring
            // for this answer and every reply since has spent one reservation for one slot, so
            // the room cannot have gone. What reaches this is a peer whose reply tail regressed
            // under the reservation. The answer's BYTES are lost with no path holding the
            // capability to remake them, and this is what tells that loss apart from a caller's
            // own refused call in send_refused.
            count_up(g_counts[self()].reply_unsent);
            // THE OBLIGATION OUTLIVES THE ANSWER. The token dies here as on the sent path, so
            // no holder answers twice; the record stays live and pending, which holds its call
            // slot back and keeps a later call off the same masked slot, and node_service
            // publishes the wire's refusal shape for it once the producer's own strike bound
            // has made that ring believable.
            record_defer(*r);
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
