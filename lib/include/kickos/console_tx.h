// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Buffered, IRQ-drained console TX: a byte ring with ONE producer (thread context) and
// ONE consumer (the TX-empty ISR). The panic / fault / pre-arm paths must bypass it and
// use the synchronous writer (arch_console_write_sync); see console.cc.

#ifndef KICKOS_CONSOLE_TX_H
#define KICKOS_CONSOLE_TX_H

#include <stddef.h>
#include <stdint.h>

// Publish barrier between the ring payload store and the head update. Compiler-only by
// default, which holds on an in-order single-core M-class part; a weakly-ordered core
// must inject a real fence via -DKICKOS_CONSOLE_TX_BARRIER=... (same seam as rtt.h).
// The head is published under IrqLock, so this pins only the compiler's store order.
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
// capacity is size-1. Call once, after irq_init has seeded the dispatch table.
// Until then, writes route to the synchronous path.
// `irq_line` is the line console_tx_deinit detaches.
void console_tx_init(struct console_tx_backend const* be, char* storage, uint32_t size,
                     int irq_line);

// Nonzero once console_tx_init has run (the routing guard in console.cc reads it).
int console_tx_armed(void);

// Producer, thread context ONLY (it publishes and primes under IrqLock). A burst wider
// than the ring is queued in ring-sized chunks with the wait between them UNMASKED, so
// the masked window is one ring copy and not one transmission. Only when nothing drains
// for a whole poll window does it fall back to a bounded synchronous burst (in order, TX
// IRQ disabled) rather than dropping output. A concurrent producer can interleave at a
// chunk boundary; each stream keeps its own order and loses no bytes. A console_tx_deinit
// landing between two chunks stops the buffering: the remainder goes out synchronously on
// the still-kernel-owned UART, which HANDING_OFF holds for a writer already inside the
// chip-writer bracket, and is dropped only once a driver has the device.
void console_tx_write(char const* buf, size_t n);

// Consumer (ISR context). Push ring bytes while a slot is free; disable the TX
// IRQ once the ring empties. Bound to the TX line via irq_attach; MUST NOT
// sem_post / switch / block.
void console_tx_isr(void);

// Poll-drain whatever is queued (TX IRQ disabled first so the ISR cannot race the
// drain). Panic uses this to flush queued bytes before printing, preserving order.
void console_tx_flush_sync(void);

// Arch seam. The fallback TU returns null, which leaves the console on the synchronous
// path (sim and polled-only chips).
struct console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size,
                                                         int* irq_line);

// Relinquish the buffered TX path so a userspace driver can take the UART. Idempotent.
// Runs under one IrqLock, with the ownership state held at HANDING_OFF across it. See the
// console-handover design (D2).
void console_tx_deinit(void);

// Console device-ownership seam (state in console.cc). Every kernel-owned device poke
// MUST be bracketed by enter/leave, else kos_console_publish cannot drain an in-flight
// writer before handing the UART over. See the console-handover design (D1/D3).
int console_owner_is_kernel(void);   // nonzero while the kernel owns the UART (KERNEL_OWNED)
void console_chip_writer_enter(void); // bracket a kernel-owned device poke
void console_chip_writer_leave(void);
int console_chip_writers(void);      // in-flight kernel chip writers (publish drain spin)

// Nonzero while the kernel may still touch the UART. TRUE through a handover, unlike
// console_owner_is_kernel: an in-flight writer finishing there has to reach the device.
int console_chip_writable(void);

// The console handover, in the two halves the drain has to sit between.
//
// begin: refuse every NEW kernel chip writer and relinquish the ring, leaving the UART
// kernel-owned. set_user: hand it to the driver, and retire any pending death note, which
// named the OLD console.
//
// Between them the caller MUST spin console_chip_writers() down to zero. A writer already
// counted when begin ran can still be mid-message with no buffered path left; it finishes
// synchronously on the still-kernel-owned UART, and set_user before that drain would put it
// on the driver's device. The drain converges only because begin refuses new writers, so
// nothing may increment the count outside a bracket.
void console_handover_begin(void);
void console_owner_set_user(void);

// Driver-death reclaim, split in two so a REFUSAL survives the call that made it.
//
// Only RECORDS that the published console endpoint lost its last WAIT-bearing cap. Sticky
// across a refused reclaim: cleared by a reclaim that goes through, and by a re-publish,
// which retires a note naming the console it replaced.
void console_note_driver_death(void);
// The reclaim itself. Puts the UART back in a known polled state so panic and ordinary
// kprintf still reach the wire. Idempotent, and a no-op if the console was never
// published, if the dead thread was not its last receiver, or while ANY live thread still
// holds arch_console_reclaim_window().
//
// MUST run BEFORE the EPIPE wake of the parked senders, not merely inside the same masked
// window: sched::wake admits a switch when the closer is neither exited nor dying, and
// arch_switch swaps INLINE on the sim and on xtensa LX6, so a woken peer would observe a
// dark console. Safe ahead of the wake because cap_teardown releases every IRQ cap the
// dying thread held first, so no line it owned is still armed into irq_event_isr. Only a
// thread DEATH can free the register window, so a refusal is retried at the next death.
void console_on_driver_death(void);

#ifdef __cplusplus
}
#endif

#endif
