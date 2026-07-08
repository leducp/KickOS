// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Userspace C syscall API. Plain-C ergonomic wrappers over the arch syscall
// trap. A C++ RAII layer sits on top in <kickos/kos.h>.

#ifndef KICKOS_SYS_H
#define KICKOS_SYS_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/sys/abi.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Debug console: unbuffered, polling, pointed straight at the kernel console --
// the developer escape hatch (works in boot/panic, driver-independent). This is
// NOT stdout: KickOS has no fd namespace; ordinary output = libc stdio over a
// userspace console driver (Later). kos_print strlen's in the stub; the syscall
// takes an explicit (buf, len) so the kernel never reads an unbounded user ptr.
long kos_kconsole_write(void const* buf, size_t len);
void kos_print(char const* s);

void kos_yield(void);
void kos_sleep_ns(uint64_t ns);

// Counting semaphore. The returned handle is OPAQUE (index + generation); do not
// assume it's an array index. sem_destroy is quiescent-only (fails with waiters)
// and bumps the slot generation, so a stale handle fails to resolve.
int kos_sem_create(int initial); // -> opaque handle, or -1
void kos_sem_wait(int sem);
void kos_sem_post(int sem);
int kos_sem_destroy(int sem); // 0, or -1 (bad handle / has waiters)

int kos_thread_spawn(struct kos_thread_params const* params);
void kos_exit(int code) __attribute__((noreturn));
void kos_irq_inject(int irq);

#if defined(KICKOS_ENABLE_SELFTEST)
// Test-only: address of a page that faults on unprivileged access, for the MPU
// privilege self-test. Not part of the production syscall ABI.
void* kos_guard_addr(void);
// Test-only: count of IRQs that fired on a line with no driver (masked by the
// default handler). For the spurious-IRQ self-test.
uint32_t kos_irq_spurious_count(void);
// Test-only: enable a controller line directly, so an injected raise reaches the
// default handler on masked-by-default controllers (ARM NVIC, RX). Privileged.
int kos_irq_unmask(int line); // 0, or -1
#endif

// Bind device line `irq` so that firing it posts semaphore `sem_id` from ISR
// context (tier-2, privileged in-kernel handler). Returns 0, or -1 if the caller
// is unprivileged, the irq/handle is bad, or the line is already bound.
int kos_irq_attach(int irq, int sem_id);

// Tier-1 IRQ-as-event: an unprivileged driver binds a line (irq_register), waits
// for it to fire (irq_wait, thread context), then unmasks it once serviced
// (irq_ack). The first-level ISR masks the line and posts the bound notification.
int kos_irq_register(int line); // -> handle, or -1
int kos_irq_wait(int handle);   // block until the line fires; 0, or -1 on bad handle
int kos_irq_ack(int handle);    // unmask the line; 0, or -1 on bad handle
uint64_t kos_clock_now(void);   // monotonic nanoseconds

// Allocate a page-aligned block from the MPU-governed user-RAM pool, to hand to
// a thread as its domain data region (see kos_thread_params.mem_base). NULL if
// exhausted. On MCU this pool is a linker-defined region.
void* kos_ram_alloc(size_t size);

// Borrow the KERNEL'S single diagnostic LED (the kernel also drives it for
// self-debug, e.g. solid on panic). Not an app-owned device: this is provisional
// until the capability model gives userspace a real GPIO driver. No-op on boards
// with no known LED.
void kos_kernel_diag_led_set(int on);
void kos_kernel_diag_led_toggle(void);

#ifdef __cplusplus
}
#endif

#endif
