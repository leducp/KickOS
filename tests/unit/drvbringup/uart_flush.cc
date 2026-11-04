// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KOS_UART_DRAIN on a buffered UART console (docs/reference/console.md): the service thread
// answers only once the ring AND the device's transmit path have drained, or its bound expired,
// and the reply carries which. The service loop of <kickos/sys/console_service.h> and the IRQ
// loop of <kickos/sys/uart_service.h> run here on two host threads over a fake device whose FIFO
// empties only through kos_uart_flush, so a byte still in the FIFO when the reply goes is a drain
// that did not wait.

#include <kickos/sys/driver_service.h>
#include <kickos/sys/uart_service.h>

#include <kickos/sys/errno.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include <string.h>

namespace uart = kickos::uart;

namespace
{
    constexpr uint32_t FIFO_DEPTH = 64u;

    // Never destroyed: a parked thread still waits on them at exit, and destroying a condition
    // variable with a waiter blocks.
    std::mutex& g_lock = *new std::mutex;
    std::condition_variable& g_raised = *new std::condition_variable;
    bool g_pending = false;
    // Bumped at each arm's end: an IRQ thread of an earlier generation parks for good.
    uint32_t g_generation = 0u;
    thread_local uint32_t t_generation = 0u;

    // The device, under g_lock.
    uint8_t g_fifo[FIFO_DEPTH];
    uint32_t g_fifo_used = 0u;
    std::string g_wire;
    int32_t g_flush_rc = 0;
    std::atomic<uint32_t> g_dev_flushes{0u};

    std::atomic<uint32_t> g_sleeps{0u};
    std::atomic<bool> g_real_sleep{true};

    // The service thread's script: what each receive delivers, a plain send or a KOS_UART_DRAIN
    // call, and what the device held when the receive after the last message was reached.
    constexpr kos_cap_t REPLY_CAP = 40;
    char const* const DRAIN = "drain";
    char const* g_script[4] = {};
    uint32_t g_script_len = 0u;
    uint32_t g_receives = 0u;
    uint32_t g_fifo_at_return = 0u;
    std::string g_wire_at_return;
    // The status of the reply the service sent with its last receive, and the FIFO then.
    bool g_replied = false;
    int32_t g_reply_status = 1;
    uint32_t g_fifo_at_reply = 0u;

    void arm_reset()
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_pending = false;
        g_fifo_used = 0u;
        g_wire.clear();
        g_flush_rc = 0;
        g_dev_flushes = 0u;
        g_sleeps = 0u;
        g_real_sleep = true;
        g_script_len = 0u;
        g_receives = 0u;
        g_fifo_at_return = 0u;
        g_wire_at_return.clear();
        g_replied = false;
        g_reply_status = 1;
        g_fifo_at_reply = 0u;
    }

    void retire_irq_thread()
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_generation++;
        g_raised.notify_all();
    }

    void irq_thread_body(uart::Shared* sh, uint32_t generation)
    {
        t_generation = generation;
        struct kos_uart dev = {};
        uart::irq_loop(dev, sh);
    }

    std::thread start_irq_thread(uart::Shared* sh)
    {
        uint32_t generation = 0u;
        {
            std::lock_guard<std::mutex> lk(g_lock);
            generation = g_generation;
        }
        return std::thread(irq_thread_body, sh, generation);
    }
}

extern "C"
{
    int kos_reply_recv(kos_cap_t reply_cap, void* buf, uintptr_t, struct kos_reply_recv_opts* opts)
    {
        if (reply_cap == REPLY_CAP)
        {
            struct kos_uart_rsp rsp;
            memcpy(&rsp, buf, sizeof(rsp));
            std::lock_guard<std::mutex> lk(g_lock);
            g_replied = true;
            g_reply_status = rsp.status;
            g_fifo_at_reply = g_fifo_used;
        }
        uint32_t const i = g_receives;
        g_receives++;
        if (i < g_script_len and g_script[i] == DRAIN)
        {
            struct kos_uart_req req = {};
            req.op = KOS_UART_DRAIN;
            memcpy(buf, &req, sizeof(req));
            opts->info.reply_cap = REPLY_CAP;
            return static_cast<int>(sizeof(req));
        }
        if (i < g_script_len)
        {
            size_t const n = strlen(g_script[i]);
            memcpy(buf, g_script[i], n);
            return static_cast<int>(n);
        }
        std::lock_guard<std::mutex> lk(g_lock);
        g_fifo_at_return = g_fifo_used;
        g_wire_at_return = g_wire;
        return -KOS_EBADF;
    }

    // Latched like the kernel's: a raise with nobody waiting is delivered to the next wait.
    int kos_notify(kos_cap_t)
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_pending = true;
        g_raised.notify_all();
        return 0;
    }

    // The readiness latch's wake: no thread here waits on it.
    int kos_word_wake(uint32_t*, uint32_t, uint32_t)
    {
        return 0;
    }

    int kos_notify_bind(kos_cap_t)
    {
        return 0;
    }

    int kos_notify_wait(kos_cap_t, uint32_t, uint32_t, uint32_t*)
    {
        std::unique_lock<std::mutex> lk(g_lock);
        g_raised.wait(lk, [] { return g_pending or t_generation != g_generation; });
        while (t_generation != g_generation)
        {
            // An earlier arm's thread: returning would let irq_loop exit the process.
            g_raised.wait(lk);
        }
        g_pending = false;
        return 0;
    }

    void kos_sleep_ns(uint64_t ns)
    {
        g_sleeps.store(g_sleeps.load() + 1u);
        if (g_real_sleep)
        {
            std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
        }
    }

    uint32_t kos_uart_read(struct kos_uart*, unsigned char*, uint32_t)
    {
        return 0u;
    }

    uint32_t kos_uart_write(struct kos_uart*, unsigned char const* src, uint32_t n)
    {
        std::lock_guard<std::mutex> lk(g_lock);
        uint32_t took = 0u;
        while (took < n and g_fifo_used < FIFO_DEPTH)
        {
            g_fifo[g_fifo_used] = src[took];
            g_fifo_used++;
            took++;
        }
        return took;
    }

    int32_t kos_uart_flush(struct kos_uart*)
    {
        g_dev_flushes.store(g_dev_flushes.load() + 1u);
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_flush_rc != 0)
        {
            return g_flush_rc;
        }
        g_wire.append(reinterpret_cast<char const*>(g_fifo), g_fifo_used);
        g_fifo_used = 0u;
        return 0;
    }

    int32_t kos_uart_close(struct kos_uart*)
    {
        return 0;
    }
}

namespace kickos::driver
{
    void trap()
    {
        abort();
    }
}

namespace
{
    alignas(16) uart::Shared g_sh;

    TEST(UartFlush, a_drain_is_answered_only_once_the_device_drained)
    {
        arm_reset();
        uart::shared_init(&g_sh);
        std::thread irq = start_irq_thread(&g_sh);
        g_script[0] = "hello\n";
        g_script[1] = DRAIN;
        g_script_len = 2u;
        EXPECT_EQ(kickos::console::console_serve_loop<uart::Transport>(&g_sh), -KOS_EBADF);
        EXPECT_TRUE(g_replied);
        EXPECT_EQ(g_reply_status, 0);
        EXPECT_EQ(g_fifo_at_reply, 0u) << "the reply went with bytes still in the device's FIFO";
        EXPECT_EQ(g_wire_at_return.find("hello"), 0u) << "wire: " << g_wire_at_return;
        retire_irq_thread();
        irq.detach();
    }

    TEST(UartFlush, a_drain_whose_bound_expires_is_answered_as_a_failure)
    {
        arm_reset();
        uart::shared_init(&g_sh);
        g_real_sleep = false;
        g_script[0] = "hello\n";
        g_script[1] = DRAIN;
        g_script_len = 2u;
        // No IRQ thread: the ring never empties, so the drain's bound expires.
        EXPECT_EQ(kickos::console::console_serve_loop<uart::Transport>(&g_sh), -KOS_EBADF);
        EXPECT_TRUE(g_replied);
        EXPECT_EQ(g_reply_status, -KOS_EBUSY);
    }

    TEST(UartFlush, a_zero_length_send_is_taken_and_carries_nothing)
    {
        arm_reset();
        uart::shared_init(&g_sh);
        g_script[0] = "";
        g_script_len = 1u;
        EXPECT_EQ(kickos::console::console_serve_loop<uart::Transport>(&g_sh), -KOS_EBADF);
        EXPECT_FALSE(g_replied);
        EXPECT_EQ(kos_byte_ring_used(&g_sh.tx), 0u);
        EXPECT_EQ(g_sleeps, 0u) << "a probe started a drain";
    }

    TEST(UartFlush, a_request_raised_before_the_irq_thread_first_waits_is_answered)
    {
        arm_reset();
        uart::shared_init(&g_sh);
        g_sh.flush_req = 1u;
        (void)kos_notify(uart::KOS_UART_CAP_DOORBELL);
        std::thread irq = start_irq_thread(&g_sh);
        for (uint32_t i = 0; i < 2000u and g_sh.flush_ack != 1u; i++)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        EXPECT_EQ(g_sh.flush_ack, 1u);
        EXPECT_EQ(g_dev_flushes, 1u);
        retire_irq_thread();
        irq.detach();
    }

    TEST(UartFlush, a_dead_irq_thread_bounds_the_flush_and_reports_it_undrained)
    {
        arm_reset();
        uart::shared_init(&g_sh);
        g_real_sleep = false;
        EXPECT_EQ(uart::flush(&g_sh), -KOS_EBUSY);
        EXPECT_EQ(g_sleeps, kickos::console::KOS_CONSOLE_FLUSH_MAX);
    }

    TEST(UartFlush, a_device_whose_flush_fails_is_reported_undrained_and_tried_once)
    {
        arm_reset();
        uart::shared_init(&g_sh);
        g_flush_rc = -KOS_EBUSY;
        g_real_sleep = false;
        std::thread irq = start_irq_thread(&g_sh);
        EXPECT_EQ(uart::flush(&g_sh), -KOS_EBUSY);
        for (uint32_t i = 0; i < 2000u and g_dev_flushes == 0u; i++)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        (void)kos_notify(uart::KOS_UART_CAP_DOORBELL);
        (void)kos_notify(uart::KOS_UART_CAP_DOORBELL);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT_EQ(g_dev_flushes, 1u) << "a failed request is retried on every wake";
        EXPECT_NE(g_sh.flush_ack, g_sh.flush_req);
        retire_irq_thread();
        irq.detach();
    }
}
