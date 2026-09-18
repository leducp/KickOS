// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Serve the first partition port assigned to this node using ordinary
// receive/reply calls. The service handles local and far callers alike.
// Never return from root: that would shut down the partition's machine.

#include <stdbool.h>
#include <stdio.h>

#include <kickos/amp.h>
#include <kickos/sys.h>
#include <kickos/sys/abi_probe.h>

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

#if defined(KICKOS_ENABLE_SELFTEST)
    // Publish startup in the shared record so node 0 can observe uncalled peers.
    // Console output can interleave across kernels, so it is not a reliable marker.
    // Publish after queuing the boot line. The kernel validates the claimed port
    // and writes its own partition-derived value to the shared record.
    if ((intptr_t)kos_amp_probe(KOS_AMP_OP_APP_ALIVE_SET, port) < 0)
    {
        printf("ampping: node %u could not publish the port it serves\n",
               (unsigned)KOS_AMP_SELF_NODE);
    }
#endif

    while (true)
    {
        unsigned char msg[16];
        struct kos_reply_recv_opts opts = {0};
        int32_t got;
        opts.timeout_us = AMPPING_RECV_US;
        // NOT the zero the initialiser leaves: KOS_CAP_NONE is all-ones and zero is a real
        // capability index, KOS_CAP_STDOUT. A path where the kernel never reaches the info
        // write, an expiry among them, leaves exactly what is here.
        opts.info.reply_cap = KOS_CAP_NONE;
        opts.ep = ep;
        got = kos_reply_recv(KOS_CAP_NONE, msg, kos_call_lens_pack(0, sizeof(msg)), &opts);
        if (got < 0)
        {
            // A REPLY CAPABILITY CAN OUTLIVE A REFUSED ARRIVAL, and <kickos/sys.h> asks this
            // loop to answer or close on EVERY path: a far caller under KOS_TIMEOUT_NONE has
            // no deadline to fall back on. Tested against KOS_CAP_NONE and never for a sign,
            // per struct kos_recv_info.
            if (opts.info.reply_cap != KOS_CAP_NONE)
            {
                (void)kos_reply(opts.info.reply_cap, msg, 0);
            }
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
