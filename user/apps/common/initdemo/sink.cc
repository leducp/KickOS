// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// initdemo's console sink, a program of its own: counts the bytes it receives on the delegated
// endpoint into the block its creator shared through its window. A negative receive (dead
// endpoint) ends it; otherwise it parks in receive until shutdown.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include "sink.h"

int main(int, char**)
{
    kos_window w = {};
    if (kos_window_get(0, &w) != 0)
    {
        return 1;
    }
    initdemo::SinkCount* const count = reinterpret_cast<initdemo::SinkCount*>(w.base);
    int const ep = KOS_SPAWN_DELEGATED_CAP0;
    char buf[KOS_EP_MSG_MAX];
    while (true)
    {
        // Info-less recv: this sink counts plain sends only (a client kos_call bounces
        // -KOS_ENOTSUP rather than minting a reply cap here).
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, ep, KOS_RECV_NO_INFO, KOS_TIMEOUT_NONE);
        int32_t const n = kos_reply_recv(KOS_CAP_NONE, buf, kos_call_lens_pack(0, sizeof(buf)),
                                         &opts);
        if (n < 0)
        {
            break;
        }
        count->bytes = count->bytes + n;
    }
    return 0;
}
