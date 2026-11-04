// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// tlsprobe's peer, a program of its own: thread-local storage of its own, seeded and laid out
// apart from main's, which its entry thread finds as its image states it at every create and
// keeps across switches into main's task, its exit status the verdict.

#include <kickos/kos.h>

#include "tlsprobe.h"

namespace
{
    // Volatile, or a thread_local only ever read is folded and its .tdata comes out empty.
    thread_local volatile unsigned g_peer_pad[5] = {0x11u, 0x22u, 0x33u, 0x44u, 0x55u};
    thread_local volatile unsigned g_peer_seeded = 0x5EED9EE2u;
    thread_local volatile unsigned g_peer_zero[3];
}

int main(int, char**)
{
    if (g_peer_seeded != 0x5EED9EE2u)
    {
        kos_exit(TLSPROBE_PEER_TEMPLATE);
    }
    for (unsigned i = 0; i < 5u; i++)
    {
        if (g_peer_pad[i] != 0x11u * (i + 1u))
        {
            kos_exit(TLSPROBE_PEER_TEMPLATE);
        }
    }
    for (unsigned i = 0; i < 3u; i++)
    {
        if (g_peer_zero[i] != 0u)
        {
            kos_exit(TLSPROBE_PEER_ZERO);
        }
    }
    for (unsigned i = 0; i < 5u; i++)
    {
        g_peer_pad[i] = 0x9EE20000u + i;
    }
    g_peer_seeded = 0u;
    g_peer_zero[2] = 1u;
    // Main writes its own thread-local storage while this thread sleeps.
    kos::sleep_ns(20000000ull);
    for (unsigned i = 0; i < 5u; i++)
    {
        if (g_peer_pad[i] != 0x9EE20000u + i)
        {
            kos_exit(TLSPROBE_PEER_READ_BACK);
        }
    }
    if (g_peer_seeded != 0u or g_peer_zero[2] != 1u)
    {
        kos_exit(TLSPROBE_PEER_READ_BACK);
    }
    kos_exit(TLSPROBE_PEER_OK);
}
