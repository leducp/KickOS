// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A packaged driver whose one thread holds the line its composition binds: it answers a call,
// waits until that line is raised, then answers another. Its descriptor numbers the line as one
// no claim takes, so a start that claimed the descriptor's number would fail.

#include <kickos/driver/declared/testline.h>
#include <kickos/kos.h>
#include <kickos/sys/driver_service.h>

#include <kickos/sys/errno.h>

namespace drv = kickos::driver;
namespace declared = kickos::driver::declared::testline;

namespace
{
    // Past every interrupt controller's lines.
    constexpr int NO_LINE = 0x7FFF;

    constexpr kos_cap_t EP = KOS_SPAWN_DELEGATED_CAP0;
    constexpr kos_cap_t NOTE = KOS_SPAWN_DELEGATED_CAP0 + 1;
    constexpr uint32_t LINE_WAIT_US = 20000000u;

    // Answers one call with `value`: 0, or the failing call's answer. A reply the caller is no
    // longer there to take is not an answer.
    int answer(uint32_t value)
    {
        while (true)
        {
            struct kos_reply_recv_opts opts;
            kos_reply_recv_opts_init(&opts, EP, 0u, KOS_TIMEOUT_NONE);
            opts.info.reply_cap = KOS_CAP_NONE;
            int32_t const n = kos_reply_recv(KOS_CAP_NONE, nullptr, kos_call_lens_pack(0u, 0u), &opts);
            if (n < 0)
            {
                return n;
            }
            if (opts.info.reply_cap == KOS_CAP_NONE)
            {
                continue;
            }
            int const rc = kos_reply(opts.info.reply_cap, &value, sizeof(value));
            if (rc != -KOS_ESRCH)
            {
                return rc;
            }
        }
    }

    // Answers 1, waits for its line, then answers 2: the first wait arms the line.
    void service(void* arg)
    {
        (void)drv::thread_start(arg); // records the posture; the thread takes no arg
        if (answer(1u) < 0)
        {
            drv::trap_under_init();
            kos_exit(1);
        }
        uint32_t bits = 0;
        if (kos_notify_bind(NOTE) != 0 or kos_notify_wait(NOTE, 1u, LINE_WAIT_US, &bits) != 0)
        {
            kos::print("testline: the line was never raised\n");
            drv::trap_under_init();
            kos_exit(1);
        }
        kos::print("testline: its line was raised\n");
        if (answer(2u) < 0)
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
        .lines = {{NO_LINE, KOS_IRQ_EDGE}},
        .threads = {{.entry = service,
                     .name = declared::k_declared.thread_name[0],
                     .prio_delta = declared::k_declared.prio_delta[0],
                     .arg = drv::KOS_DRV_ARG_NONE,
                     .window_grant = false,
                     .cap_count = 3,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0},
                              {drv::KOS_DRV_RES_NOTIFY, KOS_CAP_WAIT, 0},
                              {drv::KOS_DRV_RES_LINE0, KOS_CAP_WAIT, 0}}}},
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
