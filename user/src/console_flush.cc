// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sys.h>
#include <kickos/sys/cap_index.h>
#include <kickos/sys/errno.h>
#include <kickos/sys/uart.h>

int32_t kos_console_flush(uint32_t timeout_us)
{
    union
    {
        struct kos_uart_req req;
        struct kos_uart_rsp rsp;
    } msg = {};
    msg.req.op = KOS_UART_DRAIN;
    int32_t const n = kos_call(KOS_CAP_STDOUT, &msg, sizeof(msg.req), sizeof(msg.rsp), timeout_us);
    if (n < 0)
    {
        return n;
    }
    if (static_cast<size_t>(n) < sizeof(msg.rsp))
    {
        return -KOS_EIO;
    }
    return msg.rsp.status;
}
