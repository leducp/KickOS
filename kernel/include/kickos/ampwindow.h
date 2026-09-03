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

#include <kickos/sys/abi.h>
#include <kickos/sys/atomic.h>

// A node IS a core, so the window is indexed by core identity.
#if KICKOS_AMP_NODE

namespace kickos
{
    namespace amp
    {
        constexpr uint32_t NODE_MAX = KICKOS_NUM_CORES;
        static_assert(NODE_MAX <= 32,
                      "a node mask is 32 bits wide, as the doorbell's core mask is");

        constexpr uint32_t RING_SLOTS = 4u;
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

        // Ports a node mints. Static in kernel init: no capability authorises a crossing.
        enum : uint32_t
        {
            PORT_ECHO = 0u,  // the payload comes back to the sender's PORT_REPLY
            PORT_REPLY = 1u, // consumed and counted; a reply is never echoed
            PORT_MAX = 32u   // the mint is a 32-bit mask, so this is its width
        };

        // What one receive found. A refusal names WHICH untrusted field was malformed.
        enum class Verdict : uint8_t
        {
            EMPTY = 0, // the producer has published nothing this node has not taken
            TOOK,      // one message copied out
            DEPTH,     // the far head names more outstanding slots than the ring holds
            LENGTH,    // the far length exceeds one slot
            PORT       // the far port names nothing this node minted
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

        struct Slot
        {
            uint32_t len;  // far: payload bytes, bounded against SLOT_BYTES before any copy
            uint32_t port; // far: the receiving node's port, bounded against its own mint
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
            Ring inbox[NODE_MAX][NODE_MAX]; // inbox[to][from]
        };

        // The sender's identity is the RING it wrote; no field in the window claims a sender.
        Ring& ring_for(uint32_t to, uint32_t from);

        // Mint `port` on every node. Kernel init only: one image configures them all, so
        // every node's mint is identical.
        void port_mint(uint32_t port);

        // True where `node` has minted `port`. Total: an out-of-range node or port answers
        // false rather than reading past the record.
        bool port_minted(uint32_t node, uint32_t port);

        // Publish `len` bytes into `to`'s inbox from THIS node and ring its doorbell.
        //
        // THE LOCAL NODE IS REFUSED: node_service skips its own self-ring, so a self-send
        // fills four slots nothing will ever take.
        Sent send(uint32_t to, uint32_t port, void const* payload, uint32_t len);

        // Take one message out of the ring `from` writes to THIS node; the local node reads
        // EMPTY, its self-ring being the one no service drains. `out` takes at most
        // SLOT_BYTES, `*out_len` the bytes taken and `*out_port` the port; none of the three
        // is touched on any verdict but TOOK.
        //
        // THE PORT IS HANDED BACK RATHER THAN LEFT TO BE RE-READ. Its slot is free the instant
        // the tail advances, so a caller reading the field again reads whatever the producer
        // has since put there.
        //
        // A MALFORMED SLOT IS DROPPED AND THE TAIL ADVANCES. Leaving it would let one bad
        // publication wedge the ring for good; the verdict is counted, so the drop is visible.
        Verdict take(uint32_t from, void* out, uint32_t* out_len, uint32_t* out_port);

        // Per-node bookkeeping. OUTSIDE the window: no far side may write it.
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
            Atomic<uint32_t, Order::RELAXED> sent;
            Atomic<uint32_t, Order::RELAXED> send_refused;
            Atomic<uint32_t, Order::RELAXED> serviced; // times its doorbell drained its inboxes
        };

        Counts const& counts(uint32_t node);

        // Drain every inbox of THIS node, echoing a PORT_ECHO message back to its sender's
        // PORT_REPLY and consuming a PORT_REPLY.
        //
        // Reached from the backend's doorbell service, so it runs with this core's interrupts
        // masked, which is the whole of a one-core kernel's exclusion. IT TOUCHES NO kernel()
        // STATE: a peer node's Kernel is provisioned and never initialised.
        void node_service(void);

        // Seat the static mint. Kernel init, before any node can be poked.
        void window_init(void);

#if defined(KICKOS_ENABLE_SELFTEST)
        // Scaffolding: write ONE publication into THIS node's inbox from `from` exactly as a
        // far side would, then take it. `head_jump` is how far past the ring the far head is
        // pushed.
        //
        // BOTH INDICES ARE RESET FIRST, so a real message in flight from that node is lost.
        Verdict forge_and_take(uint32_t from, uint32_t port, uint32_t len, uint32_t head_jump);

        // The send side's half: push the far TAIL of the ring this node produces to `to`, and
        // report what the send made of it. The forged tail is put back afterwards.
        Sent forge_tail_and_send(uint32_t to, uint32_t tail_jump);

        // Publish a depth this node cannot believe, take DEPTH_STRIKES times so the strike
        // bound resynchronises the ring, then publish one WELL-FORMED message and take it.
        // The verdict of that last take is whether the ring recovered.
        Verdict forge_depth_recovery(uint32_t from);
#endif
    }
}

#endif

#endif
