// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A PAGE WITHDRAWN UNDER A READER RUNNING ON A PEER CORE.
//
// The victim's stack is the page: the kernel maps it at spawn and unmaps it on the victim's
// own core when the victim exits. The reader, pinned to another core and spinning at the
// unprivileged level with no syscall, holds that page's translation until the unmap. Its next
// read must fault, and a fault ends this task. A read that returns is a stale translation the
// unmap's maintenance never reached.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys.h>

#include <stdint.h>

namespace
{
    // Each spinner has its core to itself: the reader must still be running, on the
    // translation it took, when the unmap lands.
    constexpr uint8_t PRIO_SPINNER = 10;
    constexpr uint32_t CORE_READER = 1;
    constexpr uint32_t CORE_VICTIM = 2;

    constexpr uint32_t MARK = 0x5A1E7A6Eu;
    constexpr uint64_t POLL_NS = 10u * 1000u * 1000u;
    // Past the reaping, how long the reader has to fault before main calls the run broken.
    constexpr uint64_t VERDICT_NS = 500u * 1000u * 1000u;

    volatile uintptr_t g_addr = 0;
    volatile uint32_t g_warm = 0;
    volatile uint32_t g_go = 0;
    volatile uint32_t g_reaped = 0;
    volatile uint32_t g_sink = 0;

    void victim(void*)
    {
        volatile uint32_t mark = MARK;
        g_addr = reinterpret_cast<uintptr_t>(&mark);
        while (g_go == 0)
        {
        }
    }

    void reader(void*)
    {
        while (g_addr == 0)
        {
        }
        volatile uint32_t* const p = reinterpret_cast<volatile uint32_t*>(g_addr);
        while (*p != MARK)
        {
        }
        g_warm = 1;
        while (g_reaped == 0)
        {
            g_sink = *p;
        }
        g_sink = *p;
        kos_print("[unmappeer] UNMAPPEER FAIL: the peer core read the withdrawn page\n");
        kos_exit(1);
    }

    kos::thread::Handle spawn(void (*entry)(void*), char const* name, uint32_t core)
    {
        return kos::thread::create(entry, nullptr, name, PRIO_SPINNER, KOS_POLICY_FIFO, 0,
                                   /*privileged=*/false, nullptr, 0, nullptr, 0, nullptr, 0,
                                   nullptr, 0, 0, nullptr, KOS_TASK_NONE, 1u << core);
    }
}

int main(int, char**)
{
    auto const v = spawn(victim, "victim", CORE_VICTIM);
    auto const r = spawn(reader, "reader", CORE_READER);
    if (not v.valid() or not r.valid())
    {
        kos_print("[unmappeer] ERROR: spawn refused\n");
        return 1;
    }
    while (g_warm == 0)
    {
        kos_sleep_ns(POLL_NS);
    }
    char msg[96];
    ksnprintf(msg, sizeof(msg), "[unmappeer] the peer core reads 0x%lx\n",
              static_cast<unsigned long>(g_addr));
    kos_print(msg);
    g_go = 1;
    if (v.join(KOS_TIMEOUT_NONE) != 0)
    {
        kos_print("[unmappeer] ERROR: the victim never came back\n");
        return 1;
    }
    g_reaped = 1;
    kos_sleep_ns(VERDICT_NS);
    kos_print("[unmappeer] ERROR: the reader neither faulted nor reported\n");
    return 1;
}
