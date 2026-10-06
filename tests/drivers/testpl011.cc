// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A packaged console driver over a PL011's window, polled, whose one thread serves WRITES plain
// sends per instance and then exits, which ends its task. Each instance marks its start and its
// end on the wire itself. A kos_call is answered -KOS_ENOSYS.

#include <kickos/driver/declared/testpl011.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/uart.h>

#include <stdint.h>

namespace drv = kickos::driver;
namespace declared = kickos::driver::declared::testpl011;

namespace
{
    constexpr uint32_t WRITES = 5u;

    constexpr uintptr_t UART_DR = 0x00u;
    constexpr uintptr_t UART_FR = 0x18u;
    constexpr uint32_t UART_FR_BUSY = 1u << 3;
    constexpr uint32_t UART_FR_TXFF = 1u << 5;
    constexpr uint32_t FLUSH_POLL_MAX = 1000000u;

    void put(uintptr_t base, char c)
    {
        volatile uint32_t* const fr = reinterpret_cast<volatile uint32_t*>(base + UART_FR);
        volatile uint32_t* const dr = reinterpret_cast<volatile uint32_t*>(base + UART_DR);
        while ((*fr & UART_FR_TXFF) != 0u)
        {
        }
        *dr = static_cast<uint8_t>(c);
    }

    // BUSY stays set until the last stop bit has left the shift register.
    void drain(uintptr_t base)
    {
        volatile uint32_t* const fr = reinterpret_cast<volatile uint32_t*>(base + UART_FR);
        for (uint32_t i = 0; i < FLUSH_POLL_MAX; i++)
        {
            if ((*fr & UART_FR_BUSY) == 0u)
            {
                return;
            }
        }
    }

    // The kernel console's line ending on this board, so a reader sees one wire either way.
    void write(uintptr_t base, char const* buf, size_t n)
    {
        for (size_t i = 0; i < n; i++)
        {
            if (buf[i] == '\n')
            {
                put(base, '\r');
            }
            put(base, buf[i]);
        }
    }

    void write_text(uintptr_t base, char const* s)
    {
        size_t n = 0;
        while (s[n] != '\0')
        {
            n++;
        }
        write(base, s, n);
    }

    void service(void*)
    {
        // Where this thread reaches its window, which a translating board chooses.
        struct kos_window window = {};
        if (kos_window_get(0u, &window) != 0)
        {
            drv::trap();
        }
        uintptr_t const base = window.base;
        write_text(base, "testpl011: serving\n");
        union
        {
            char bytes[KOS_EP_MSG_MAX];
            struct kos_uart_rsp rsp;
        } msg;
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, KOS_SPAWN_DELEGATED_CAP0, 0u, KOS_TIMEOUT_NONE);
        kos_cap_t reply_cap = KOS_CAP_NONE;
        size_t reply_len = 0;
        uint32_t served = 0;
        while (served < WRITES)
        {
            // Seated: a zeroed one is stdout's reserved index rather than the empty capability.
            opts.info.reply_cap = KOS_CAP_NONE;
            int32_t const n = kos_reply_recv(reply_cap, msg.bytes, kos_call_lens_pack(reply_len, sizeof(msg)), &opts);
            reply_cap = KOS_CAP_NONE;
            reply_len = 0;
            if (n < 0)
            {
                drv::trap();
            }
            if (opts.info.reply_cap != KOS_CAP_NONE)
            {
                msg.rsp.status = -KOS_ENOSYS;
                msg.rsp.len = 0;
                msg.rsp.rsv = 0;
                reply_len = sizeof(msg.rsp);
                reply_cap = opts.info.reply_cap;
                continue;
            }
            if (n == 0)
            {
                drain(base);
                continue;
            }
            write(base, msg.bytes, static_cast<size_t>(n));
            served++;
        }
        write_text(base, "testpl011: served its writes, exiting\n");
        kos_exit(0);
    }

    constexpr drv::Descriptor k_desc = {
        .tag = "[testpl011] ",
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
                     .arg = drv::KOS_DRV_ARG_NONE,
                     .window_grant = true,
                     .cap_count = 1,
                     .caps = {{drv::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0}}}},
        .block_init = nullptr
    };

    static_assert(drv::valid(k_desc), "the testpl011 descriptor is not a driver shape");
    static_assert(drv::declared_as(k_desc, declared::k_declared),
                  "the testpl011 descriptor departs from its kickos_add_driver declaration");
}

extern "C" int testpl011_start(struct kos_driver_instance* instance)
{
    return drv::bring_up(k_desc, instance);
}
