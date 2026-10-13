// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A failing UART or USB driver thread traps, which ends its task whichever thread failed
// (docs/design-m10-target.md, section 2). The thread bodies of the UART and USB CDC services run
// here over the recording seam; the trap reaches the host as SIGILL.

#include <kickos/sys/driver_service.h>
#include <kickos/sys/uart_service.h>
#include <kickos/sys/usb_cdc_service.h>

#include <kickos/sys/errno.h>

#include "kos_seam.h"

#include <gtest/gtest.h>

#include <signal.h>

namespace drv = kickos::driver;
namespace uart = kickos::uart;
namespace usb = kickos::usb;

namespace
{
    volatile int32_t g_open_rc = -KOS_EIO;
    volatile int g_bind_rc = 0;
    volatile int g_recv_rc = -KOS_EBADF;
    volatile int g_wait_rc = -KOS_EBADF;
    // Receives answered -KOS_EFAULT, a client-buffer fault, before g_recv_rc.
    uint32_t g_recv_faults = 0u;
    uint32_t g_receives = 0u;
}

extern "C"
{
    int kos_reply_recv(kos_cap_t, void*, uintptr_t, struct kos_reply_recv_opts*)
    {
        g_receives++;
        if (g_receives <= g_recv_faults)
        {
            return -KOS_EFAULT;
        }
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
    // Never reached past a failed bind or wait; every method only has to exist.
    struct NullUsb
    {
        int bring_up() { return 0; }
        void attach() {}
        uint32_t take_events() { return 0u; }
        uint32_t take_buff_status() { return 0u; }
        void setup_read(struct kos_usb_setup*) {}
        void bus_reset_recover() {}
        void set_address(uint8_t) {}
        void ep_open_all() {}
        void ep0_in(uint8_t const*, uint32_t, uint8_t) {}
        void ep0_out_arm(uint32_t, uint8_t) {}
        uint32_t ep0_out_read(uint8_t*, uint32_t) { return 0u; }
        void ep0_stall() {}
        void ep_in(uint8_t, uint8_t const*, uint32_t, uint8_t) {}
        void ep_out_arm(uint8_t, uint8_t) {}
        uint32_t ep_out_read(uint8_t, uint8_t*, uint32_t) { return 0u; }
        void ep_stall(uint8_t, bool) {}
    };

    alignas(16) uart::Ctx g_ctx;
    alignas(16) usb::Shared g_usb;

    void usb_irq_thread()
    {
        NullUsb dev;
        usb::Cdc<NullUsb> cdc(dev, &g_usb);
        usb::irq_loop(cdc, &g_usb);
    }

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

    TEST(DriverThread, a_receiver_receives_again_after_a_client_buffer_fault)
    {
        g_receives = 0u;
        g_recv_faults = 2u;
        alignas(16) static uart::Shared sh;
        uart::shared_init(&sh);
        EXPECT_EQ(kickos::console::console_serve_loop<uart::Transport>(&sh), -KOS_EBADF);
        EXPECT_EQ(g_receives, 3u);
        g_recv_faults = 0u;
    }

    TEST(DriverThread, a_usb_receiver_whose_receive_fails_traps)
    {
        EXPECT_EXIT(usb::console_serve_loop(&g_usb), ::testing::KilledBySignal(SIGILL), "");
    }

    TEST(DriverThread, a_usb_irq_thread_whose_wait_fails_traps)
    {
        EXPECT_EXIT(usb_irq_thread(), ::testing::KilledBySignal(SIGILL), "");
    }

    TEST(DriverThread, a_usb_irq_thread_whose_bind_fails_traps)
    {
        g_bind_rc = -KOS_EBUSY;
        EXPECT_EXIT(usb_irq_thread(), ::testing::KilledBySignal(SIGILL), "");
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
