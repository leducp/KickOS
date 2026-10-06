// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The two-thread buffered UART console service as one construct: the IRQ thunk, the block
// initialiser, the descriptor, its static_asserts, and the driver's START.
//
// SPAWN ORDER IS LOAD-BEARING and is fixed here: the IRQ thread first leaves the TX ring
// provably empty when kos_uart_open puts the channel live. That line's FIRST irq_wait
// DISCARDS any pend latched before it, so a byte pushed earlier loses its event and stalls
// until the next doorbell.
//
// The service thread holds the endpoint (WAIT) then the line it rings (SIGNAL), in that
// order, which is the layout <kickos/sys/uart_service.h> reads and desc_ok checks.
//
// NOT FOR A DESCRIPTOR THAT DEPARTS FROM THE SHAPE: two lines, a relay thread, or a retained
// endpoint. Such a driver writes the literal.

#ifndef KICKOS_SYS_UART_CONSOLE_DESC_H
#define KICKOS_SYS_UART_CONSOLE_DESC_H

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/driver/uart.h>
#include <kickos/sys/driver_service.h>
#include <kickos/sys/uart_service.h>

// `svc_name` is a bare token spelling the tag "[svc_name] ", the entry svc_name_console_start
// and the generated <kickos/driver/declared/svc_name.h>, which the including file includes
// first: its kickos_add_driver declaration states the shape, the thread names among it.
// It may NOT be called `name`: the preprocessor would then substitute the designated
// initialiser `.name` below.
//
// THE INVOCATION TAKES A TRAILING SEMICOLON. Without one, tests/static/check_syscall_return_codes.sh
// reads the whole file as one unfinished statement and reports its tail unread.
//
// `params` names a UartParams the caller defines, whose `prime` is not derivable from
// `trigger` (<kickos/sys/uart_service.h> carries that rule). `fallback_baud` 0 asks to keep
// the divisor the boot console left, not for 0 baud.
#define KICKOS_UART_CONSOLE_SERVICE(svc_name, params, fallback_baud, base, trigger)         \
    namespace                                                                               \
    {                                                                                       \
        namespace shape = ::kickos::driver::declared::svc_name;                             \
                                                                                            \
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
        constexpr ::kickos::driver::Descriptor k_desc = {                                   \
            .tag = "[" #svc_name "] ",                                                      \
            .expected_base = base,                                                          \
            .block_size = shape::k_declared.block_size,                                     \
            .block_flags = 0,                                                               \
            .ready_offset = ::kickos::uart::KOS_UART_READY_OFFSET,                          \
            .ep_posture = shape::k_declared.ep_posture,                                     \
            .line_count = shape::k_declared.line_count,                                     \
            .thread_count = shape::k_declared.thread_count,                                 \
            .barrier_after = shape::k_declared.barrier_after,                               \
            .lines = {{trigger}},                                                           \
            .threads = {{.entry = irq_entry,                                                \
                         .name = shape::k_declared.thread_name[0],                          \
                         .prio_delta = shape::k_declared.prio_delta[0],                     \
                         .arg = ::kickos::driver::KOS_DRV_ARG_BLOCK,                        \
                         .window_grant = true,                                              \
                         .cap_count = 2,                                                    \
                         .caps = {{::kickos::driver::KOS_DRV_RES_NOTIFY, KOS_CAP_WAIT, 0},   \
                                  {::kickos::driver::KOS_DRV_RES_LINE0, KOS_CAP_WAIT, 0}}},  \
                        {.entry = ::kickos::uart::console_thread,                           \
                         .name = shape::k_declared.thread_name[1],                          \
                         .prio_delta = shape::k_declared.prio_delta[1],                     \
                         .arg = ::kickos::driver::KOS_DRV_ARG_BLOCK,                        \
                         .window_grant = false,                                             \
                         .cap_count = 2,                                                    \
                         .caps = {{::kickos::driver::KOS_DRV_RES_EP, KOS_CAP_WAIT, 0},      \
                                  {::kickos::driver::KOS_DRV_RES_NOTIFY, KOS_CAP_SIGNAL,     \
                                   ::kickos::driver::doorbell_badge(1)}}}},              \
            .block_init = block_init                                                        \
        };                                                                                  \
                                                                                            \
        static_assert(::kickos::driver::valid(k_desc),                                      \
                      "the " #svc_name " descriptor is not a well-formed driver shape");    \
        static_assert(::kickos::uart::desc_ok(k_desc),                                      \
                      "the " #svc_name " cap positions do not match KOS_UART_CAP_*");       \
        static_assert(::kickos::driver::declared_as(k_desc, shape::k_declared),             \
                      "the " #svc_name " descriptor departs from its declared shape");      \
    }                                                                                       \
                                                                                            \
    extern "C" int svc_name##_console_start(struct kos_driver_instance* instance)           \
    {                                                                                       \
        return ::kickos::driver::bring_up(k_desc, instance);                                \
    }

#endif
