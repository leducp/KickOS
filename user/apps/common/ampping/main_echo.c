// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Echo calls on the first partition port assigned to this node. Leave other
// ports unanswered for reply-guard tests. Never return from root: that would
// shut down the machine shared by the partition.

#include <iso646.h> // and / or / not are macros in C, not keywords
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <kickos/amp.h>
#include <kickos/sys.h>

// Written before the first receive, so a caller this node has answered reads it. The selftest's
// amp_share_crossing states the same line and word.
#define AMP_SHARE_LINE 64u
#define AMP_SHARE_MARK(node) (0x53480000u | (uint32_t)(node))

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    uint32_t port = KOS_AMP_NO_ENTRY;
    uint32_t i;
    for (i = 0; i < KOS_AMP_PORT_COUNT; i++)
    {
        if (kos_amp_entry_node(i) == KOS_AMP_SELF_NODE)
        {
            port = kos_amp_entry_port(i);
            break;
        }
    }
    if (port == KOS_AMP_NO_ENTRY)
    {
        printf("ampecho: node %u serves no port\n", (unsigned)KOS_AMP_SELF_NODE);
        return 1;
    }
    kos_cap_t const ep = kos_amp_port(KOS_AMP_SELF_NODE, port);
    if (KOS_AMP_SHARE_SIZE != 0
        and kos_mem_self_grant((void*)KOS_AMP_SHARE_BASE, KOS_AMP_SHARE_SIZE,
                              KOS_AMP_SHARE_MEM_FLAGS)
               == 0)
    {
        __atomic_store_n((uint32_t*)(KOS_AMP_SHARE_BASE + KOS_AMP_SELF_NODE * AMP_SHARE_LINE),
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
    return 0;
}
