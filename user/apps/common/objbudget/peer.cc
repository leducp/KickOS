// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// objbudget's peer, a program of its own: creates an endpoint while its creator sits at its own
// ceiling, and answers the result down the endpoint its creator delegated.

#include <kickos/kos.h>
#include <kickos/sys.h>

namespace
{
    // The statics a create starts from the image's: one in .data and one in .bss.
    int g_seeded = 7;
    int g_runs = 0;
}

int main(int argc, char** argv)
{
    if (argc > 1 and argv[1][0] == 'c')
    {
        g_runs = g_runs + 1;
        g_seeded = g_seeded + 1;
        if (g_runs == 1 and g_seeded == 8)
        {
            return 0;
        }
        return 1;
    }
    constexpr kos_cap_t REPLY_EP = KOS_SPAWN_DELEGATED_CAP0;
    kos_cap_t ep = KOS_CAP_NONE;
    int rc = kos_endpoint_create(&ep);
    if (rc == 0)
    {
        (void)kos_handle_close(ep);
    }
    (void)kos_send(REPLY_EP, &rc, sizeof(rc), KOS_TIMEOUT_NONE);
    return 0;
}
