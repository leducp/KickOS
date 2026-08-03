// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Buffered, IRQ-drained console TX: a byte ring with ONE producer (thread context) and
// ONE consumer (the TX-empty ISR), which decouples a debug-console write from the UART
// bit rate. The panic / fault / pre-arm paths must bypass it and use the synchronous
// writer (arch_console_write_sync); see console.cc.
//
// Declarations only: the implementation lives in the kernel because the producer's
// publish+prime step needs IrqLock.

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
// capacity is size-1. Called once from console_buffer_init after irq_init has
// seeded the dispatch table. Until then, writes route to the synchronous path.
// `irq_line` is stashed for console_tx_deinit and set BEFORE armed flips, so no
// window has the ring armed with a stale (-1) line.
void console_tx_init(struct console_tx_backend const* be, char* storage, uint32_t size,
                     int irq_line);

// Nonzero once console_tx_init has run (the routing guard in console.cc reads it).
int console_tx_armed(void);

// Producer, thread context ONLY (it publishes and primes under IrqLock). On overflow it
// drains and writes the burst synchronously (in order, TX IRQ disabled) rather than
// dropping output.
void console_tx_write(char const* buf, size_t n);

// Consumer (ISR context). Push ring bytes while a slot is free; disable the TX
// IRQ once the ring empties. Bound to the TX line via irq_attach; MUST NOT
// sem_post / switch / block.
void console_tx_isr(void);

// Poll-drain whatever is queued (TX IRQ disabled first so the ISR cannot race the
// drain). Panic uses this to flush queued bytes before printing, preserving order.
void console_tx_flush_sync(void);

// Arch seam: a chip with a buffered console returns its backend + ring storage +
// TX IRQ line here; the fallback TU returns null, leaving the console on the
// synchronous path (sim + polled-only chips). Called once by console_buffer_init.
struct console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size,
                                                         int* irq_line);

// Relinquish the buffered TX path (flush, disable the TX IRQ, detach the line, disarm)
// so a userspace driver can take the UART. Idempotent (no-op if not armed / already
// relinquished). Runs under one IrqLock. See the console-handover design (D2).
void console_tx_deinit(void);

// Console device-ownership seam (state in console.cc). Every kernel-owned device poke
// MUST be bracketed by enter/leave, else kos_console_publish cannot drain an in-flight
// writer before handing the UART over. See the console-handover design (D1/D3).
int console_owner_is_kernel(void);   // nonzero while the kernel owns the UART (KERNEL_OWNED)
void console_owner_set_user(void);   // flip to USER_OWNED (publish's LAST step)
void console_chip_writer_enter(void); // bracket a kernel-owned device poke (count++)
void console_chip_writer_leave(void); // (count--)
int console_chip_writers(void);      // in-flight kernel chip writers (publish drain spin)

// Driver-death reclaim, split in two so the ordering cannot be collapsed.
//
// Called from the cap layer, this only RECORDS that the published console endpoint lost
// its last WAIT-bearing cap. It must not reclaim: it runs at an arbitrary point in
// cap_teardown's loop, where the dying driver's IRQ cap can still be live and its TX
// line still armed, so re-initialising the UART here could race an interrupt still
// firing into irq_event_isr.
void console_note_driver_death(void);
// The reclaim itself, and only from exit_current after the whole teardown loop and after
// domain_release: every cap dropped means every line masked and detached, and the
// exiting thread's claim on the window released. Puts the UART back in a known polled
// state so panic and ordinary kprintf still reach the wire. Idempotent, and a no-op if
// the console was never published, if the dead thread was not its last receiver, or
// while ANY live thread still holds arch_console_reclaim_window(). The note is sticky across
// such a refusal, so the last window holder's exit is what reclaims.
void console_on_driver_death(void);

#ifdef __cplusplus
}
#endif

#endif
