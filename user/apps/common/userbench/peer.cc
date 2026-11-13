// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The bench's peer, a program of its own, which main creates in two tasks at a time for the
// cross-task rows. argv[1] is the role: the two players of the ping-pong, the server and the
// caller of the call/reply row, or `exit`, which is the spawn row's task.

#include <stdlib.h>
#include <string.h>

#include <kickos/board_config.h>
#include <kickos/kos.h>
#include <kickos/sys.h>

#include "callreply.h"

#ifndef KICKOS_KERNEL_CORES
#define KICKOS_KERNEL_CORES 1
#endif

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        return 1;
    }
    char const* const role = argv[1];
    if (strcmp(role, "lead") == 0 or strcmp(role, "follow") == 0)
    {
        if (bench::PLAYER_CORES != 0 and KICKOS_KERNEL_CORES > 1)
        {
            (void)kos_thread_affinity(kos_thread_self(), bench::PLAYER_CORES);
        }
        bench::xt_player(strcmp(role, "lead") == 0);
        return 0;
    }
    if (strcmp(role, "server") == 0)
    {
        bench::callreply_server(nullptr);
        return 0;
    }
    if (strcmp(role, "caller") == 0 and argc > 2)
    {
        uintptr_t const params = static_cast<uintptr_t>(strtoul(argv[2], nullptr, 10));
        bench::callreply_caller(reinterpret_cast<void*>(params));
        return 0;
    }
    return 0;
}
