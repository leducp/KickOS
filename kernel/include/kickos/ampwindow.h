// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The AMP shared window: the memory one node writes and another reads, and the validation
// the reading node owes it.
//
// EVERY FIELD BELOW COMMENTED `far` IS ANOTHER NODE'S WRITING AND IS UNTRUSTED INPUT. No
// index read out of the window is ever used as an index, and no length read out of it is used
// as a length before it is bounded. BOTH SIDES VALIDATE: the consumer reads a head the
// producer owns, and the producer reads a tail the consumer owns.
//
// The window needs coherency with no software maintenance, or a port owes maintenance at
// every access here. An A53 cluster in one inner-shareable domain has it.
//
// PROTOCOL SCAFFOLDING, AND NOT AN ISOLATION BOUNDARY BETWEEN KERNELS. Every node maps the
// same writable kernel RAM, so a compromised peer KERNEL can rewrite the mint, the counters
// and every other node's kernel data whatever this file validates. What the validation below
// defends a node against is a MALFORMED peer, never a hostile one. Per-node memory
// partitioning is the AMP partition layout the contract leaves open, and until it lands
// nothing here may be read as a privilege boundary.

#ifndef KICKOS_AMPWINDOW_H
#define KICKOS_AMPWINDOW_H

#include <stdint.h>

#include <kickos/arch/arch.h>
#include <kickos/instance_local.h>

#include <kickos/config/amp_ports.h>
#include <kickos/sys/abi.h>
#include <kickos/sys/atomic.h>

#if KICKOS_AMP_NODE

namespace kickos
{
    namespace amp
    {
        // The PARTITION's width, which is not how many cores this image drives: an own-image
        // node drives one and still names every peer it can reach.
        constexpr uint32_t NODE_MAX = KICKOS_AMP_NODES;
        static_assert(NODE_MAX <= 32,
                      "a node mask is 32 bits wide, as the doorbell's core mask is");
#if KICKOS_AMP_SHARED_IMAGE
        static_assert(NODE_MAX >= KICKOS_NUM_CORES,
                      "one image drives every node under this posture, so a core it drives "
                      "that the partition does not name would index past the window");
#else
        static_assert(KICKOS_AMP_NODE_ID < NODE_MAX,
                      "this image is built as a node its own partition does not hold");
        static_assert(KICKOS_NUM_CORES == 1,
                      "an own-image node drives one core: a count above it names peers this "
                      "image does not launch");
#endif

        // WHICH NODE THIS IMAGE IS, and the only site that answers it.
        inline uint32_t self(void)
        {
#if KICKOS_AMP_SHARED_IMAGE
            return arch_cpu_id();
#else
            return KICKOS_AMP_NODE_ID;
#endif
        }

        // The partition's port list, in the order the kernel seats it into root's table. AN
        // ENTRY'S POSITION IS ITS CAPABILITY INDEX, so reordering renumbers every constant
        // derived from it. Both nodes of a crossing read the same entry and derive opposite
        // roles: node == self() binds a local endpoint, any other node is a far endpoint.
        constexpr uint32_t PORT_COUNT = KICKOS_AMP_PORT_COUNT;
        static_assert(PORT_COUNT > 0u,
                      "a node whose partition names no crossing can neither be called nor "
                      "call; CMakeLists.txt refuses an empty KICKOS_AMP_PORTS");
        constexpr uint8_t PORT_NODE[PORT_COUNT] = {KICKOS_AMP_PORT_NODE_LIST};
        constexpr uint8_t PORT_PORT[PORT_COUNT] = {KICKOS_AMP_PORT_PORT_LIST};

        // The node the seating runs for, as a build constant. Asserted equal to self() where
        // the seating happens: under the shared image self() is a core register, this is not.
        constexpr uint32_t SELF_NODE = KICKOS_AMP_SELF_NODE;

        constexpr uint32_t RING_SLOTS = KOS_AMP_RING_SLOTS;
        static_assert((RING_SLOTS & (RING_SLOTS - 1u)) == 0u,
                      "RING_SLOTS must be a power of two");

        // A slot holds a whole message at the local IPC bound.
        constexpr uint32_t SLOT_BYTES = KOS_EP_MSG_MAX;

        // WHAT ONE SERVICE CALL WILL DO, per sender and across every sender. node_service
        // runs inside the masked doorbell handler, so an unbounded drain is a peer keeping
        // this core in the handler for as long as it keeps publishing. One ring's worth per
        // sender empties a ring that was full on entry; anything past that was published
        // AFTER entry and carries a doorbell raise of its own.
        constexpr uint32_t SERVICE_PER_SENDER = RING_SLOTS;
        constexpr uint32_t SERVICE_PER_CALL = RING_SLOTS * (NODE_MAX - 1u);

        // Consecutive DEPTH verdicts from one sender before its tail is resynchronised to the
        // far head. A refused depth does not advance the tail, so without a bound the ring
        // stays dead for the life of the image; the resynchronisation loses whatever the ring
        // held and is counted, and the far index it takes is still only ever spent modulo
        // RING_SLOTS.
        constexpr uint32_t DEPTH_STRIKES = 4u;

        // A53 line size (DDI 0500J section 2.1). Too low costs sharing: the two indices below
        // have one writer each and different owners.
        constexpr uint32_t LINE_BYTES = 64u;

        // The window layer's own two ports, which every node mints and no node binds, and the
        // width of the mint mask. Every other port is the partition's (PORT_NODE above).
        enum : uint32_t
        {
            PORT_ECHO = KOS_AMP_PORT_ECHO,
            PORT_REPLY = KOS_AMP_PORT_REPLY,
            PORT_MAX = 32u // the mint is a 32-bit mask, so this is its width
        };

        // Which ring of the ordered pair. The two are reclaimed differently and cannot be one:
        // a CALL slot is this node's record of the caller until the reply is sent, a REPLY slot
        // is released as it is taken. Merged, a node's parked callers fill the ring toward them
        // with the peer's un-replied calls and the reply that would free them finds no slot.
        enum class Class : uint8_t
        {
            CALL = 0,
            REPLY,
            CLASS_MAX
        };

        // What one receive found. A refusal names WHICH untrusted field was malformed.
        enum class Verdict : uint8_t
        {
            EMPTY = 0, // the producer has published nothing this node has not taken
            TOOK,      // one message copied out
            DEPTH,     // the far head names more outstanding slots than the ring holds
            LENGTH,    // the far length exceeds one slot
            PORT,      // the far port names nothing this node minted
            CLASS      // a message of the other class: a reply on the call ring, or the reverse
        };

        // Why one send was refused.
        enum class Sent : uint8_t
        {
            OK = 0,
            FULL,   // every slot is outstanding
            DEPTH,  // the far tail names more outstanding slots than the ring holds
            LENGTH, // the caller's own length exceeds one slot
            NODE,   // the target names no node this image drives
            PORT    // the target port is outside the mint width
        };

        // A caller's reply route, carried across and handed straight back. Neither field is
        // ever spent as an index or a length here, which is the whole of why an unvalidated
        // one may cross.
        struct ReplyTag
        {
            uint32_t thread; // far
            uint32_t seq;    // far
        };

        // THE WIRE WIDTH OF A REPLY SEQUENCE, and the ONE conversion to it: the publisher,
        // the validator and the selftest forge all take their sequence from here, so the
        // width cannot be changed at one site and left standing at another.
        //
        // `seq` carries 32 bits and only these are validated, so a caller that RETAINS a tag
        // aliases the same long-lived caller after 256 calls to that node. The validation
        // defends a node against a MALFORMED peer and not a hostile one, and a peer that
        // replays a tag it kept is the hostile case. Widening this widens the wire.
        constexpr uint32_t REPLY_SEQ_MASK = 0xFFu;

        constexpr uint8_t reply_seq(uint32_t seq)
        {
            return static_cast<uint8_t>(seq & REPLY_SEQ_MASK);
        }
        static_assert(REPLY_SEQ_MASK <= 0xFFu,
                      "reply_seq narrows to the uint8_t cap_reply_thread takes, so a wider "
                      "mask would be masked again on the way in");

        // The route of a sender that does not park, and it is NOT the zero one: zero is index
        // 0 at generation 0, which a live thread slot can be, so an echo of it could complete
        // some unrelated caller's call. KOS_THREAD_NONE carries the all-ones index the thread
        // pool reserves and never seats, so no generation resolves it.
        constexpr ReplyTag REPLY_TAG_NONE = {KOS_THREAD_NONE, 0u};

        struct Slot
        {
            uint32_t len;  // far: payload bytes, bounded against SLOT_BYTES before any copy
            uint32_t port; // far: the receiving node's port, bounded against its own mint
            ReplyTag tag;  // far: handed back to the taker, spent by nothing in this file
            uint8_t payload[SLOT_BYTES];
        };

        struct alignas(LINE_BYTES) Index
        {
            Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE> v;
        };

        // ONE ring per ORDERED PAIR of nodes: one inbox shared by every sender would put
        // several producers on one head, and moving that head needs a read-modify-write.
        struct Ring
        {
            Index head; // the producer's; far on the receive side
            Index tail; // the consumer's; far on the send side
            Slot slot[RING_SLOTS];
        };

        struct Window
        {
            Ring inbox[static_cast<unsigned>(Class::CLASS_MAX)][NODE_MAX][NODE_MAX];
        };

        // The sender's identity is the RING it wrote; no field in the window claims a sender.
        Ring& ring_for(Class cls, uint32_t to, uint32_t from);

        // Bind a port of THIS node to one of its own endpoints, so a call arriving on it
        // reaches a thread rather than the window layer. Static per N8: no capability
        // authorises the crossing. A port bound to nothing is not an error and is what
        // PORT_ECHO is, the window layer answering it with no thread involved.
        void port_bind(uint32_t port, uint16_t endpoint);

        // The endpoint `port` is bound to on THIS node, or EP_BOUND_NONE. Binding to
        // EP_BOUND_NONE itself unbinds, the store being biased so that the two coincide.
        constexpr uint16_t EP_BOUND_NONE = 0xFFFFu;
        uint16_t port_endpoint(uint32_t port);

        // True where `node` has minted `port`. Total: an out-of-range node or port answers
        // false rather than reading past the record.
        //
        // Per node: a mint spread over every row makes a far endpoint mintable for a node that
        // serves nothing, which then answers a caller with silence rather than a refusal.
        //
        // A sender validates a far port against its OWN copy of the far node's row
        // (kernel/syscall/syscall_ipc.cc), which is why window_init seats every row and not just
        // its own. Both images derive the mint from the one partition list, so neither has to
        // check the other.
        bool port_minted(uint32_t node, uint32_t port);

        // Ring `to`'s doorbell. kernel/amp/ampmap.cc owns the node-to-core map it needs.
        void ring(uint32_t to);

        // Which machine core carries `node`. The partition states the map; nothing derives it.
        uint32_t core_of(uint32_t node);

        // Publish `len` bytes into `to`'s inbox from THIS node and ring its doorbell.
        //
        // THE LOCAL NODE IS REFUSED: node_service skips its own self-ring, so a self-send
        // fills four slots nothing will ever take.
        Sent send(uint32_t to, uint32_t port, ReplyTag const& tag, void const* payload,
                  uint32_t len);

        // Take one REPLY out of the ring `from` writes to THIS node; the local node reads
        // EMPTY, its self-ring being the one no service drains. `out` takes at most
        // SLOT_BYTES, `*out_len` the bytes taken, `*out_port` the port and `*out_tag` the
        // reply route; none of the four is touched on any verdict but TOOK.
        //
        // THE PORT AND THE TAG ARE HANDED BACK RATHER THAN LEFT TO BE RE-READ. Their slot is
        // free the instant the tail advances, so a caller reading either field again reads
        // whatever the producer has since put there.
        //
        // A MALFORMED SLOT IS DROPPED AND THE TAIL ADVANCES. Leaving it would let one bad
        // publication wedge the ring for good; the verdict is counted, so the drop is visible.
        Verdict take_reply(uint32_t from, void* out, uint32_t* out_len, uint32_t* out_port,
                           ReplyTag* out_tag);

        // Take one CALL and HOLD its slot: on TOOK the slot is this node's record of the caller
        // until release_call frees it, so outstanding inbound calls from one peer are RING_SLOTS
        // by construction. `*out_slot` names the held slot, masked to the ring, and is what
        // release_call takes back. The tag is copied OUT rather than re-read from the held slot,
        // which a malformed producer may have touched.
        Verdict take_call(uint32_t from, void* out, uint32_t* out_len, uint32_t* out_port,
                          ReplyTag* out_tag, uint32_t* out_slot);

        // A held call's record: the origin node and the reply token, stored verbatim and never
        // spent here. One per slot of every ordered pair, so the table is a build constant.
        //
        // `gen` counts the deaths of this slot's record. The record IS the ring slot
        // (docs/design-multicore.md N6f), and the resynchronisation in depth_ok is the one path
        // that frees one whose capability is still live; the table slot is keyed by ring
        // position, so without the count that holder's token would name the slot's NEXT tenant
        // and would answer a stranger's caller and release a stranger's slot.
        struct Inbound
        {
            ReplyTag tag;
            uint16_t gen;
            uint8_t from;
            uint8_t slot;
            uint8_t live;
        };

        // Seat a record for a held call and answer a TOKEN for it, or FAR_RECORD_NONE where the
        // table has none free. Kernel only, from the doorbell service. The token is the record's
        // generation in its upper half and the flat table index in its lower, the encoding
        // kickos::ThreadPool::far_reply_handle carries through a reply capability.
        constexpr uint32_t FAR_RECORD_NONE = 0xFFFFFFFFu;
        uint32_t inbound_seat(uint32_t from, uint32_t slot, ReplyTag const& tag);

        // What record `token` names, or nullptr where it names nothing: an index outside the
        // table, a free record, or a record whose generation has moved past this token's.
        Inbound const* inbound_at(uint32_t token);

        // Free the record `token` names WITHOUT answering and WITHOUT releasing its call slot.
        // For the one path that seats a record and then cannot hand anyone a capability for it:
        // the slot is the caller's answer and stays the taker's to release.
        void inbound_forget(uint32_t token);

        // Send `payload` as the reply the record `token` names, release its call slot, and
        // free the record. Total over a token naming nothing, which includes a record a ring
        // resynchronisation freed under its holder: that slot belongs to a later wrap's call
        // by then and its caller is gone, so nothing is sent and nothing is released.
        void inbound_reply(uint32_t token, void const* payload, uint32_t len);

        // Release a held call slot. The tail advances over released slots from the oldest, so
        // replies may complete OUT OF ORDER while reclamation stays in order. `slot` is the
        // index take_call handed out, masked to the ring; a resynchronisation between the take
        // and the release makes that index name a later wrap's slot, which the record
        // generation above keeps out of here.
        void release_call(uint32_t from, uint32_t slot);

        // Per-node bookkeeping, one row per node, each row written by that node alone.
        //
        // A count is a REPORT and never an input: no field of a peer's row is spent as an index
        // or a length anywhere, which is what lets it sit where a peer can write it.
        //
        // RELAXED ATOMICS BECAUSE THEY ARE READ ACROSS NODES: a syscall on node 0 sweeps every
        // node's row while a peer is inside its own doorbell handler writing its own. One
        // writer per row, so the load/store pair below carries no lost update, and there is no
        // read-modify-write above the seam (tests/static/check_atomic_rmw.sh).
        struct Counts
        {
            Atomic<uint32_t, Order::RELAXED> took;
            Atomic<uint32_t, Order::RELAXED> depth;
            Atomic<uint32_t, Order::RELAXED> depth_reset; // rings resynchronised after DEPTH_STRIKES
            Atomic<uint32_t, Order::RELAXED> length;
            Atomic<uint32_t, Order::RELAXED> port;
            Atomic<uint32_t, Order::RELAXED> wrong_class; // a reply on the call ring, or the reverse
            Atomic<uint32_t, Order::RELAXED> sent;
            Atomic<uint32_t, Order::RELAXED> send_refused;
            Atomic<uint32_t, Order::RELAXED> serviced; // times its doorbell drained its inboxes
            // Replies taken and then refused by the endpoint layer's validation of the tag.
            // A node running no kernel of its own counts EVERY reply here, its thread pool
            // refusing each at the first clause.
            Atomic<uint32_t, Order::RELAXED> reply_drop;
        };

        Counts const& counts(uint32_t node);

        // Drain every inbox of THIS node, echoing a PORT_ECHO message back to its sender's
        // PORT_REPLY and routing a PORT_REPLY to whatever local caller its tag names.
        //
        // Reached from the backend's doorbell service, so it runs with this core's interrupts
        // masked, which is the whole of a one-core kernel's exclusion.
        //
        // THE REPLY ROUTE READS kernel() AND IS INERT ON A NODE THAT RUNS NO KERNEL: a peer
        // node's Kernel is provisioned and never initialised, so its thread pool is zeroed and
        // refuses every index at the first clause. Nothing else here touches kernel() state.
        void node_service(void);

        // Seat the static mint and THIS NODE'S OWN rows, then read the rings once. Kernel init,
        // before any node can be poked.
        //
        // The mint is every row because port_minted is asked about peers; every other table here
        // is this node's alone. A peer is already inside node_service by the time kmain reaches
        // here, having been released in arch_init, so writing a peer's row would race its own
        // drain with nothing ordering the two.
        void window_init(void);

#if defined(KICKOS_ENABLE_SELFTEST)
        // Scaffolding: write ONE publication into THIS node's inbox from `from` exactly as a
        // far side would, then take it. `head_jump` is how far past the ring the far head is
        // pushed.
        //
        // BOTH INDICES ARE RESET FIRST, so a real message in flight from that node is lost.
        Verdict forge_and_take(uint32_t from, uint32_t port, ReplyTag const& tag, uint32_t len,
                               uint32_t head_jump);

        // The send side's half: push the far TAIL of the ring this node produces to `to`, and
        // report what the send made of it. The forged tail is put back afterwards.
        Sent forge_tail_and_send(uint32_t to, uint32_t tail_jump);

        // Publish ONE PORT_REPLY into THIS node's inbox from `from` carrying `tag`, then take
        // it and route it exactly as node_service does, so a HOSTILE reply is playable at a
        // caller that is genuinely parked. True where it reached one.
        //
        // BOTH INDICES ARE RESET FIRST, as forge_and_take does.
        bool forge_reply(uint32_t from, ReplyTag const& tag);

        // Scaffolding: publish ONE well-formed call from `from` on `port` into THIS node's
        // ring and take it no further, so the doorbell's own service body is what drains it.
        // BOTH INDICES ARE RESET FIRST, as the other forges do.
        bool forge_publish(uint32_t from, uint32_t port, ReplyTag const& tag);

        // Drain what forge_publish left, and answer whether the delivery kept the call's slot.
        // The CALL ring tail moves in release_call alone, so an unmoved tail is the whole of
        // "the receiver holds this slot until it replies" and a moved one the whole of "the
        // taker released it".
        bool forge_drain_held(uint32_t from);

        // Publish a depth this node cannot believe, take DEPTH_STRIKES times so the strike
        // bound resynchronises the ring, then publish one WELL-FORMED message and take it.
        // The verdict of that last take is whether the ring recovered.
        Verdict forge_depth_recovery(uint32_t from);

        // The same bound on the REPLY ring, driven through node_service rather than a bare take
        // because that is where the two rings of a pair meet: one pass yields at most one reply
        // strike and then runs the call ring's depth clause, which a strike count not keyed by
        // class would clear. The verdict of a well-formed reply taken afterwards is whether the
        // ring recovered.
        Verdict forge_reply_depth_recovery(uint32_t from);

        // One inbound record across a resynchronisation: a record seated on a HELD slot, the
        // ring resynchronised under it, a fresh call taken at the same masked slot, and the
        // first holder's token spent afterwards, in that order and on one ring.
        //
        // Answers a bit per claim, all four set where the reset owns the record's death:
        //   1  the resynchronisation freed the record it abandoned the slot of
        //   2  the call taken after it was granted a record of its own
        //   4  that record's token differs from the abandoned one's
        //   8  spending the abandoned token released no slot
        // Bit 16 says the scaffold reached the end, so a zero answer is a forge that could not
        // run rather than four failed claims.
        constexpr uint32_t RESET_RECORD_OK = 0x1Fu;
        uint32_t forge_reset_record(uint32_t from);
#endif
    }
}

#endif

#endif
