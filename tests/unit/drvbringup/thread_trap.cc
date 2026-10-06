// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A failing UART driver thread by posture (docs/design-m10-target.md, section 2): given an
// instance it traps, which ends its task whichever thread failed; on a service list its IRQ
// thread panics on a refused open, returns on a refused bind and exits on a failed wait, and its
// receiver exits, as before. The thread bodies of <kickos/sys/uart_service.h> and
// user/src/uart_service.cc run here over the recording seam; the trap reaches the host as SIGILL.

#include <kickos/sys/driver_service.h>
#include <kickos/sys/uart_service.h>

#include <kickos/sys/errno.h>

#include "kos_seam.h"

#include <gtest/gtest.h>

#include <signal.h>
#include <stdio.h>
#include <unistd.h>

namespace drv = kickos::driver;
namespace uart = kickos::uart;

namespace
{
    volatile int32_t g_open_rc = -KOS_EIO;
    volatile int g_bind_rc = 0;
    volatile int g_recv_rc = -KOS_EBADF;
    volatile int g_wait_rc = -KOS_EBADF;
}

extern "C"
{
    // The endpoint gone: the receiver's loop returns.
    int kos_reply_recv(kos_cap_t, void*, uintptr_t, struct kos_reply_recv_opts*)
    {
        return g_recv_rc;
    }

    // The doorbell a write rings; nothing writes here.
    int kos_notify(kos_cap_t)
    {
        return 0;
    }

    int32_t kos_uart_open(struct kos_uart*, struct kos_uart_config const*)
    {
        return g_open_rc;
    }

    int kos_notify_bind(kos_cap_t)
    {
        return g_bind_rc;
    }

    // The notification gone: the IRQ thread's loop breaks.
    int kos_notify_wait(kos_cap_t, uint32_t, uint32_t, uint32_t*)
    {
        return g_wait_rc;
    }

    void kos_panic(char const* msg)
    {
        fprintf(stderr, "PANIC %s\n", msg);
        _exit(42);
    }
}

namespace kickos::uart
{
    void irq_pass(struct kos_uart*, Shared*)
    {
    }

    void dev_shutdown(struct kos_uart*)
    {
    }

    void win_puts(struct kos_uart*, char const*)
    {
    }
}

namespace
{
    void t_entry(void*)
    {
    }

    // The smallest descriptor a bring-up completes, to record a posture.
    constexpr drv::Descriptor k_one = {
        .tag = "[drvtrap] ",
        .expected_base = 0,
        .block_size = 0,
        .block_flags = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = drv::KOS_DRV_EP_RETAIN,
        .svc_kind = KOS_SVC_SPI,
        .line_count = 0,
        .thread_count = 1,
        .barrier_after = 1,
        .lines = {},
        .threads = {{.entry = t_entry,
                     .name = nullptr,
                     .prio_delta = 0,
                     .arg = drv::KOS_DRV_ARG_NONE,
                     .window_grant = false,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
        .block_init = nullptr
    };

    alignas(16) uart::Ctx g_ctx;

    // What a thread body is handed: g_ctx, with whatever bit bring_up's spawn carried.
    void* g_arg = nullptr;

    // Brings k_one up under `under_init`, then records the posture as the spawned thread's entry
    // does, through thread_start over the arg its spawn carried.
    void posture(bool under_init)
    {
        kos_seam_reset();
        struct kos_service_cfg cfg = {};
        cfg.name = "drvtrap";
        cfg.prio = 8u;
        cfg.kind = KOS_SVC_SPI;
        struct kos_driver_instance in = {};
        in.endpoint = 7u;
        in.watch = 8u;
        kos_cap_t ep = KOS_CAP_NONE;
        if (under_init)
        {
            cfg.instance = &in;
        }
        ASSERT_EQ(drv::bring_up(k_one, &cfg, &ep), 0);
        // k_one's thread declares no arg, so what its spawn carried is the posture alone.
        uintptr_t const spawned = reinterpret_cast<uintptr_t>(kos_seam_spawn_arg(0));
        g_arg = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&g_ctx) | spawned);
        (void)drv::thread_start(g_arg);
    }

    uart::UartParams const k_params = {.open_fail = "[drvtrap] open refused", .announce = "", .prime = false};

    TEST(DriverThread, a_spawn_under_the_init_carries_the_posture_its_thread_records)
    {
        posture(true);
        EXPECT_EQ(kos_seam_spawn_arg(0), reinterpret_cast<void*>(1));
        EXPECT_EQ(drv::thread_start(g_arg), static_cast<void*>(&g_ctx));
        EXPECT_EXIT(drv::trap_under_init(), ::testing::KilledBySignal(SIGILL), "");
        posture(false);
        EXPECT_EQ(kos_seam_spawn_arg(0), nullptr);
        EXPECT_EQ(drv::thread_start(g_arg), static_cast<void*>(&g_ctx));
        drv::trap_under_init();
    }

    TEST(DriverThread, a_receiver_whose_receive_fails_traps_under_the_init)
    {
        posture(true);
        EXPECT_EXIT(uart::console_thread(g_arg), ::testing::KilledBySignal(SIGILL), "");
    }

    TEST(DriverThread, a_receiver_whose_receive_fails_exits_on_a_service_list)
    {
        posture(false);
        EXPECT_EXIT(uart::console_thread(g_arg), ::testing::ExitedWithCode(0), "");
    }

    TEST(DriverThread, an_irq_thread_whose_device_refuses_its_open_traps_under_the_init)
    {
        posture(true);
        EXPECT_EXIT(uart::irq_thread<struct kos_uart>(&g_ctx, k_params), ::testing::KilledBySignal(SIGILL), "");
    }

    TEST(DriverThread, an_irq_thread_whose_wait_fails_traps_under_the_init_and_exits_on_a_service_list)
    {
        g_open_rc = 0;
        posture(true);
        EXPECT_EXIT(uart::irq_thread<struct kos_uart>(&g_ctx, k_params), ::testing::KilledBySignal(SIGILL), "");
        posture(false);
        EXPECT_EXIT(uart::irq_thread<struct kos_uart>(&g_ctx, k_params), ::testing::ExitedWithCode(0), "");
        g_open_rc = -KOS_EIO;
    }

    TEST(DriverThread, an_irq_thread_whose_bind_fails_traps_under_the_init_and_returns_on_a_service_list)
    {
        g_open_rc = 0;
        g_bind_rc = -KOS_EBUSY;
        posture(true);
        EXPECT_EXIT(uart::irq_thread<struct kos_uart>(&g_ctx, k_params), ::testing::KilledBySignal(SIGILL), "");
        posture(false);
        EXPECT_EXIT(
            {
                uart::irq_thread<struct kos_uart>(&g_ctx, k_params);
                _exit(7);
            },
            ::testing::ExitedWithCode(7), "");
        g_open_rc = -KOS_EIO;
        g_bind_rc = 0;
    }

    TEST(DriverThread, an_irq_thread_whose_device_refuses_its_open_panics_on_a_service_list)
    {
        posture(false);
        EXPECT_EXIT(uart::irq_thread<struct kos_uart>(&g_ctx, k_params), ::testing::ExitedWithCode(42),
                    "PANIC \\[drvtrap\\] open refused");
    }

    TEST(UartContext, the_instance_s_line_0_index_reaches_the_class_config)
    {
        struct kos_driver_instance in = {};
        in.line_count = 1;
        in.lines[0] = {40u, 3u};
        struct kos_service_cfg cfg = {};
        cfg.mmio_base = 0x40000000u;
        cfg.instance = &in;
        uart::Ctx ctx;
        EXPECT_EQ(uart::ctx_init(&ctx, &cfg, 0u), 0);
        EXPECT_EQ(ctx.ucfg.line_index, 3u);

        cfg.instance = nullptr;
        EXPECT_EQ(uart::ctx_init(&ctx, &cfg, 0u), 0);
        EXPECT_EQ(ctx.ucfg.line_index, 0u) << "a service list routes onto index 0";
    }
}
