// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A failing UART driver thread traps, which ends its task whichever thread failed
// (docs/design-m10-target.md, section 2). The thread bodies of <kickos/sys/uart_service.h> and
// user/src/uart_service.cc run here over the recording seam; the trap reaches the host as SIGILL.

#include <kickos/sys/driver_service.h>
#include <kickos/sys/uart_service.h>

#include <kickos/sys/errno.h>

#include "kos_seam.h"

#include <gtest/gtest.h>

#include <signal.h>

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
}

namespace kickos::uart
{
    void irq_pass(struct kos_uart*, Shared*)
    {
    }

    void dev_shutdown(struct kos_uart*)
    {
    }

    int32_t dev_flush(struct kos_uart*)
    {
        return 0;
    }

    void win_puts(struct kos_uart*, char const*)
    {
    }
}

namespace
{
    alignas(16) uart::Ctx g_ctx;

    uart::UartParams const k_params = {.announce = "", .prime = false};

    TEST(DriverThread, a_receiver_whose_receive_fails_traps)
    {
        EXPECT_EXIT(uart::console_thread(&g_ctx), ::testing::KilledBySignal(SIGILL), "");
    }

    TEST(DriverThread, an_irq_thread_whose_device_refuses_its_open_traps)
    {
        EXPECT_EXIT(uart::irq_thread<struct kos_uart>(&g_ctx, k_params), ::testing::KilledBySignal(SIGILL), "");
    }

    TEST(DriverThread, an_irq_thread_whose_wait_fails_traps)
    {
        g_open_rc = 0;
        EXPECT_EXIT(uart::irq_thread<struct kos_uart>(&g_ctx, k_params), ::testing::KilledBySignal(SIGILL), "");
        g_open_rc = -KOS_EIO;
    }

    TEST(DriverThread, an_irq_thread_whose_bind_fails_traps)
    {
        g_open_rc = 0;
        g_bind_rc = -KOS_EBUSY;
        EXPECT_EXIT(uart::irq_thread<struct kos_uart>(&g_ctx, k_params), ::testing::KilledBySignal(SIGILL), "");
        g_open_rc = -KOS_EIO;
        g_bind_rc = 0;
    }

    TEST(UartContext, the_instance_s_window_and_line_0_index_reach_the_class_config)
    {
        struct kos_driver_instance in = {};
        in.mmio_base = 0x40000000u;
        in.line_count = 1;
        in.lines[0] = {40u, 3u};
        uart::Ctx ctx;
        EXPECT_EQ(uart::ctx_init(&ctx, &in, 0u), 0);
        EXPECT_EQ(ctx.ucfg.base, 0x40000000u);
        EXPECT_EQ(ctx.ucfg.line_index, 3u);

        in.line_count = 0;
        EXPECT_EQ(uart::ctx_init(&ctx, &in, 0u), 0);
        EXPECT_EQ(ctx.ucfg.line_index, 0u) << "an instance with no line routes onto index 0";
    }
}
