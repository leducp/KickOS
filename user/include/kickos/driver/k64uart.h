// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// K64F/UART0 userspace polled UART TX console driver (console handover). An
// UNPRIVILEGED thread owns the granted UART0 register window and serves a console
// endpoint: it kos_recv()s byte batches from stdout clients and POLL-writes each
// byte to the UART0 data register. It does NOT clock/pin/baud the UART: the
// kernel's uart0_init() already did that at boot, and console_tx_deinit() left it
// TX-capable in a polled state. The driver only drives TX inside its window; SIM
// (clock gates) and PORTB (pin mux) stay OUT of the window, privileged.
//
// HARD RULE (design D7): the driver MUST NOT use libc stdio (printf/puts). That
// self-sends to the very endpoint it serves and deadlocks. Diagnostics go direct
// to the UART0 window or via kos_kconsole_write (RTT / kernel debug path).
//
// Isolation reality (honest, coarse-AIPS): unlike the XMC PMSA case, the K64F
// per-thread SYSMPU grant over the UART0 window is INERT for the peripheral. AIPS
// peripheral bridges are NOT SYSMPU slave ports (RM 3.3.6.2), so peripheral MMIO is
// not per-thread MPU-gated; the real enabler is the AIPS PACR open the driver's own
// kos_uart_open asks for through kos_periph_enable, which makes UART0 reachable by
// EVERY unprivileged thread. Holding the window is the sole authorisation for that
// call. Handover here is FUNCTIONAL + RECLAIM-PROOF, not per-thread peripheral
// isolation. SYSMPU still enforces MEMORY (stack/data) isolation, which is why
// enforcement is still needed.

#ifndef KICKOS_DRIVER_K64UART_H
#define KICKOS_DRIVER_K64UART_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/sys/service.h> // kos_service_cfg (the bring-up config)

#ifdef __cplusplus
extern "C"
{
#endif

    // The unprivileged driver thread entry. `arg` is the granted UART0 window BASE,
    // passed as the thread-arg VALUE (never dereferenced as memory). The delegated
    // recv cap lands at child table index 1. Loops kos_recv() -> poll-write each byte
    // to UART0_D; exits cleanly when kos_recv returns < 0 (endpoint dead / EPIPE).
    // Spawned by k64uart_console_start(), or directly by a consumer.
    void k64uart_console_driver(void* arg);

    // One-shot console-handover bring-up (call ONCE from the app main, BEFORE spawning
    // any app that should print through the driver). The caller needs KOS_AUTH_CONSOLE
    // and KOS_AUTH_MEMORY:
    //   1. create a console endpoint E,
    //   2. kos_console_publish(E)  (relinquishes the kernel UART, routes stdout to E),
    //   3. spawn the UNPRIVILEGED driver granted the UART0 window + {E | WAIT},
    //   4. close root's own WAIT-bearing cap on E (else driver death cannot EPIPE
    //      and clients hang).
    // `cfg` carries the UART0 window base/size and the driver priority as data (a
    // KOS_SVC_CONSOLE service entry); cfg->prio must be >= every client's priority
    // (D9: no PI on rendezvous). Returns 0, or < 0 on any failure
    // (endpoint/publish/spawn). On failure the caller MUST NOT spawn console-dependent
    // apps: publish and spawn are inseparable.
    int k64uart_console_start(struct kos_service_cfg const* cfg);

#ifdef __cplusplus
}
#endif

#endif
