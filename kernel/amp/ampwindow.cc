// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/ampwindow.h>

#if KICKOS_AMP_NODE

#include <kickos/arch/amp_shared.h>

#include <kickos/endpoint.h>
#include <kickos/irqlock.h>
#include <kickos/kruntime.h>
#include <kickos/sys/errno.h>

// The doorbell root brackets with IrqLock, which above one kernel core would draw a second
// ticket from inside arch_kernel_lock's poll.
static_assert(KICKOS_KERNEL_CORES == 1, "the AMP doorbell root assumes one kernel core");

namespace kickos
{
    namespace amp
    {
        namespace
        {
            // THE ONE OBJECT BOTH SIDES WRITE.
            KICKOS_AMP_SHARED("window") constinit Window g_window{};

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

            // This node's own view of each ring it consumes, and not in the window: a far side
            // able to write it would decide when its own slots are reclaimed.
            //
            // tail <= taken <= head. `taken` is the next slot to take; `released` holds one bit
            // per slot from the tail, set when a call's reply was sent or a reply was landed.
            struct Inbox
            {
                uint32_t taken;
                uint32_t released;
            };
            static_assert(RING_SLOTS <= 32u, "one released bit per slot must fit the mask");
            // Per [class][receiver][sender]: under the shared image every core runs this file,
            // so a row keyed on the sender alone would be two nodes' view of two different rings.
            Inbox g_inbox[static_cast<unsigned>(Class::CLASS_MAX)][NODE_MAX][NODE_MAX];

            Inbox& inbox_of(Class cls, uint32_t me, uint32_t from)
            {
                return g_inbox[static_cast<unsigned>(cls)][me][from];
            }

            // Resynchronisations of each reply ring this node consumes: a caller's hold names
            // its slot by a MASKED index, which after one names a later wrap's reply.
            uint16_t g_reply_gen[NODE_MAX][NODE_MAX];

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

            bool is_peer(uint32_t node)
            {
                return node < NODE_MAX and node != self();
            }

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
                r.pending = Inbound::PENDING_NONE;
                r.gen = static_cast<uint16_t>(r.gen + 1u);
            }

            // A record whose answer the reply ring refused. The TOKEN dies as it does in any
            // other death; the record stays live so the call slot it names is not handed back
            // under an answer still owed on it, and so no later call seats on its masked slot.
            void record_defer(Inbound& r, uint8_t state)
            {
                r.pending = state;
                r.defers = 0u;
                r.gen = static_cast<uint16_t>(r.gen + 1u);
            }

            // One writer per row, so a load and a store rather than an increment
            // (tests/static/check_atomic_rmw.sh).
            void count_up(Atomic<uint32_t, Order::RELAXED>& c)
            {
                c.store(c.load() + 1u);
            }

            // The CALLER decides whether the answer is credible: one of the two indices is always
            // the far side's.
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
            // The ring's indices are not read here: the tag comes out of the node-local record.
            //
            // A RECORD ALREADY PENDING IS PASSED OVER, answer_deferred owning it from there:
            // answering it here would count one lost answer twice and bump its generation a
            // second time, halving the distance a stale token travels to collide.
            //
            // EVERY RECORD LEFT STANDING IS PENDING_GONE: the caller has ALREADY cleared the
            // run, so no run accounts for what it owes.
            void record_answer_pair(uint32_t me, uint32_t from)
            {
                for (uint32_t i = 0; i < RING_SLOTS; i++)
                {
                    Inbound& r = g_inbound[me][from][i];
                    if (r.live == 0u)
                    {
                        continue;
                    }
                    if (r.pending != Inbound::PENDING_NONE)
                    {
                        r.pending = Inbound::PENDING_GONE;
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
                    record_defer(r, Inbound::PENDING_GONE);
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

            // One taken CALL. Its slot is held until this returns, and the caller releases it.
            // TRUE where the slot is now a record and the reply releases it; false where the
            // taker still owns the slot and releases it itself.
            //
            // EVERY PATH THAT DID NOT BECOME A RECORD PUBLISHES AN ANSWER, AT THIS ONE SITE.
            // The far caller parks until its deadline, and under KOS_TIMEOUT_NONE there is no
            // deadline, so an arm that returned false without publishing leaks that thread for
            // the life of the image. Past the receiver's pop the call IS a record, whose answer
            // is owed through inbound_reply wherever it is refused.
            bool dispatch_call(uint32_t me, uint32_t from, uint32_t port, ReplyTag const& tag,
                               uint32_t len, uint32_t slot, Held held)
            {
                void const* echo = nullptr;
                uint32_t answer_len = 0u;
                if (port_endpoint(port) != EP_BOUND_NONE)
                {
                    if (endpoint_far_call_deliver(from, port, tag, len, slot, held))
                    {
                        return true;
                    }
                }
                else if (port == PORT_ECHO)
                {
                    // No thread receives an echo, so nothing but the handler can answer it.
                    echo = ring_for(Class::CALL, me, from).slot[slot & RING_MASK].payload;
                    answer_len = len;
                }
                (void)send(from, PORT_REPLY, tag, echo, answer_len);
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
            if (port >= PORT_MAX)
            {
                return;
            }
            g_port_ep1[me][port] = static_cast<uint16_t>(endpoint + 1u);
        }

        uint16_t port_endpoint(uint32_t port)
        {
            uint32_t const me = self();
            if (port >= PORT_MAX)
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

        namespace
        {
            using CountField = Atomic<uint32_t, Order::RELAXED> Counts::*;
            constexpr CountField COUNT_FIELDS[] = {
                &Counts::took,          &Counts::depth,         &Counts::depth_reset,
                &Counts::tail_reset,    &Counts::length,        &Counts::port,
                &Counts::wrong_class,   &Counts::sent,          &Counts::send_refused,
                &Counts::reply_reserve, &Counts::serviced,      &Counts::reply_drop,
                &Counts::reply_unsent,  &Counts::deliver_fault,
            };
            static_assert(sizeof(COUNT_FIELDS) / sizeof(COUNT_FIELDS[0]) == COUNT_IDS,
                          "one id per count");
            static_assert(sizeof(Counts) == COUNT_IDS * sizeof(Atomic<uint32_t, Order::RELAXED>),
                          "a field of Counts has no id");
            constexpr bool names(uint32_t id, CountField field)
            {
                return COUNT_FIELDS[id] == field;
            }
            static_assert(names(KOS_AMP_COUNT_TOOK, &Counts::took)
                              and names(KOS_AMP_COUNT_DEPTH, &Counts::depth)
                              and names(KOS_AMP_COUNT_DEPTH_RESET, &Counts::depth_reset)
                              and names(KOS_AMP_COUNT_TAIL_RESET, &Counts::tail_reset)
                              and names(KOS_AMP_COUNT_LENGTH, &Counts::length)
                              and names(KOS_AMP_COUNT_PORT, &Counts::port)
                              and names(KOS_AMP_COUNT_WRONG_CLASS, &Counts::wrong_class)
                              and names(KOS_AMP_COUNT_SENT, &Counts::sent)
                              and names(KOS_AMP_COUNT_SEND_REFUSED, &Counts::send_refused)
                              and names(KOS_AMP_COUNT_REPLY_RESERVE, &Counts::reply_reserve)
                              and names(KOS_AMP_COUNT_SERVICED, &Counts::serviced)
                              and names(KOS_AMP_COUNT_REPLY_DROP, &Counts::reply_drop)
                              and names(KOS_AMP_COUNT_REPLY_UNSENT, &Counts::reply_unsent)
                              and names(KOS_AMP_COUNT_DELIVER_FAULT, &Counts::deliver_fault)
                              and KOS_AMP_COUNT_DELIVER_FAULT + 1u == COUNT_IDS,
                          "each row sits at the id the ABI names it by");
        }

        int count_read(uint32_t node, uint32_t which, uint32_t* out)
        {
            if (node >= NODE_MAX or which >= COUNT_IDS)
            {
                return -KOS_EINVAL;
            }
            *out = (g_counts[node].*COUNT_FIELDS[which]).load();
            return 0;
        }

        void count_deliver_fault(void)
        {
            count_up(g_counts[self()].deliver_fault);
        }

        namespace
        {
            // The ring is a parameter so the selftest forge can run this arithmetic over a ring
            // no node produces into and no service drains, rather than over a live peer's.
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
            if (not is_peer(to))
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
            // A resynchronisation loses every held slot of that ring and owns their death: each
            // held call record is answered or left PENDING_GONE (record_answer_pair); every
            // reply hold dies with the ring's generation. Left standing, either holder's release
            // would land one wrap later on a DIFFERENT slot still owed to someone else. The tail is
            // returned before the generations move, which is safe only because the caller holds
            // this core's interrupts masked: no snapshot check runs between the two.
            bool depth_ok(Class cls, uint32_t me, uint32_t from, Ring& r, uint32_t head,
                          uint32_t tail)
            {
                // BOTH CURSORS. A head that regressed BEHIND `taken` wraps to a huge count there
                // while head - tail is still small, and the slots it would hand out were never
                // written: a zeroed one reads as a well-formed zero-length echo.
                uint32_t deepest = outstanding(head, tail);
                uint32_t const unread = outstanding(head, inbox_of(cls, me, from).taken);
                if (unread > deepest)
                {
                    deepest = unread;
                }
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
                    inbox_of(cls, me, from).taken = head;
                    inbox_of(cls, me, from).released = 0;
                    if (cls == Class::CALL)
                    {
                        record_answer_pair(me, from);
                    }
                    else
                    {
                        g_reply_gen[me][from] = static_cast<uint16_t>(g_reply_gen[me][from] + 1u);
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
            // AND THE HELD RUN IS NOT THE WHOLE OF WHAT IS OWED. A PENDING_GONE record is counted
            // beside the run, no length of run accounting for it; a pending record still INSIDE
            // the run is left to the arithmetic below, its slot not released while it stands.
            bool reply_reserved(uint32_t me, uint32_t from, uint32_t held, uint32_t released)
            {
                uint32_t owed = 0;
                uint32_t replied = 0;
                for (uint32_t i = 0; i < RING_SLOTS; i++)
                {
                    Inbound const& r = g_inbound[me][from][i];
                    if (r.pending == Inbound::PENDING_GONE)
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

        namespace
        {
            // Release one held slot of a ring this node consumes, answering whether the tail
            // moved. The held run is tail..taken, so a masked index names exactly one of its
            // slots and an index outside it maps past the run rather than into it.
            bool release_on(Class cls, uint32_t me, uint32_t from, uint32_t slot)
            {
                Ring& r = ring_for(cls, me, from);
                Inbox& ib = inbox_of(cls, me, from);
                uint32_t tail = r.tail.v.load();
                uint32_t const held = outstanding(ib.taken, tail);
                uint32_t const off = (slot - tail) & RING_MASK;
                if (off >= held)
                {
                    return false;
                }
                ib.released = ib.released | (1u << off);
                uint32_t const was = tail;
                while ((ib.released & 1u) != 0u and tail != ib.taken)
                {
                    ib.released = ib.released >> 1;
                    tail = tail + 1u;
                }
                r.tail.v.store(tail);
                return tail != was;
            }

            // A reply slot's release, and the CREDIT RETURN its advance owes (take_reply).
            void reply_release_on(uint32_t me, uint32_t from, uint32_t slot)
            {
                if (release_on(Class::REPLY, me, from, slot))
                {
                    ring(from);
                }
            }

            // A reply hold's token: the ring's generation over an index past every record's, so
            // hold_payload and hold_release tell the two kinds apart by the index alone.
            constexpr uint32_t HOLD_REPLY_BASE = RECORD_MAX;
            static_assert(HOLD_REPLY_BASE + RECORD_MAX < RECORD_MASK,
                          "a reply hold's index must stay below the all-ones half as a record's "
                          "does");

            uint32_t reply_hold_of(uint32_t me, uint32_t from, uint32_t slot)
            {
                return record_token(HOLD_REPLY_BASE + record_index(me, from, slot),
                                    g_reply_gen[me][from]);
            }

            // True where a reply reached a parked caller, who then holds its slot.
            bool dispatch_reply(uint32_t me, uint32_t from, ReplyTag const& tag, uint32_t len,
                                uint32_t slot, Held held)
            {
                if (endpoint_far_reply_deliver(from, tag, reply_hold_of(me, from, slot), len, held))
                {
                    return true;
                }
                count_up(g_counts[me].reply_drop);
                return false;
            }
        }

        Verdict take_reply(uint32_t from, uint32_t* out_len, uint32_t* out_port,
                           ReplyTag* out_tag, uint32_t* out_slot)
        {
            uint32_t const me = self();
            if (not is_peer(from))
            {
                return Verdict::EMPTY;
            }

            Ring& r = ring_for(Class::REPLY, me, from);
            Inbox& ib = inbox_of(Class::REPLY, me, from);
            uint32_t const tail = r.tail.v.load();
            // FAR: the producer owns this index, and it is NEVER used as one.
            uint32_t const head = r.head.v.load();
            uint32_t const unread = outstanding(head, ib.taken);
            if (outstanding(head, tail) == 0u and unread == 0u)
            {
                return Verdict::EMPTY;
            }

            // ONE EXIT PAST THIS POINT, so the credit raise below cannot be reached around.
            Verdict v = Verdict::DEPTH;
            if (depth_ok(Class::REPLY, me, from, r, head, tail))
            {
                v = Verdict::EMPTY;
                if (unread != 0u)
                {
                    uint32_t const at = ib.taken;
                    // The slot index comes from this node's OWN cursor.
                    Slot const& s = r.slot[at & RING_MASK];
                    // ONE LOAD EACH: every clause below spends the snapshot.
                    uint32_t const len = s.len.load();
                    uint32_t const port = s.port.load();
                    v = slot_ok(Class::REPLY, me, len, port);
                    ib.taken = at + 1u;
                    if (v == Verdict::TOOK)
                    {
                        *out_len = len;
                        *out_port = port;
                        *out_tag = s.tag;
                        *out_slot = at & RING_MASK;
                        count_up(g_counts[me].took);
                    }
                    else
                    {
                        (void)release_on(Class::REPLY, me, from, at & RING_MASK);
                    }
                }
            }

            // CREDIT RETURN, whose omission costs liveness and not latency (ampwindow.h). It
            // rings wherever the tail advances: a drop or a resynchronisation here, a release in
            // reply_release_on.
            if (r.tail.v.load() != tail)
            {
                ring(from);
            }
            return v;
        }

        void release_call(uint32_t from, uint32_t slot)
        {
            uint32_t const me = self();
            if (not is_peer(from))
            {
                return;
            }
            (void)release_on(Class::CALL, me, from, slot);
        }

        void release_reply(uint32_t from, uint32_t slot)
        {
            uint32_t const me = self();
            if (not is_peer(from))
            {
                return;
            }
            reply_release_on(me, from, slot);
        }

        Verdict take_call(uint32_t from, uint32_t* out_len, uint32_t* out_port,
                          ReplyTag* out_tag, uint32_t* out_slot)
        {
            uint32_t const me = self();
            if (not is_peer(from))
            {
                return Verdict::EMPTY;
            }

            Ring& r = ring_for(Class::CALL, me, from);
            Inbox& ib = inbox_of(Class::CALL, me, from);
            uint32_t const tail = r.tail.v.load();
            uint32_t const head = r.head.v.load();
            if (not depth_ok(Class::CALL, me, from, r, head, tail))
            {
                return Verdict::DEPTH;
            }

            // Held plus unread is the far head over this node's tail, which the depth clause
            // above bounds at RING_SLOTS: a ring with every slot held has nothing unread.
            if (outstanding(head, ib.taken) == 0u)
            {
                return Verdict::EMPTY;
            }
            uint32_t const at = ib.taken;
            Slot const& s = r.slot[at & RING_MASK];
            uint32_t const len = s.len.load();
            uint32_t const port = s.port.load();
            Verdict const v = slot_ok(Class::CALL, me, len, port);
            if (v != Verdict::TOOK)
            {
                // UNCONDITIONALLY, and ahead of the reserve below: a malformed slot owes no
                // reply, so it may hold no record.
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
                    if (r.pending == Inbound::PENDING_NONE)
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
                    bool const gone = r.pending == Inbound::PENDING_GONE;
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

        void node_service(Held held)
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
                    uint32_t len = 0;
                    uint32_t port = PORT_MAX;
                    uint32_t slot = 0;
                    ReplyTag tag = {};
                    Verdict const v = take_reply(from, &len, &port, &tag, &slot);
                    if (v == Verdict::EMPTY or v == Verdict::DEPTH)
                    {
                        break;
                    }
                    done++;
                    if (v != Verdict::TOOK)
                    {
                        continue;
                    }
                    // A reply no caller lands is released here, or the tail never moves past it.
                    if (not dispatch_reply(me, from, tag, len, slot, held))
                    {
                        reply_release_on(me, from, slot);
                    }
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
                    uint32_t len = 0;
                    uint32_t port = PORT_MAX;
                    uint32_t slot = 0;
                    ReplyTag tag = {};
                    Verdict const v = take_call(from, &len, &port, &tag, &slot);
                    if (v == Verdict::EMPTY or v == Verdict::DEPTH or v == Verdict::RESERVE)
                    {
                        break;
                    }
                    done++;
                    if (v != Verdict::TOOK)
                    {
                        continue;
                    }
                    if (not dispatch_call(me, from, port, tag, len, slot, held))
                    {
                        release_call(from, slot);
                    }
                }
            }
        }

        uint32_t inbound_seat(uint32_t from, uint32_t slot, ReplyTag const& tag)
        {
            uint32_t const me = self();
            if (from >= NODE_MAX)
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
            record_defer(*r, Inbound::PENDING_HELD);
        }

        void const* hold_payload(uint32_t hold)
        {
            uint32_t index = token_index(hold);
            Class cls = Class::CALL;
            if (index >= HOLD_REPLY_BASE)
            {
                index = index - HOLD_REPLY_BASE;
                cls = Class::REPLY;
            }
            if (index >= RECORD_MAX)
            {
                return nullptr;
            }
            uint32_t const me = index / (NODE_MAX * RING_SLOTS);
            uint32_t const from = (index / RING_SLOTS) % NODE_MAX;
            return ring_for(cls, me, from).slot[index % RING_SLOTS].payload;
        }

        bool hold_live(uint32_t hold)
        {
            uint32_t const index = token_index(hold);
            if (index < HOLD_REPLY_BASE)
            {
                return record_of(hold) != nullptr;
            }
            uint32_t const at = index - HOLD_REPLY_BASE;
            if (at >= RECORD_MAX)
            {
                return false;
            }
            uint32_t const me = at / (NODE_MAX * RING_SLOTS);
            uint32_t const from = (at / RING_SLOTS) % NODE_MAX;
            if (me != self() or from == me or token_gen(hold) != g_reply_gen[me][from])
            {
                return false;
            }
            Inbox const& ib = inbox_of(Class::REPLY, me, from);
            uint32_t const tail = ring_for(Class::REPLY, me, from).tail.v.load();
            uint32_t const off = ((at % RING_SLOTS) - tail) & RING_MASK;
            return off < outstanding(ib.taken, tail) and (ib.released & (1u << off)) == 0u;
        }

        void hold_release(uint32_t hold)
        {
            uint32_t const index = token_index(hold);
            if (index < HOLD_REPLY_BASE)
            {
                inbound_reply(hold, nullptr, 0u);
                return;
            }
            uint32_t const at = index - HOLD_REPLY_BASE;
            if (at >= RECORD_MAX)
            {
                return;
            }
            uint32_t const me = at / (NODE_MAX * RING_SLOTS);
            uint32_t const from = (at / RING_SLOTS) % NODE_MAX;
            // A hold the ring's generation has moved past names a later wrap's reply.
            if (token_gen(hold) != g_reply_gen[me][from])
            {
                return;
            }
            reply_release_on(me, from, at % RING_SLOTS);
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
            for (unsigned cls = 0; cls < static_cast<unsigned>(Class::CLASS_MAX); cls++)
            {
                for (uint32_t from = 0; from < NODE_MAX; from++)
                {
                    Inbox& ib = inbox_of(static_cast<Class>(cls), me, from);
                    ib.taken = g_window.inbox[cls][me][from].tail.v.load();
                    ib.released = 0u;
                }
            }

            // The peer's half of the bring-up pairing: a sender that found this node unseatable
            // published anyway and skipped only the raise, so what it sent is in a ring with no
            // notice coming. The fence is required and is NOT the one the seating store
            // carries: seating then reading is a store then a load on both sides, and without a
            // full barrier on each the two may both read the older value.
            arch_ipi_fence();
            IrqLock lock;
            node_service(lock);
        }
    }
}

extern "C" void kickos_amp_node_service(void)
{
    ::kickos::IrqLock lock;
    ::kickos::amp::node_service(lock);
}

#endif
