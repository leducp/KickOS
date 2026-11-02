// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// taskleave's peer, a program of its own: its entry thread is the closer, pinned to core 1 and
// parked, and the leaver it spawns on core 0 releases it and dies, so the closer's departure is
// what empties the task.

#include <kickos/kos.h>
#include <kickos/sys.h>

namespace
{
    kos_sem_t g_sig = KOS_SEM_INIT(0);

    // Long enough for the creator to read the live space count while the closer is parked.
    constexpr uint64_t PARK_NS = 20u * 1000u * 1000u;

    void leaver(void*)
    {
        (void)kos_sem_post(&g_sig);
        kos_exit(0);
    }
}

int main(int, char**)
{
    if (kos_thread_affinity(kos_thread_self(), 1u << 1) < 0)
    {
        return 1;
    }
    kos::sleep_ns(PARK_NS);
    auto const a = kos::thread::create_caps(leaver, nullptr, "leaver", 10, nullptr, 0,
                                            KOS_POLICY_FIFO, 0, 0, nullptr, nullptr, 0, 1u << 0);
    if (not a.valid())
    {
        return 1;
    }
    (void)kos_sem_wait(&g_sig, KOS_TIMEOUT_NONE);
    return 0;
}
