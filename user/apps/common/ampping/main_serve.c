// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A serving node of the ampping partition: it answers the crossing its composition serves with
// ordinary receive and reply calls, and keeps its own row of the partition region.

#include <iso646.h> // and / or / not are macros in C, not keywords
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "book.h"

#include <kickos/amp.h>
#include <kickos/sys.h>

#define AMPPING_RECV_US (2u * 1000u * 1000u)
#define AMPPING_IDLE_NS (100ull * 1000ull * 1000ull)

void ampserve_main(kos_self_t const* self)
{
    kos_cap_t ep = KOS_CAP_NONE;
    uint32_t const port = ampping_crossing(self, KOS_CAP_WAIT, KOS_AMP_NO_ENTRY, &ep);
    kos_window_t const region = kos_grant_mem(self, AMPBOOK_NAME);
    struct ampbook_row* const book = (struct ampbook_row*)kos_window_addr(region);
    if (port == KOS_AMP_NO_ENTRY or ep == KOS_CAP_NONE or book == NULL
        or kos_window_size(region) < KICKOS_AMP_NODES * sizeof(struct ampbook_row))
    {
        printf("ampping: node %u serves no port\n", (unsigned)KOS_AMP_SELF_NODE);
        exit(1);
    }
    printf("ampping: node %u serves port %u\n", (unsigned)KOS_AMP_SELF_NODE, (unsigned)port);
    struct ampbook_row* const row = &book[KOS_AMP_SELF_NODE];
    ampping_gate_serve(self, row);
    // After the announcement is queued: node 0 prints nothing a gate reads before every row is set.
    atomic_store_explicit(&row->alive, port + 1u, memory_order_release);

    uint32_t served = 0;
    while (true)
    {
        unsigned char msg[16];
        struct kos_reply_recv_opts opts = {0};
        opts.timeout_us = AMPPING_RECV_US;
        // KOS_CAP_NONE is all-ones and zero is KOS_CAP_STDOUT: a path where the kernel never
        // writes the info leaves exactly what is here.
        opts.info.reply_cap = KOS_CAP_NONE;
        opts.ep = ep;
        int32_t const got = kos_reply_recv(KOS_CAP_NONE, msg, kos_call_lens_pack(0, sizeof(msg)), &opts);
        if (got < 0)
        {
            // A reply capability can outlive a refused arrival, and a far caller with no deadline
            // waits on it.
            if (opts.info.reply_cap != KOS_CAP_NONE)
            {
                (void)kos_reply(opts.info.reply_cap, msg, 0);
            }
            kos_sleep_ns(AMPPING_IDLE_NS);
            continue;
        }
        if (opts.info.reply_cap == KOS_CAP_NONE)
        {
            continue;
        }
        if (got < 1)
        {
            (void)kos_reply(opts.info.reply_cap, msg, 0);
            continue;
        }
        unsigned char rep[4];
        rep[0] = (unsigned char)(msg[0] + 1u);
        rep[1] = 0xB1u;
        rep[2] = 0xB2u;
        rep[3] = 0xB3u;
        printf("  serve %u -> %u\n", (unsigned)msg[0], (unsigned)rep[0]);
        // Ahead of the reply: the caller reads this row as soon as its answer lands.
        served++;
        atomic_store_explicit(&row->served, served, memory_order_release);
        (void)kos_reply(opts.info.reply_cap, rep, sizeof(rep));
    }
}
