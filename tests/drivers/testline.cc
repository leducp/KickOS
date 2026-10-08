// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A packaged driver holding the two lines its composition binds: its IRQ thread prints the index
// line 0 has among its device's lines, as its spawn hands it, then waits until line 0 is raised
// and says so; its service thread answers calls.

#include <kickos/driver/declared/testline.h>
#include <kickos/kos.h>
#include <kickos/sys/driver_service.h>

#include "serve.h"

namespace drv = kickos::driver;

namespace
{
    constexpr kos_cap_t NOTE = KOS_SPAWN_DELEGATED_CAP0;
    constexpr kos_cap_t EP = KOS_SPAWN_DELEGATED_CAP0;
    constexpr uint32_t LINE_WAIT_US = 20000000u;

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
        while (true)
        {
            (void)kos_notify_wait(NOTE, 1u, KOS_TIMEOUT_NONE, &bits);
        }
    }

    void service(void*)
    {
        if (testdrivers::serve(EP) < 0)
        {
            drv::trap();
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
