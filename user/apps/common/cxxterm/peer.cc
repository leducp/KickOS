// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// cxxterm_worker's task under test, a program of its own: a worker throws an exception nothing
// catches while a sibling and main keep running. Only the task's end stops the sibling and main,
// so the creator seeing this task dead is the witness.

#include <stdexcept>

#include <kickos/kos.h>
#include <kickos/sys.h>

namespace
{
    kos_sem_t g_sibling_up = KOS_SEM_INIT(0);

    // Out of line, so the throw is not folded into a direct call to terminate.
    [[gnu::noinline]] void raise_uncaught()
    {
        throw std::runtime_error("cxxterm worker uncaught");
    }

    void thrower(void*)
    {
        (void)kos_sem_wait(&g_sibling_up, KOS_TIMEOUT_NONE);
        kos::print("cxxterm: worker throwing\n");
        raise_uncaught();
        kos::print("cxxterm: worker returned from an uncaught throw\n");
    }

    void sibling(void*)
    {
        kos::print("cxxterm: sibling running\n");
        (void)kos_sem_post(&g_sibling_up);
        while (true)
        {
            kos::sleep_ns(1000000ull);
        }
    }
}

int main(int, char**)
{
    constexpr uint8_t PRIO = 10;
    auto const s = kos::thread::create(sibling, nullptr, "sibling", PRIO);
    auto const t = kos::thread::create(thrower, nullptr, "thrower", PRIO);
    if (not s.valid() or not t.valid())
    {
        kos::print("cxxterm: peer spawn refused\n");
        return 1;
    }
    while (true)
    {
        kos::sleep_ns(1000000000ull);
    }
}
