// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// XMC4800 userspace polled UART TX console driver (console handover, M3 #4 stage
// ii-a). An UNPRIVILEGED thread owns the granted USIC0 CH0 register window and
// serves a console endpoint: it kos_recv()s byte batches from stdout clients and
// POLL-writes each byte to the USIC transmit buffer. It does NOT clock/pin/baud
// the USIC -- the kernel's kickos_xmc_usic_init() already did that at boot, and
// console_tx_deinit() left the channel TX-capable in a polled state. The driver
// only drives TX inside its window; SCU (clock) and P1 IOCR (pin mux) stay OUT of
// the window, privileged.
//
// HARD RULE (design D7): the driver MUST NOT use libc stdio (printf/puts) -- that
// self-sends to the very endpoint it serves and deadlocks. Diagnostics go direct
// to the USIC window or via kos_kconsole_write (RTT / kernel debug path).
//
// Isolation reality: on ARMv7-M PMSA the granted DEV window IS a genuine per-thread
// capability (reprogrammed every switch-in), so another unprivileged thread faults
// on the U0C0 window and SCU/IOCR stay privileged. That is why XMC is the first
// handover target.

#ifndef KICKOS_DRIVER_XMCUART_H
#define KICKOS_DRIVER_XMCUART_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    // The unprivileged driver thread entry. `arg` is the granted USIC0 CH0 window
    // BASE, passed as the thread-arg VALUE (never dereferenced as memory). The
    // delegated recv cap lands at child table index 1. Loops kos_recv() ->
    // poll-write each byte to TBUF0; exits cleanly when kos_recv returns < 0
    // (endpoint dead / EPIPE). Spawned by xmcuart_console_start(), or directly by
    // a consumer that wants its own orchestration.
    void xmcuart_console_driver(void* arg);

    // Privileged one-shot console-handover bring-up (call ONCE from the privileged
    // app main, BEFORE spawning any app that should print through the driver):
    //   1. create a console endpoint E,
    //   2. kos_console_publish(E)  (relinquishes the kernel UART, routes stdout to E),
    //   3. spawn the UNPRIVILEGED driver granted the USIC0 CH0 window + {E | WAIT},
    //   4. close root's own WAIT-bearing cap on E (S4: else driver death cannot EPIPE
    //      and clients hang).
    // `driver_prio` must be >= every client's priority (D9: no PI on rendezvous).
    // Returns 0, or < 0 on any failure (endpoint/publish/spawn). On failure the
    // caller MUST NOT spawn console-dependent apps (S6: publish+spawn are inseparable).
    int xmcuart_console_start(uint8_t driver_prio);

#ifdef __cplusplus
}
#endif

#endif // KICKOS_DRIVER_XMCUART_H
