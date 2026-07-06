// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Buffered, IRQ-drained console TX: a single-producer (thread) / single-consumer
// (TX-empty ISR) byte ring that decouples a debug-console write from the UART bit
// rate. The producer memcpys into the ring and primes the TX interrupt; the ISR
// drains the ring into the peripheral and disables its own interrupt when empty.
// Replaces the polled busy-wait writer (the largest on-CPU cost measured by
// telemetry). The panic / fault / pre-arm paths bypass this and use the
// synchronous writer (arch_console_write_sync) instead -- see console.cc.
//
// This header is a leaf (declarations only); the implementation lives in the
// kernel because the producer's publish+prime step needs IrqLock.

#ifndef KICKOS_CONSOLE_TX_H
#define KICKOS_CONSOLE_TX_H

#include <stddef.h>
#include <stdint.h>

// Publish barrier between the ring payload store and the head update. Compiler-
// only by default (correct on the in-order, single-core M-class parts today);
// a weakly-ordered core injects a real fence via -DKICKOS_CONSOLE_TX_BARRIER=...
// (same seam as rtt.h). Head is published under IrqLock, so no fence is needed
// against the same-core ISR -- this only pins the compiler's store order.
#ifndef KICKOS_CONSOLE_TX_BARRIER
#define KICKOS_CONSOLE_TX_BARRIER() __asm volatile("" ::: "memory")
#endif

#ifdef __cplusplus
extern "C"
{
#endif

// The per-chip TX edge. slot_free/push touch one data register; irq_enable/
// irq_disable gate the TX-empty/transmit-buffer interrupt AT THE PERIPHERAL (the
// NVIC line stays enabled once armed). None may block or reschedule.
struct console_tx_backend
{
    int (*slot_free)(void);    // nonzero if the TX data register can take a byte now
    void (*push)(uint8_t b);   // write one byte to the TX data register
    void (*irq_enable)(void);  // enable the TX-empty / transmit-buffer interrupt
    void (*irq_disable)(void); // disable it
};

// Arm the buffered path. `size` MUST be a power of two (index masking); usable
// capacity is size-1. Called once from console_buffer_init after irq_init has
// seeded the dispatch table. Until then, writes route to the synchronous path.
void console_tx_init(struct console_tx_backend const* be, char* storage, uint32_t size);

// Nonzero once console_tx_init has run (the routing guard in console.cc reads it).
int console_tx_armed(void);

// Producer (thread context only). memcpy the burst into the ring, then publish +
// prime the TX IRQ under IrqLock. On overflow, drains the ring and writes the
// burst synchronously (in order, TX IRQ disabled) rather than dropping output.
void console_tx_write(char const* buf, size_t n);

// Consumer (ISR context). Push ring bytes while a slot is free; disable the TX
// IRQ once the ring empties. Bound to the TX line via irq_attach; MUST NOT
// sem_post / switch / block.
void console_tx_isr(void);

// Poll-drain whatever is queued (TX IRQ disabled first so the ISR cannot race the
// drain). Panic uses this to flush queued bytes before printing, preserving order.
void console_tx_flush_sync(void);

// Arch seam: a chip with a buffered console returns its backend + ring storage +
// TX IRQ line here; the default (weak) returns null, leaving the console on the
// synchronous path (sim + polled-only chips). Called once by console_buffer_init.
struct console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size,
                                                         int* irq_line);

#ifdef __cplusplus
}
#endif

#endif
