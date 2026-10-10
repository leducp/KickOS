// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A PAGE WITHDRAWN UNDER A READER THAT HOLDS ITS TRANSLATION.
//
// The victim's stack is the page, unmapped on the victim's own core when it exits. The reader
// reads it while it stands and again once the victim is gone; that read must fault, ending this
// task. Between the two, main hands the kernel the dead page as a console write's buffer, which
// must be refused -KOS_EFAULT.
//
// Above one kernel core the reader spins on a core of its own with no syscall, so the unmap lands
// while its translation is cached. It must not read the page again before main is done with the
// kernel, or its fault ends the task first.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys.h>
#include <kickos/sys/errno.h>

#include <stdint.h>

namespace
{
    constexpr uint32_t MARK = 0x5A1E7A6Eu;

    volatile uintptr_t g_addr = 0;
    volatile uint32_t g_sink = 0;

    // Main's semaphores, delegated to the victim: it posts READY once its page is published
    // and leaves once GO is posted.
    constexpr kos_cap_t READY = KOS_SPAWN_DELEGATED_CAP0;
    constexpr kos_cap_t GO = KOS_SPAWN_DELEGATED_CAP0 + 1;

    void victim(void*)
    {
        volatile uint32_t mark = MARK;
        g_addr = reinterpret_cast<uintptr_t>(&mark);
        (void)kos_sem_post(READY);
        (void)kos_sem_wait(GO, KOS_TIMEOUT_NONE);
    }

    void announce(int core)
    {
        char msg[96];
        ksnprintf(msg, sizeof(msg), "[deadstack] the reader on core %d holds 0x%lx\n", core,
                  static_cast<unsigned long>(g_addr));
        kos_print(msg);
    }

    // Returns only on the refusal: anything else ends the run here.
    void kernel_read_after()
    {
        int32_t const rc =
            KICKOS_KCONSOLE_MEASURED(ANSWER, reinterpret_cast<void const*>(g_addr), sizeof(MARK));
        char msg[112];
        if (rc != -KOS_EFAULT)
        {
            ksnprintf(msg, sizeof(msg),
                      "[deadstack] DEADSTACK FAIL: the kernel took the dead page as a buffer: %d\n",
                      static_cast<int>(rc));
            kos_print(msg);
            kos_exit(1);
        }
        kos_print("[deadstack] the kernel refused the dead page as a buffer\n");
    }

    void read_after(volatile uint32_t const* p)
    {
        g_sink = *p;
        kos_print("[deadstack] DEADSTACK FAIL: the reader read the withdrawn page\n");
        kos_exit(1);
    }

#if KICKOS_KERNEL_CORES > 1
    // Each spinner has its core to itself: the reader must still be running, on the
    // translation it took, when the unmap lands.
    constexpr uint8_t PRIO_SPINNER = 10;
    constexpr uint64_t POLL_NS = 10u * 1000u * 1000u;
    constexpr uint32_t WARM_POLLS = 500u;
    // Past the reaping, how long the reader has to fault before main calls the run broken.
    constexpr uint64_t VERDICT_NS = 500u * 1000u * 1000u;

    volatile uint32_t g_warm = 0;
    volatile uint32_t g_reaped = 0;

    void reader(void*)
    {
        while (g_addr == 0)
        {
        }
        volatile uint32_t const* const p = reinterpret_cast<volatile uint32_t const*>(g_addr);
        while (*p != MARK)
        {
        }
        g_warm = 1;
        while (g_reaped == 0)
        {
        }
        read_after(p);
    }
#endif
}

int main(int, char**)
{
    kos_cap_t ready = KOS_CAP_NONE;
    kos_cap_t go = KOS_CAP_NONE;
    if (kos_sem_create(0, &ready) != 0 or kos_sem_create(0, &go) != 0)
    {
        kos_print("[deadstack] ERROR: no semaphore\n");
        return 1;
    }
    kos_cap_grant const caps[] = {{ready, KOS_CAP_SIGNAL}, {go, KOS_CAP_WAIT}};
    uint32_t victim_mask = 0;
#if KICKOS_KERNEL_CORES > 1
    // Off main's core: the reader spins above main's priority and would starve it there.
    int const here = kos_core_current();
    if (here < 0)
    {
        kos_print("[deadstack] ERROR: main's core is unreadable\n");
        return 1;
    }
    uint32_t const core_reader = (static_cast<uint32_t>(here) + 1u) % KICKOS_KERNEL_CORES;
    uint32_t const core_victim = (static_cast<uint32_t>(here) + 2u) % KICKOS_KERNEL_CORES;
    victim_mask = 1u << core_victim;
#endif
    uint8_t const ncaps = static_cast<uint8_t>(sizeof(caps) / sizeof(caps[0]));
    auto const v = kos::thread::create_caps(victim, nullptr, "victim", 10, caps, ncaps,
                                            KOS_POLICY_FIFO, 0, /*privileged=*/false, nullptr,
                                            0, 0, nullptr, KOS_TASK_NONE, nullptr, 0,
                                            victim_mask);
    if (not v.valid() or kos_sem_wait(ready, KOS_TIMEOUT_NONE) != 0)
    {
        kos_print("[deadstack] ERROR: the victim never published its page\n");
        return 1;
    }
#if KICKOS_KERNEL_CORES > 1
    auto const r = kos::thread::create(reader, nullptr, "reader", PRIO_SPINNER, KOS_POLICY_FIFO,
                                       0, /*privileged=*/false, nullptr, 0, nullptr, 0, nullptr,
                                       0, nullptr, 0, 0, nullptr, KOS_TASK_NONE,
                                       1u << core_reader);
    if (not r.valid())
    {
        kos_print("[deadstack] ERROR: reader spawn refused\n");
        return 1;
    }
    for (uint32_t polls = 0; g_warm == 0; polls++)
    {
        if (polls == WARM_POLLS)
        {
            kos_print("[deadstack] ERROR: the reader never read the victim's page\n");
            return 1;
        }
        kos_sleep_ns(POLL_NS);
    }
    announce(static_cast<int>(core_reader));
    (void)kos_sem_post(go);
    if (v.join(KOS_TIMEOUT_NONE) != 0)
    {
        kos_print("[deadstack] ERROR: the victim never came back\n");
        return 1;
    }
    kernel_read_after();
    g_reaped = 1;
    kos_sleep_ns(VERDICT_NS);
    kos_print("[deadstack] ERROR: the reader neither faulted nor reported\n");
    return 1;
#else
    volatile uint32_t const* const p = reinterpret_cast<volatile uint32_t const*>(g_addr);
    if (*p != MARK)
    {
        kos_print("[deadstack] ERROR: the victim's page does not hold its mark\n");
        return 1;
    }
    announce(0);
    g_sink = *p;
    (void)kos_sem_post(go);
    if (v.join(KOS_TIMEOUT_NONE) != 0)
    {
        kos_print("[deadstack] ERROR: the victim never came back\n");
        return 1;
    }
    kernel_read_after();
    read_after(p);
    return 1;
#endif
}
