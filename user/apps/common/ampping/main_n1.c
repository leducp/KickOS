// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The SERVING node of a two-image AMP partition: an ordinary thread parked in an ordinary
// receive, handed an ordinary reply capability. Nothing below can tell a far caller from a near
// one, which is the transparency docs/design-multicore.md N6d requires.
//
// It never returns: root returning ends the system, and on this partition that is one machine
// both nodes run on, so a serving node's main parks instead (<kickos/sys/init.h>).

#include <stdbool.h>
#include <stdio.h>

#include <kickos/amp.h>
#include <kickos/sys.h>

#define AMPPING_RECV_US (2u * 1000u * 1000u)
#define AMPPING_IDLE_NS (100ull * 1000ull * 1000ull)

// The port this image SERVES: the first entry of the partition's list naming this node. Its
// capability is a local endpoint the kernel already bound the port to.
static kos_cap_t ampping_service(uint32_t* out_port)
{
    uint32_t i;
    for (i = 0; i < KOS_AMP_PORT_COUNT; i++)
    {
        uint32_t const node = kos_amp_entry_node(i);
        uint32_t const port = kos_amp_entry_port(i);
        if (node != KOS_AMP_SELF_NODE)
        {
            continue;
        }
        *out_port = port;
        return kos_amp_port(node, port);
    }
    return KOS_CAP_NONE;
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;
    uint32_t port = 0;
    kos_cap_t const ep = ampping_service(&port);
    if (ep == KOS_CAP_NONE)
    {
        printf("ampping: node %u serves no port\n", (unsigned)KOS_AMP_SELF_NODE);
        return 1;
    }
    printf("ampping: node %u serves port %u\n", (unsigned)KOS_AMP_SELF_NODE, (unsigned)port);

    while (true)
    {
        unsigned char msg[16];
        struct kos_recv_timed_opts opts = {0};
        int32_t got;
        opts.timeout_us = AMPPING_RECV_US;
        got = kos_recv_timed(ep, msg, sizeof(msg), &opts);
        if (got < 0)
        {
            kos_sleep_ns(AMPPING_IDLE_NS);
            continue;
        }
        if (opts.info.reply_cap == KOS_CAP_NONE)
        {
            // A send and not a call: there is nothing to answer (N6e).
            continue;
        }
        unsigned char rep[4];
        rep[0] = (unsigned char)(msg[0] + 1u);
        rep[1] = 0xB1u;
        rep[2] = 0xB2u;
        rep[3] = 0xB3u;
        printf("  serve %u -> %u\n", (unsigned)msg[0], (unsigned)rep[0]);
        (void)kos_reply(opts.info.reply_cap, rep, sizeof(rep));
    }
    return 0;
}
