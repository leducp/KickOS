// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The CALLING node of a two-image AMP partition. Its peer runs a kernel of its own, out of a
// second image in the same programmable artefact, and this app reaches a thread parked in that
// other kernel through the ordinary call.
//
// Nothing here names a node's identity, a ring, a window or a doorbell. The partition states
// its crossings once (CONFIG_KICKOS_AMP_PORTS) and the kernel seats this node's derived set
// into root before main is entered. Locality never reaches the API
// (docs/design-multicore.md N7): this is kos_call_timed.

#include <iso646.h> // and / or / not are macros in C, not keywords
#include <stdio.h>

#include <kickos/amp.h>
#include <kickos/sys.h>
#include <kickos/sys/abi_probe.h>

#define AMPPING_ROUNDS 4
#define AMPPING_CALL_US (500u * 1000u)
#define AMPPING_SETTLE_NS (20ull * 1000ull * 1000ull)
#define AMPPING_TRIES 40

// The peer this image was handed a capability to CALL: the first entry of the partition's list
// naming a node that is not this one.
static kos_cap_t ampping_peer(uint32_t* out_node, uint32_t* out_port)
{
    uint32_t i;
    for (i = 0; i < KOS_AMP_PORT_COUNT; i++)
    {
        uint32_t const node = kos_amp_entry_node(i);
        uint32_t const port = kos_amp_entry_port(i);
        if (node == KOS_AMP_SELF_NODE)
        {
            continue;
        }
        *out_node = node;
        *out_port = port;
        return kos_amp_port(node, port);
    }
    return KOS_CAP_NONE;
}

#if defined(KICKOS_ENABLE_SELFTEST)
// The capability this image holds for a crossing at `node`: the first entry of the partition's
// list naming it. KOS_CAP_NONE where the partition names that node no port at all.
static kos_cap_t ampping_port_of(uint32_t node, uint32_t* out_port)
{
    uint32_t i;
    for (i = 0; i < KOS_AMP_PORT_COUNT; i++)
    {
        if (kos_amp_entry_node(i) != node)
        {
            continue;
        }
        *out_port = kos_amp_entry_port(i);
        return kos_amp_port(node, *out_port);
    }
    return KOS_CAP_NONE;
}

// The first port the partition names `node`, or KOS_AMP_NO_ENTRY where it names none. The
// capability is not wanted here, only the port: this node holds none for a peer's own crossing
// it was handed no entry for.
static uint32_t ampping_first_port(uint32_t node)
{
    uint32_t i;
    for (i = 0; i < KOS_AMP_PORT_COUNT; i++)
    {
        if (kos_amp_entry_node(i) == node)
        {
            return kos_amp_entry_port(i);
        }
    }
    return KOS_AMP_NO_ENTRY;
}
#endif

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    uint32_t peer = 0;
    uint32_t port = 0;
    kos_cap_t const ep = ampping_peer(&peer, &port);
    if (ep == KOS_CAP_NONE)
    {
        // A node the partition hands no crossing learns so here, not from a call nobody answers.
        printf("ampping: node %u was handed no far port\n", (unsigned)KOS_AMP_SELF_NODE);
        return 1;
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // --- Which nodes' APPS ran, reported by this node ---------------------------------------
    // NOTHING A GATE READS MAY BE PRINTED ABOVE THIS BLOCK, and that is what keeps those lines
    // out of the banner storm. Every node queues its whole BOOT output, its kernel banner and
    // its app's one announcement, before it publishes the mark swept here, so a sweep that has
    // read every node's mark has read past every byte a peer writes while booting. It does not
    // make the peers silent afterwards: a serving node prints once for every round it answers.
    // Two kernels share one console and no lock spans them (N6h): a line printed above this
    // block lands among the peers' banners and arrives cut in half.
    //
    // The rounds below witness the FIRST peer's app and nothing else: this app calls that one
    // crossing and no other, so a wider partition holds nodes no crossing ever reaches and not
    // one window counter moves for them. Each node's app declares itself into its own row of
    // the shared record instead, and THIS node reports what it reads, because a line a quieter
    // node prints is not evidence to a gate: nothing serialises the console across two kernels
    // and N6h rules the stream interleaves at byte granularity.
    //
    // WHO MAY WRITE A ROW: the kernel alone, into the row of the node it is running on, at the
    // request of that node's root. The node is derived in the kernel and is never a parameter,
    // so one writer per row survives userspace being what asks.
    //
    // WHAT THIS READER VALIDATES: the mark is not a flag but the port the partition names that
    // node, biased by one, so each row is checked against the port derived HERE from this
    // node's own copy of the list. Node 0's own row is swept as the known-value control, the
    // way the shared diagnostic record keeps cells whose values the reader already knows: a
    // sweep that gets this node's own mark wrong is an artefact and not a peer failure. A row
    // is a REPORT and is spent as neither an index nor a length. And the primary zeroed the
    // shared region before any peer was released, so a nonzero mark cannot be an earlier boot's.
    uint32_t const self_port = ampping_first_port((uint32_t)KOS_AMP_SELF_NODE);
    (void)kos_amp_probe(KOS_AMP_OP_APP_ALIVE_SET, self_port);

    unsigned alive = 0;
    unsigned own_row = 0;
    int settle;
    for (settle = 0; settle < AMPPING_TRIES; settle++)
    {
        uint32_t seen;
        alive = 0;
        own_row = 0;
        for (seen = 0; seen < (uint32_t)KICKOS_AMP_NODES; seen++)
        {
            uint32_t const want = ampping_first_port(seen);
            intptr_t const mark = (intptr_t)kos_amp_probe(KOS_AMP_OP_APP_ALIVE, seen);
            if (want == KOS_AMP_NO_ENTRY or mark < 0)
            {
                continue;
            }
            if ((uint32_t)mark != want + 1u)
            {
                continue;
            }
            alive++;
            if (seen == (uint32_t)KOS_AMP_SELF_NODE)
            {
                own_row = 1;
            }
        }
        if (alive == (unsigned)KICKOS_AMP_NODES)
        {
            break;
        }
        // A peer this app never calls reaches its own app in its own time, so an early sweep
        // reads a node that is still booting rather than one that failed.
        kos_sleep_ns(AMPPING_SETTLE_NS);
    }
    // The sweep says every node has QUEUED its boot output, not that the bytes have drained;
    // no run has ever shown a tear from that gap, so nothing waits for it here.
    printf("ampping: %u of %u node app(s) alive on the port the partition names, own row %u\n",
           alive, (unsigned)KICKOS_AMP_NODES, own_row);
#endif

    printf("ampping: node %u calls node %u port %u\n", (unsigned)KOS_AMP_SELF_NODE,
           (unsigned)peer, (unsigned)port);

    unsigned answered = 0;
    int round;
    for (round = 1; round <= AMPPING_ROUNDS; round++)
    {
        // One buffer, in and out: kos_call sends from it and writes the reply back into it.
        unsigned char msg[4];
        msg[0] = (unsigned char)round;
        msg[1] = 0xA1u;
        msg[2] = 0xA2u;
        msg[3] = 0xA3u;
        int32_t n = kos_call_timed(ep, msg, sizeof(msg), sizeof(msg), AMPPING_CALL_US);
        // Retry is the application's and not the kernel's (N6e): the peer is released after
        // this node and parks in its own time, so the first call can find no receiver.
        //
        // ZERO IS A REFUSAL HERE AND NOT AN ANSWER. A serving node that refuses a call past
        // the point it took the slot publishes an EMPTY reply carrying the tag, which is the
        // only thing that can wake a caller its receiver never served; this app asked for four
        // bytes back, so a reply of none is that refusal and not a peer with nothing to say.
        int tries = 1;
        while (n <= 0 and tries < AMPPING_TRIES)
        {
            kos_sleep_ns(AMPPING_SETTLE_NS);
            n = kos_call_timed(ep, msg, sizeof(msg), sizeof(msg), AMPPING_CALL_US);
            tries++;
        }
        if (n <= 0)
        {
            printf("ampping: round %d refused after %d attempt(s), rc %ld\n", round, tries,
                   (long)n);
            return 1;
        }
        if (tries > 1)
        {
            printf("  (node %u answered on attempt %d)\n", (unsigned)peer, tries);
        }
        // ASSERTED HERE AND NOT BY A GATE READING THE LINE BELOW. The answer is the peer's own
        // transformation of the payload, which is what separates a served call from a wake, and
        // the summary after this loop is printed only for rounds that carried it.
        if ((size_t)n != sizeof(msg))
        {
            printf("ampping: round %d came back with %ld of %u byte(s)\n", round, (long)n,
                   (unsigned)sizeof(msg));
            return 1;
        }
        if (msg[0] != (unsigned char)(round + 1))
        {
            printf("ampping: round %d came back as %u, not %u\n", round, (unsigned)msg[0],
                   (unsigned)(round + 1));
            return 1;
        }
        answered++;
        // FOR A HUMAN READING A HANG. No gate counts these.
        printf("  ping %d -> pong %u from node %u (%ld byte(s))\n", round, (unsigned)msg[0],
               (unsigned)peer, (long)n);
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // WHAT WITNESSES THE CROSSING, and the console is not it (docs/design-multicore.md N6h):
    // a peer is witnessed through a counter the other node reads and never through what it
    // printed. The serving app bumps its own row ahead of every reply, so the row a reader on
    // this node holds cannot be cut in half by a peer writing the same UART, where every line
    // above can. This is the same record the app-alive sweep reads and the same one-writer-per-
    // row rule; no second channel and nothing new crosses.
    //
    // POLLED RATHER THAN READ ONCE: the row is stored relaxed, so a run where this node holds
    // the answer before the peer's store is visible reads again instead of reporting a short
    // row. A row still short after the last try is REPORTED as it stands, the gate's clause
    // being what fails on it.
    unsigned long served = 0;
    int wait;
    for (wait = 0; wait < AMPPING_TRIES; wait++)
    {
        served = (unsigned long)kos_amp_probe(KOS_AMP_OP_APP_SERVED, peer);
        if (served >= (unsigned long)answered)
        {
            break;
        }
        kos_sleep_ns(AMPPING_SETTLE_NS);
    }
    printf("ampping: node %u answered %u round(s), its own record says %lu\n", (unsigned)peer,
           answered, served);
#endif

    printf("ampping: node %u done, %d round(s) across the partition\n",
           (unsigned)KOS_AMP_SELF_NODE, AMPPING_ROUNDS);

#if defined(KICKOS_ENABLE_SELFTEST)
    // The ring is the authority and a raise is a hint (docs/design-multicore.md N6f): a peer
    // that cannot yet be poked is published to anyway, and a skipped raise costs latency and
    // never a message. The peer's own counter, read out of the shared window, is what says the
    // message arrived; an absence is satisfied by one that never did.
    //
    // Every node's counter is taken AHEAD of the publication and one row afterwards: which node
    // the kernel published at is reported only once the probe returns, so a single row taken
    // after it would be read across the publication it is measuring.
    // THE PEER MUST BE BACK ON ITS RECEIVE BEFORE THE NOTICE CALL BELOW. A far call finding
    // nothing parked on the port is refused on the spot rather than held (N6f), and a serving
    // app is off its receive between answering the last round and parking again. The notice
    // would come back empty and this arm would report a loss it never saw.
    kos_sleep_ns(AMPPING_SETTLE_NS);
    unsigned long took0[KICKOS_AMP_NODES];
    uint32_t row;
    for (row = 0; row < (uint32_t)KICKOS_AMP_NODES; row++)
    {
        took0[row] = (unsigned long)kos_amp_probe(KOS_AMP_OP_TOOK, row);
    }
    // A refusal and a packed answer come back through the SAME unsigned word, so the word is
    // read as signed before anything is decoded out of it. A doorbell whose raise names its
    // peer with one register write keeps no seat to clear and refuses this scenario ahead of
    // the publication; decoded instead of read, that -KOS_ENOSYS is a count of 4294967295 at a
    // node of 65498, and the app reports it as an ordinary partition it cannot carry a notice
    // on.
    uintptr_t const deferred = kos_amp_probe(KOS_AMP_OP_DEFER, 0);
    if ((intptr_t)deferred < 0)
    {
        printf("ampping: the doorbell has no seat, so no raise of it can be deferred (rc %ld)\n",
               (long)(intptr_t)deferred);
        return 0;
    }
    unsigned const skipped = (unsigned)(deferred >> 16);
    uint32_t const at = (uint32_t)(deferred & 0xFFFFu);
    // The notice goes to the node the publication went to, which only the kernel names. The
    // capability the rounds ran on is the FIRST crossing this node holds, and that is the same
    // node only where the partition names it no other.
    uint32_t at_port = 0;
    kos_cap_t const at_ep = ampping_port_of(at, &at_port);
    if (at_ep == KOS_CAP_NONE)
    {
        printf("ampping: deferred %u raise(s) skipped at node %u, which the partition names no "
               "port: nothing this app holds can carry its notice\n",
               skipped, (unsigned)at);
        return 0;
    }
    // One publication left with no notice at all.
    unsigned char beat[4];
    beat[0] = 0xE0u;
    beat[1] = 0xE1u;
    beat[2] = 0xE2u;
    beat[3] = 0xE3u;
    int32_t const woke =
        kos_call_timed(at_ep, beat, sizeof(beat), sizeof(beat), AMPPING_CALL_US);
    unsigned long const took1 = (unsigned long)kos_amp_probe(KOS_AMP_OP_TOOK, at);
    // ASSERTED AND NOT ONLY PRINTED, and ahead of the line below so a bad answer costs the
    // gate the report it parses rather than only changing a field of it. A refusal is not an
    // error: n == 0 is the empty reply a serving node publishes when it refuses past the take,
    // so the notice was taken (the count below still moves) and nothing served it. Retry is
    // not open here as it is in the rounds, since every further call moves that same count.
    if (woke < 0)
    {
        printf("ampping: the notice call to node %u failed, rc %ld\n", (unsigned)at,
               (long)woke);
        return 1;
    }
    if (woke == 0)
    {
        printf("ampping: node %u refused the notice call past its take\n", (unsigned)at);
        return 1;
    }
    if ((size_t)woke != sizeof(beat))
    {
        printf("ampping: node %u answered the notice with %ld of %u byte(s)\n", (unsigned)at,
               (long)woke, (unsigned)sizeof(beat));
        return 1;
    }
    // The count is TWO: the publication whose raise was skipped, and the call that carried the
    // next notice. One would mean the deferred message was lost and only the call arrived.
    // Both nodes are printed so the gate can assert they are the same one.
    printf("ampping: deferred %u raise(s) skipped at node %u, notice to node %u port %u, took "
           "%lu message(s), call rc %ld\n",
           skipped, (unsigned)at, (unsigned)at, (unsigned)at_port, took1 - took0[at],
           (long)woke);
#endif
    return 0;
}
