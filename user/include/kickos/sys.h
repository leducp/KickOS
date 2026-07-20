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

// Counting semaphore. The returned handle is an OPAQUE per-task CAPABILITY (index +
// generation in THIS thread's table); do not assume it's an array index, and it does
// NOT name the same object in another thread -- to share a sem with a child, delegate
// it via kos_thread_params.caps (see kos_cap_grant). sem_create grants the creator a
// full-rights (WAIT|SIGNAL|TRANSFER) cap.
int kos_sem_create(int initial); // -> opaque cap handle, or -1
void kos_sem_wait(int sem);
void kos_sem_post(int sem);

// Priority-inheritance mutex. Like a semaphore, the handle is an OPAQUE per-task
// CAPABILITY -- share it with a child by delegating it via kos_thread_params.caps.
// Possession IS the authority to lock and unlock (no rights split); create grants a
// CAP_TRANSFER-only cap. While a lower-priority holder is contended by a
// higher-priority waiter, the holder is boosted to the waiter's priority until it
// unlocks (bounded priority inversion). Not recursive: locking a mutex you already
// hold returns -2. No trylock/timed lock (parity with the unexposed sem_trywait).
int kos_mutex_create(void); // -> opaque cap handle, or -1 (pool/table full)
// 0 locked; KOS_MUTEX_OWNER_DIED (1) locked but the previous owner died holding it
// (protected state may be inconsistent); -1 bad cap; -2 refused, would deadlock.
int kos_mutex_lock(int mtx);
// 0, or -1 (bad cap, or the caller is not the owner). Only the owner may unlock.
int kos_mutex_unlock(int mtx);

// Synchronous IPC rendezvous endpoint. The handle is an OPAQUE per-task CAPABILITY
// (like a sem/mutex): delegate it to a child via kos_thread_params.caps. create grants
// a full-rights cap (send needs SIGNAL, recv needs WAIT). send and recv block until the
// peer arrives; the kernel copies min(sent, capacity) bytes (receiver-side truncation is
// not an error). A send above KOS_EP_MSG_MAX is rejected (-1); recv clamps its capacity.
int kos_endpoint_create(void); // -> opaque cap handle, or -1 (pool/table full)
// Send `len` bytes. -> bytes transferred (>= 0), or -1 (bad cap/buffer, len too big, dead
// endpoint, or EPIPE: the last receiver went away while parked). n == 0 is a valid signal.
long kos_send(int ep, void const* buf, size_t len);
// Receive up to `cap_len` bytes into buf; badge (if non-null) receives the sender badge
// (always 0 in this stage). -> bytes received (>= 0), or -1 (bad cap/buffer).
long kos_recv(int ep, void* buf, size_t cap_len, uint32_t* badge);

// Hand the kernel console UART over to a userspace driver serving endpoint `ep`.
// Privileged-only (returns -1 for an unprivileged caller or a bad cap). After this the
// kernel chip path drops (RTT, if built, still carries kernel output); libc stdout routes
// through the driver via cap index 0 (seated into children spawned AFTER the publish).
// Re-callable to re-point at a fresh driver. -> 0, or -1.
int kos_console_publish(int ep);

// Drop THIS thread's capability. Type-agnostic (a cap knows its own type) and
// refcounted: the underlying object is destroyed only at the LAST close across all
// holders. Always succeeds on a live cap, even while other holders remain open (it
// touches no waiters). Returns 0, or -1 on a bad/stale cap.
int kos_handle_close(int cap);
int kos_sem_destroy(int cap); // alias of kos_handle_close (source compatibility)

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

// Running core clock in Hz, so an app can do its own cycle<->ns math without the
// kernel hardwiring SystemCoreClock for it. 0 if the backend has no silicon core
// clock (the host sim).
uint32_t kos_cpu_clock_hz(void);

// Retune the core clock to a P-state (the MECHANISM seam; policy belongs to a future
// userspace power manager). Returns the ACTUALLY-LANDED core Hz -- compare it against
// what the requested point implies to learn whether you got it. Returns 0 when the
// chip cannot change its clock, the caller is unprivileged, or a userspace driver owns
// the console (a retune would garble a baud the kernel cannot relocate). Privileged.
uint32_t kos_cpu_clock_set(kos_pstate_t pstate);

// Set the Unix-epoch wall clock: unix_ns is the current time, and the offset
// stored is unix_ns - kos_clock_now(). Backs newlib's _gettimeofday (see
// newlib_stubs.cc), so std::chrono::system_clock::now() reads true epoch time
// after this is called; default offset 0 leaves wall time reading boot-relative.
// Does NOT affect kos_clock_now() -- that stays a pure monotonic counter.
void kos_clock_set_realtime(uint64_t unix_ns);

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
