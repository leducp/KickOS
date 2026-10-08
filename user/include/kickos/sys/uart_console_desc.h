// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The two-thread buffered UART console service as one construct: the IRQ thunk, the block
// initialiser, the descriptor its declaration generates, its static_asserts, and the driver's
// START.
//
// SPAWN ORDER IS LOAD-BEARING: the IRQ thread first leaves the TX ring provably empty when
// kos_uart_open puts the channel live. That line's FIRST irq_wait DISCARDS any pend latched
// before it, so a byte pushed earlier loses its event and stalls until the next doorbell.
//
// The service thread holds the endpoint (WAIT) then the line it rings (SIGNAL), in that
// order, which is the layout <kickos/sys/uart_service.h> reads and desc_ok checks.

#ifndef KICKOS_SYS_UART_CONSOLE_DESC_H
#define KICKOS_SYS_UART_CONSOLE_DESC_H

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/uart.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/uart_service.h>

// The including file includes its generated <kickos/driver/declared/svc_name.h> first, whose
// THREADs name irq_entry and uart::console_thread, INIT block_init, and START
// svc_name_console_start.
//
// THE INVOCATION TAKES A TRAILING SEMICOLON. Without one, tests/static/check_syscall_return_codes.sh
// reads the whole file as one unfinished statement and reports its tail unread.
//
// `params` names a UartParams the caller defines, whose `prime` is not derivable from the
// line's trigger (<kickos/sys/uart_service.h> carries that rule). `fallback_baud` 0 asks to
// keep the divisor the boot console left, not for 0 baud.
#define KICKOS_UART_CONSOLE_SERVICE(svc_name, params, fallback_baud)                       \
    namespace                                                                               \
    {                                                                                       \
        void irq_entry(void* arg)                                                           \
        {                                                                                   \
            ::kickos::uart::irq_thread<struct kos_uart>(                                    \
                static_cast< ::kickos::uart::Ctx*>(arg), params);                           \
        }                                                                                   \
                                                                                            \
        int block_init(void* blk, struct kos_driver_instance const* in)                     \
        {                                                                                   \
            return ::kickos::uart::ctx_init(static_cast< ::kickos::uart::Ctx*>(blk), in,    \
                                            fallback_baud);                                 \
        }                                                                                   \
                                                                                            \
        constexpr ::kickos::driver::Descriptor k_desc = KICKOS_DRIVER_DESCRIPTOR;           \
                                                                                            \
        static_assert(::kickos::driver::valid(k_desc),                                      \
                      "the " #svc_name " descriptor is not a well-formed driver shape");    \
        static_assert(::kickos::uart::desc_ok(k_desc),                                      \
                      "the " #svc_name " cap positions do not match KOS_UART_CAP_*");       \
    }                                                                                       \
                                                                                            \
    extern "C" int svc_name##_console_start(struct kos_driver_instance* instance)           \
    {                                                                                       \
        return ::kickos::driver::bring_up(k_desc, instance);                                \
    }

#endif
