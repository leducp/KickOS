// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// mutexbench's peer, a program of its own: the waiter of the window leg's handoff, reaching the
// handoff's word through the memory window its creator passed, which is all it shares.

#include <kickos/board_config.h>
#include <kickos/kos.h>
#include <kickos/sys.h>

#include "handoff.h"

#ifndef KICKOS_KERNEL_CORES
#define KICKOS_KERNEL_CORES 1
#endif

int main(int, char**)
{
#if KICKOS_KERNEL_CORES > 1
    if (kos_thread_affinity(kos_thread_self(), 1u << 1) < 0)
    {
        return 1;
    }
#endif
    kos_window at = {};
    if (kos_window_get(0, &at) != 0)
    {
        return 1;
    }
    mutexbench::WindowBlock* const b = reinterpret_cast<mutexbench::WindowBlock*>(at.base);
    mutexbench::waiter_run(&b->ho, b->wait);
    return 0;
}
