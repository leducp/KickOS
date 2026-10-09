// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A packaged driver holding the two lines its composition binds: its IRQ thread prints the index
// line 0 has among its device's lines, as its spawn hands it, then waits until line 0 is raised
// and says so. Its raiser holds a SIGNAL-only copy of line 0 and raises it until the IRQ thread
// has seen it; its service thread answers each call with whether it has.

#include <kickos/driver/declared/testline.h>
#include <kickos/kos.h>
#include <kickos/sys/atomic.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/errno.h>

namespace drv = kickos::driver;

namespace
{
    constexpr kos_cap_t NOTE = KOS_SPAWN_DELEGATED_CAP0;
    constexpr kos_cap_t EP = KOS_SPAWN_DELEGATED_CAP0;
    constexpr kos_cap_t LINE0 = KOS_SPAWN_DELEGATED_CAP0;
    constexpr uint32_t LINE_WAIT_US = 20000000u;
    constexpr uint64_t RAISE_PACE_NS = 10000000ull;

    kickos::Atomic<uint32_t, kickos::Order::ACQUIRE | kickos::Order::RELEASE> g_raised = {};

    void print_index(uint16_t index)
    {
        char digits[6] = {};
        size_t at = sizeof(digits) - 1u;
        uint32_t value = index;
        do
        {
            at--;
            digits[at] = static_cast<char>('0' + value % 10u);
            value /= 10u;
        } while (value != 0u);
        kos::print("testline: line 0 is index ");
        kos::print(&digits[at]);
        kos::print(" of its device\n");
    }

    void irq(void* arg)
    {
        print_index(drv::line_index_of(arg));
        uint32_t bits = 0;
        if (kos_notify_bind(NOTE) != 0 or kos_notify_wait(NOTE, 1u, LINE_WAIT_US, &bits) != 0)
        {
            kos::print("testline: line 0 was never raised\n");
            drv::trap();
        }
        kos::print("testline: line 0 was raised\n");
        g_raised = 1u;
        while (true)
        {
            (void)kos_notify_wait(NOTE, 1u, KOS_TIMEOUT_NONE, &bits);
        }
    }

    // Answers until it has answered that line 0 was raised. A reply the caller is no longer
    // there to take is not counted.
    void service(void*)
    {
        uint32_t raised = 0;
        while (raised == 0u)
        {
            struct kos_reply_recv_opts opts;
            kos_reply_recv_opts_init(&opts, EP, 0u, KOS_TIMEOUT_NONE);
            opts.info.reply_cap = KOS_CAP_NONE;
            int32_t const n =
                kos_reply_recv(KOS_CAP_NONE, nullptr, kos_call_lens_pack(0u, 0u), &opts);
            if (n < 0)
            {
                drv::trap();
            }
            if (opts.info.reply_cap == KOS_CAP_NONE)
            {
                continue;
            }
            uint32_t const now = g_raised.load();
            int const rc = kos_reply(opts.info.reply_cap, &now, sizeof(now));
            if (rc < 0 and rc != -KOS_ESRCH)
            {
                drv::trap();
            }
            if (rc == 0)
            {
                raised = now;
            }
        }
        kos_exit(0);
    }

    void raiser(void*)
    {
        while (g_raised.load() == 0u)
        {
            if (kos_irq_raise(LINE0) != 0)
            {
                kos::print("testline: raising line 0 was refused\n");
                drv::trap();
            }
            kos_sleep_ns(RAISE_PACE_NS);
        }
        kos_exit(0);
    }

    constexpr drv::Descriptor k_desc = KICKOS_DRIVER_DESCRIPTOR;

    static_assert(drv::valid(k_desc), "the testline descriptor is not a driver shape");
}

extern "C" int testline_start(struct kos_driver_instance* instance)
{
    return drv::bring_up(k_desc, instance);
}
