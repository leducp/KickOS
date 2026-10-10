// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Node 0 of the ampping partition: it calls the first peer across the crossing its composition
// uses, and reads every node's row of the partition region the partition build placed in the
// user share. Locality never reaches the API (docs/design-multicore.md N7): this is
// kos_call_timed.

#include <iso646.h> // and / or / not are macros in C, not keywords
#include <stdio.h>
#include <stdlib.h>

#include "book.h"

#include <kickos/amp.h>
#include <kickos/sys.h>

#define AMPPING_ROUNDS 4
#define AMPPING_CALL_US (500u * 1000u)
#define AMPPING_SETTLE_NS (20ull * 1000ull * 1000ull)
#define AMPPING_TRIES 40

// The first port the partition names `node`, or KOS_AMP_NO_ENTRY where it names none.
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

// The node the partition list names the server of `port`.
static uint32_t ampping_server(uint32_t port)
{
    uint32_t i;
    for (i = 0; i < KOS_AMP_PORT_COUNT; i++)
    {
        if (kos_amp_entry_port(i) == port)
        {
            return kos_amp_entry_node(i);
        }
    }
    return KOS_AMP_NO_ENTRY;
}

void ampping_main(kos_self_t const* self)
{
    kos_cap_t ep = KOS_CAP_NONE;
    uint32_t const port = ampping_crossing(self, KOS_CAP_SIGNAL, KOS_AMP_NO_ENTRY, &ep);
    kos_window_t const region = kos_grant_mem(self, AMPBOOK_NAME);
    struct ampbook_row* const book = (struct ampbook_row*)kos_window_addr(region);
    if (port == KOS_AMP_NO_ENTRY or ep == KOS_CAP_NONE or book == NULL
        or kos_window_size(region) < KICKOS_AMP_NODES * sizeof(struct ampbook_row))
    {
        printf("ampping: node %u holds no crossing to call or no partition region\n", (unsigned)KOS_AMP_SELF_NODE);
        exit(1);
    }
    uint32_t const peer = ampping_server(port);

    // NOTHING A GATE READS MAY BE PRINTED ABOVE THE SWEEP: every node queues its whole boot output
    // before it marks its row, and two kernels share one console with no lock across them (N6h).
    uint32_t const own = ampping_first_port((uint32_t)KOS_AMP_SELF_NODE);
    atomic_store_explicit(&book[KOS_AMP_SELF_NODE].alive, own + 1u, memory_order_release);
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
            uint32_t const mark = atomic_load_explicit(&book[seen].alive, memory_order_acquire);
            if (want == KOS_AMP_NO_ENTRY or mark != want + 1u)
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
        kos_sleep_ns(AMPPING_SETTLE_NS);
    }
    printf("ampping: %u of %u node app(s) alive on the port the partition names, own row %u\n", alive,
           (unsigned)KICKOS_AMP_NODES, own_row);
    ampping_gate_report(self, book);
    printf("ampping: node %u calls node %u port %u\n", (unsigned)KOS_AMP_SELF_NODE, (unsigned)peer, (unsigned)port);

    unsigned answered = 0;
    int round;
    for (round = 1; round <= AMPPING_ROUNDS; round++)
    {
        unsigned char msg[4];
        msg[0] = (unsigned char)round;
        msg[1] = 0xA1u;
        msg[2] = 0xA2u;
        msg[3] = 0xA3u;
        int32_t n = kos_call_timed(ep, msg, sizeof(msg), sizeof(msg), AMPPING_CALL_US);
        // The peer parks in its own time, so the first call can find no receiver (N6e). Zero is
        // the empty reply of a refusal past the take, not an answer.
        int tries = 1;
        while (n <= 0 and tries < AMPPING_TRIES)
        {
            kos_sleep_ns(AMPPING_SETTLE_NS);
            n = kos_call_timed(ep, msg, sizeof(msg), sizeof(msg), AMPPING_CALL_US);
            tries++;
        }
        if (n <= 0)
        {
            printf("ampping: round %d refused after %d attempt(s), rc %ld\n", round, tries, (long)n);
            exit(1);
        }
        if (tries > 1)
        {
            printf("  (node %u answered on attempt %d)\n", (unsigned)peer, tries);
        }
        if ((size_t)n != sizeof(msg))
        {
            printf("ampping: round %d came back with %ld of %u byte(s)\n", round, (long)n, (unsigned)sizeof(msg));
            exit(1);
        }
        if (msg[0] != (unsigned char)(round + 1))
        {
            printf("ampping: round %d came back as %u, not %u\n", round, (unsigned)msg[0], (unsigned)(round + 1));
            exit(1);
        }
        answered++;
        printf("  ping %d -> pong %u from node %u (%ld byte(s))\n", round, (unsigned)msg[0], (unsigned)peer, (long)n);
    }

    // The peer's own row, which it stores ahead of every reply: a reply the kernel publishes
    // with no thread behind it moves no row.
    unsigned long served = 0;
    int wait;
    for (wait = 0; wait < AMPPING_TRIES; wait++)
    {
        served = atomic_load_explicit(&book[peer].served, memory_order_acquire);
        if (served >= (unsigned long)answered)
        {
            break;
        }
        kos_sleep_ns(AMPPING_SETTLE_NS);
    }
    printf("ampping: node %u answered %u round(s), its own record says %lu\n", (unsigned)peer, answered, served);
    printf("ampping: node %u done, %d round(s) across the partition\n", (unsigned)KOS_AMP_SELF_NODE,
           AMPPING_ROUNDS);
}
