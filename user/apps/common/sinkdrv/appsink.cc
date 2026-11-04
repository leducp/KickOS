// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A packaged console driver over no device, which an app creates on request: it takes what
// stdout sends and writes nothing. It counts the bytes of plain sends, answers a one-byte call
// `Q` with the count and a drain with success, and ends its task when its endpoint dies.

#include <kickos/board_config.h> // KICKOS_USER_STACK_SIZE, which the stack bound reads
#include <kickos/driver/declared/appsink.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/uart.h>

#include <stdint.h>

namespace drv = kickos::driver;

namespace
{
    void service(void*)
    {
        union
        {
            char bytes[KOS_EP_MSG_MAX];
            struct kos_uart_rsp rsp;
            int32_t count;
        } msg;
        struct kos_reply_recv_opts opts;
        kos_reply_recv_opts_init(&opts, KOS_SPAWN_DELEGATED_CAP0, 0u, KOS_TIMEOUT_NONE);
        kos_cap_t reply_cap = KOS_CAP_NONE;
        size_t reply_len = 0;
        int32_t total = 0;
        while (true)
        {
            // Seated: a zeroed one is stdout's reserved index rather than the empty capability.
            opts.info.reply_cap = KOS_CAP_NONE;
            int32_t const n = kos_reply_recv(reply_cap, msg.bytes,
                                             kos_call_lens_pack(reply_len, sizeof(msg)), &opts);
            reply_cap = KOS_CAP_NONE;
            reply_len = 0;
            if (n < 0)
            {
                break;
            }
            if (opts.info.reply_cap != KOS_CAP_NONE)
            {
                reply_cap = opts.info.reply_cap;
                if (n == 1 and msg.bytes[0] == 'Q')
                {
                    msg.count = total;
                    reply_len = sizeof(msg.count);
                    continue;
                }
                msg.rsp.status = 0;
                msg.rsp.len = 0;
                msg.rsp.rsv = 0;
                reply_len = sizeof(msg.rsp);
                continue;
            }
            total = total + n;
        }
        kos_exit(0);
    }

    constexpr drv::Descriptor k_desc = KICKOS_DRIVER_DESCRIPTOR;

    static_assert(drv::valid(k_desc), "the appsink descriptor is not a driver shape");
}

extern "C" int appsink_start(struct kos_driver_instance* instance)
{
    return drv::bring_up(k_desc, instance);
}
