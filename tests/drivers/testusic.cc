// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A packaged console driver over an XMC4800 USIC channel's window, polled like xmcuart: a plain
// send is console bytes, and a zero-length one a flush answered once the channel has drained. A
// zero-length kos_call asks it to scramble the channel it holds, answered 0 once the clock is
// gated, so nothing reaches the wire after it unless the kernel reclaims the console. Any other
// call is answered -KOS_ENOSYS.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/declared/testusic.h>
#include <kickos/driver/uart.h>
#include <kickos/io/mmio.h> // r32
#include <kickos/sys/driver_service.h>
#include <kickos/sys/errno.h>
#include <kickos/sys/uart.h>

#include <regs/usic.h>

#include <stdint.h>

namespace drv = kickos::driver;
namespace declared = kickos::driver::declared::testusic;
namespace ru = kickos::xmc::reg::usic;

namespace
{
    // A channel that never reports room costs a bounded delay per byte, and the byte is dropped.
    constexpr uint32_t TX_POLL_TIMEOUT = 1000000u;

    void poll_put(struct kos_uart* dev, unsigned char v)
    {
        for (uint32_t i = 0; i < TX_POLL_TIMEOUT; i++)
        {
            if (kos_uart_write(dev, &v, 1u) == 1u)
            {
                return;
            }
        }
    }

    void put_bytes(struct kos_uart* dev, char const* buf, size_t n)
    {
        for (size_t i = 0; i < n; i++)
        {
            poll_put(dev, static_cast<unsigned char>(buf[i]));
        }
    }

    void put_text(struct kos_uart* dev, char const* s)
    {
        for (; *s != '\0'; s++)
        {
            poll_put(dev, static_cast<unsigned char>(*s));
        }
    }

    // FDR, BRG and CCR are write-PV-only on this part and discard an unprivileged store, so only
    // the U,PV-writable registers are wrecked. The clock gate goes LAST: once it lands, every
    // later store to the channel is dropped.
    void scramble(uintptr_t base)
    {
        r32(base + ru::off::SCTR) = 0u;
        r32(base + ru::off::TCSR) = 0u;
        r32(base + ru::off::PCR) = 0u;
        r32(base + ru::off::KSCFG) = ru::KSCFG_BPMODEN;
    }

    void service(void* arg)
    {
        uintptr_t const base = reinterpret_cast<uintptr_t>(arg);

        struct kos_uart_stats stats = {};
        struct kos_uart_config cfg = {};
        cfg.base = base;
        cfg.stats = &stats;
        cfg.baud = 0; // keeps the kernel's divisor
        cfg.data_bits = 8;
        cfg.parity = KOS_UART_PARITY_NONE;
        cfg.stop_bits = 1;
        cfg.rsv = 0;

        struct kos_uart dev;
        if (kos_uart_open(&dev, &cfg) < 0)
        {
            // Not stdio: this thread's stdout is the endpoint it serves, which has no receiver yet.
            kos::print("[testusic] ERROR: channel open refused\n");
            drv::trap();
        }
        put_text(&dev, "[testusic] driver up (polled TX)\n");

        union
        {
            char bytes[KOS_EP_MSG_MAX];
            struct kos_uart_rsp rsp;
        } msg;
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, KOS_SPAWN_DELEGATED_CAP0, 0u, KOS_TIMEOUT_NONE);
        kos_cap_t reply_cap = KOS_CAP_NONE;
        size_t reply_len = 0;
        while (true)
        {
            // Seated: a zeroed one is stdout's reserved index rather than the empty capability.
            opts.info.reply_cap = KOS_CAP_NONE;
            int32_t const n = kos_reply_recv(reply_cap, msg.bytes, kos_call_lens_pack(reply_len, sizeof(msg)), &opts);
            reply_cap = KOS_CAP_NONE;
            reply_len = 0;
            if (n < 0)
            {
                break;
            }
            if (opts.info.reply_cap != KOS_CAP_NONE)
            {
                msg.rsp.status = -KOS_ENOSYS;
                if (n == 0)
                {
                    put_text(&dev, "[testusic] scrambling the channel, its clock gated last\n");
                    (void)kos_uart_flush(&dev);
                    scramble(base);
                    msg.rsp.status = 0;
                }
                msg.rsp.len = 0;
                msg.rsp.rsv = 0;
                reply_len = sizeof(msg.rsp);
                reply_cap = opts.info.reply_cap;
                continue;
            }
            if (n == 0)
            {
                (void)kos_uart_flush(&dev);
                continue;
            }
            put_bytes(&dev, msg.bytes, static_cast<size_t>(n));
        }

        // Close leaves ASC mode, which truncates a frame still shifting.
        (void)kos_uart_flush(&dev);
        (void)kos_uart_close(&dev);
        kos_exit(0);
    }

    constexpr drv::Descriptor k_desc = {
        .tag = "[testusic] ",
        .expected_base = 0,
        .block_size = declared::k_declared.block_size,
        .block_flags = 0,
        .ready_offset = drv::KOS_DRV_READY_NONE,
        .ep_posture = declared::k_declared.ep_posture,
        .line_count = declared::k_declared.line_count,
        .thread_count = declared::k_declared.thread_count,
        .barrier_after = declared::k_declared.barrier_after,
        .lines = {},
        .threads = {{.entry = service,
                     .name = declared::k_declared.thread_name[0],
                     .prio_delta = declared::k_declared.prio_delta[0],
                     .arg = drv::KOS_DRV_ARG_WINDOW,
                     .window_grant = true,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
        .block_init = nullptr
    };

    static_assert(drv::valid(k_desc), "the testusic descriptor is not a driver shape");
    static_assert(drv::declared_as(k_desc, declared::k_declared),
                  "the testusic descriptor departs from its kickos_add_driver declaration");
}

extern "C" int testusic_start(struct kos_driver_instance* instance)
{
    return drv::bring_up(k_desc, instance);
}
