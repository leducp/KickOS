// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Echo calls on the crossing this node's composition serves, the first partition port named
// for it. The node's other ports are served by tasks that never receive, for the reply-guard
// arms.

#include <iso646.h> // and / or / not are macros in C, not keywords
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "book.h"

#include <kickos/amp.h>
#include <kickos/sys.h>

// Written before the first receive, so a caller this node has answered reads it. The selftest's
// amp_share_crossing states the same line and word.
#define AMP_SHARE_LINE 64u
#define AMP_SHARE_MARK(node) (0x53480000u | (uint32_t)(node))

void ampecho_spare(kos_self_t const* self)
{
    (void)self;
    while (true)
    {
        kos_sleep_ns(1000000000ull);
    }
}

void ampecho_main(kos_self_t const* self)
{
    kos_cap_t ep = KOS_CAP_NONE;
    uint32_t const port = ampping_crossing(self, KOS_CAP_WAIT, KOS_AMP_NO_ENTRY, &ep);
    if (port == KOS_AMP_NO_ENTRY or ep == KOS_CAP_NONE)
    {
        printf("ampecho: node %u serves no port\n", (unsigned)KOS_AMP_SELF_NODE);
        exit(1);
    }
    kos_window_t const share = kos_grant_mem(self, "/shm/share");
    unsigned char* const view = (unsigned char*)kos_window_addr(share);
    if (view != NULL and kos_window_size(share) >= (KOS_AMP_SELF_NODE + 1u) * AMP_SHARE_LINE)
    {
        __atomic_store_n((uint32_t*)(view + KOS_AMP_SELF_NODE * AMP_SHARE_LINE),
                         AMP_SHARE_MARK(KOS_AMP_SELF_NODE), __ATOMIC_RELAXED);
    }
    printf("ampecho: node %u echoing on port %u\n", (unsigned)KOS_AMP_SELF_NODE,
           (unsigned)port);

    // Untimed on purpose. A far call finding nothing parked on the port is refused ON THE SPOT
    // (N6f) rather than held, so a timeout here leaves this peer off its receive between one
    // expiry and the next park, and a caller landing in that gap waits out its own deadline.
    while (true)
    {
        unsigned char msg[KOS_EP_MSG_MAX];
        struct kos_recv_info info = {0};
        int32_t got;
        // NOT the zero the initialiser leaves: KOS_CAP_NONE is all-ones and zero is a real
        // capability index, KOS_CAP_STDOUT. A path where the kernel never reaches the info
        // write leaves exactly what is here.
        info.reply_cap = KOS_CAP_NONE;
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, ep, 0, KOS_TIMEOUT_NONE);
        got = kos_reply_recv(KOS_CAP_NONE, msg, kos_call_lens_pack(0, sizeof(msg)), &opts);
        info = opts.info;
        if (got < 0)
        {
            // A REPLY CAPABILITY CAN OUTLIVE A REFUSED ARRIVAL, and <kickos/sys.h> asks this
            // loop to answer or close on EVERY path: a far caller under KOS_TIMEOUT_NONE has
            // no deadline to fall back on. Tested against KOS_CAP_NONE and never for a sign,
            // per struct kos_recv_info.
            if (info.reply_cap != KOS_CAP_NONE)
            {
                (void)kos_reply(info.reply_cap, msg, 0);
            }
            continue;
        }
        if (info.reply_cap == KOS_CAP_NONE)
        {
            continue; // a send and not a call: there is nothing to answer
        }
        // The caller's own bytes, unchanged: the far-call arms check the payload.
        (void)kos_reply(info.reply_cap, msg, (size_t)got);
    }
}
