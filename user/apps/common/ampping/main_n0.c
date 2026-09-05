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

#include <stdio.h>

#include <kickos/amp.h>
#include <kickos/sys.h>

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
    printf("ampping: node %u calls node %u port %u\n", (unsigned)KOS_AMP_SELF_NODE,
           (unsigned)peer, (unsigned)port);

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
        int tries = 1;
        while (n < 0 && tries < AMPPING_TRIES)
        {
            kos_sleep_ns(AMPPING_SETTLE_NS);
            n = kos_call_timed(ep, msg, sizeof(msg), sizeof(msg), AMPPING_CALL_US);
            tries++;
        }
        if (n < 0)
        {
            printf("ampping: round %d refused after %d attempt(s), rc %ld\n", round, tries,
                   (long)n);
            return 1;
        }
        if (tries > 1)
        {
            printf("  (node %u answered on attempt %d)\n", (unsigned)peer, tries);
        }
        printf("  ping %d -> pong %u from node %u (%ld byte(s))\n", round, (unsigned)msg[0],
               (unsigned)peer, (long)n);
    }
    printf("ampping: node %u done, %d round(s) across the partition\n",
           (unsigned)KOS_AMP_SELF_NODE, AMPPING_ROUNDS);

    // The ring is the authority and a raise is a hint (docs/design-multicore.md N6f): a peer
    // that cannot yet be poked is published to anyway, and a skipped raise costs latency and
    // never a message. The peer's own counter, read out of the shared window, is what says the
    // message arrived; an absence is satisfied by one that never did.
    //
    // Every node's counter is taken AHEAD of the publication and one row afterwards: which node
    // the kernel published at is reported only once the probe returns, so a single row taken
    // after it would be read across the publication it is measuring.
    unsigned long took0[KICKOS_AMP_NODES];
    uint32_t row;
    for (row = 0; row < (uint32_t)KICKOS_AMP_NODES; row++)
    {
        took0[row] = (unsigned long)kos_aspace_probe(KOS_ASPACE_OP_AMP_TOOK, row);
    }
    uintptr_t const deferred = kos_aspace_probe(KOS_ASPACE_OP_AMP_DEFER, 0);
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
    unsigned long const took1 = (unsigned long)kos_aspace_probe(KOS_ASPACE_OP_AMP_TOOK, at);
    // The count is TWO: the publication whose raise was skipped, and the call that carried the
    // next notice. One would mean the deferred message was lost and only the call arrived.
    // Both nodes are printed so the gate can assert they are the same one.
    printf("ampping: deferred %u raise(s) skipped at node %u, notice to node %u port %u, took "
           "%lu message(s), call rc %ld\n",
           skipped, (unsigned)at, (unsigned)at, (unsigned)at_port, took1 - took0[at],
           (long)woke);
    return 0;
}
