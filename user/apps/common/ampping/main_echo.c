// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A serving node for the selftest to talk to. It answers every call on the port the partition
// gives it with the caller's own bytes: a far call is witnessed by the payload coming back, so
// the peer has to return it rather than transform it.
//
// The partition names this node TWO ports and the kernel binds both. This app receives on the
// FIRST alone: the second is deliberately left with no receiver, so a caller parks on it and
// stays parked, which is what the reply-guard arm needs (docs/design-multicore.md N6f).
//
// It never returns, for the reason <kickos/amp.h> states: root returning ends the system, and
// the nodes of a partition share one machine.

#include <stdbool.h>
#include <stdio.h>

#include <kickos/amp.h>
#include <kickos/sys.h>


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
    printf("ampecho: node %u echoing on port %u\n", (unsigned)KOS_AMP_SELF_NODE,
           (unsigned)port);

    // Untimed on purpose. A far call finding nothing parked on the port is refused ON THE SPOT
    // (N6f) rather than held, so a timeout here leaves this peer off its receive between one
    // expiry and the next park, and a caller landing in that gap waits out its own deadline.
    while (true)
    {
        unsigned char msg[KOS_EP_MSG_MAX];
        struct kos_recv_info info = {0};
        int32_t const got = kos_recv(ep, msg, sizeof(msg), &info);
        if (got < 0)
        {
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
