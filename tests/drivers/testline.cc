// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A packaged driver holding the two lines its composition binds: its IRQ thread prints the index
// line 0 has among its device's lines, as its spawn hands it, then waits until line 0 is raised
// and says so; its service thread answers calls. Its descriptor numbers both lines as none a claim
// takes and states another index, so a start claiming the descriptor's numbers fails and a thread
// handed the descriptor's index prints that one.

#include <kickos/driver/declared/testline.h>
#include <kickos/kos.h>
#include <kickos/sys/driver_service.h>

#include "serve.h"

namespace drv = kickos::driver;
namespace declared = kickos::driver::declared::testline;

namespace
{
    // Past every interrupt controller's lines.
    constexpr int NO_LINE = 0x7FFF;
    constexpr uint16_t DESCRIPTOR_INDEX = 0u;

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
        print_index(drv::line_index_of(drv::thread_start(arg)));
        uint32_t bits = 0;
        if (kos_notify_bind(NOTE) != 0 or kos_notify_wait(NOTE, 1u, LINE_WAIT_US, &bits) != 0)
        {
            kos::print("testline: line 0 was never raised\n");
            drv::trap_under_init();
            kos_exit(1);
        }
        kos::print("testline: line 0 was raised\n");
        while (true)
        {
            (void)kos_notify_wait(NOTE, 1u, KOS_TIMEOUT_NONE, &bits);
        }
    }

    void service(void* arg)
    {
        (void)drv::thread_start(arg); // records the posture; the thread takes no arg
        if (testdrivers::serve(EP) < 0)
        {
            drv::trap_under_init();
        }
        kos_exit(0);
    }

    constexpr drv::Descriptor k_desc = {
        .tag = "[testline] ",
        .expected_base = 0,
        .block_size = declared::k_declared.block_size,
        .block_flags = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = declared::k_declared.ep_posture,
        .svc_kind = KOS_SVC_SPI, // no kind is neutral: an instance reads only whether it is KOS_SVC_CONSOLE
        .line_count = declared::k_declared.line_count,
        .thread_count = declared::k_declared.thread_count,
        .barrier_after = declared::k_declared.barrier_after,
        .lines = {{NO_LINE, KOS_IRQ_EDGE, DESCRIPTOR_INDEX}, {NO_LINE, KOS_IRQ_EDGE}},
        .threads = {{.entry = irq,
                     .name = declared::k_declared.thread_name[0],
                     .prio_delta = declared::k_declared.prio_delta[0],
                     .arg = drv::KOS_DRV_ARG_LINE0_INDEX,
                     .window_grant = false,
                     .cap_count = 3,
                     .caps = {{drv::KOS_DRV_RES_NOTIFY, KOS_CAP_WAIT, 0},
                              {drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT, 0},
                              {drv::KOS_DRV_RES_LINE1, KOS_CAP_WAIT, 0}}},
                    {.entry = service,
                     .name = declared::k_declared.thread_name[1],
                     .prio_delta = declared::k_declared.prio_delta[1],
                     .arg = drv::KOS_DRV_ARG_NONE,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
        .block_init = nullptr
    };

    static_assert(drv::valid(k_desc), "the testline descriptor is not a driver shape");
    static_assert(drv::declared_as(k_desc, declared::k_declared),
                  "the testline descriptor departs from its kickos_add_driver declaration");
}

extern "C" int testline_start(struct kos_service_cfg const* cfg)
{
    return drv::bring_up(k_desc, cfg, nullptr);
}
