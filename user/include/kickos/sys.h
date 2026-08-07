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

// The byte-count-or-negative-errno returns below (kos_kconsole_write, kos_send,
// kos_send_timed, kos_recv, kos_recv_timed, kos_call, kos_call_timed) must stay a
// FIXED-WIDTH signed type: `long` is 4 bytes on the cross toolchains and 8 on the host,
// which is two widths for one declaration across the trap boundary. One static_assert per
// name in user/src/syscall_stubs.cc holds them at 4, so a name added here needs one there.

// Debug console: unbuffered, polling, straight at the kernel console (works in
// boot/panic, driver-independent). NOT stdout: KickOS has no fd namespace; ordinary
// output = libc stdio over a userspace console driver (Later). kos_print strlen's in
// the stub; the syscall takes an explicit (buf, len) so the kernel never reads an
// unbounded user ptr. Returns bytes written (>= 0; a len-0 write is a legitimate 0),
// or -KOS_EFAULT if the buffer is not readable by the caller. kos_print stays void:
// it discards both the count and any -KOS_EFAULT.
int32_t kos_kconsole_write(void const* buf, size_t len);
void kos_print(char const* s);

void kos_yield(void);
void kos_sleep_ns(uint64_t ns);

// EVERY capability-minting call below returns a status and writes the handle to `*out_cap`.
// The handle is a full 32-bit word (abi.h, kos_cap_t) with no room left for an errno.
// `*out_cap` is ALWAYS written, KOS_CAP_NONE on every failure, so a caller that ignores the
// status holds a handle no call will accept rather than an uninitialized one.
//
// Counting semaphore. The handle is an OPAQUE per-task CAPABILITY (index + generation in
// THIS thread's table); do not assume it's an array index, and it does NOT name the same
// object in another thread: share it with a child by delegating it via
// kos_thread_params.caps (see kos_cap_grant). Create grants the creator a full-rights
// (WAIT|SIGNAL|TRANSFER) cap.
// The two exhaustion codes are NOT interchangeable: -KOS_ENOMEM is the object's own shared
// pool (here KICKOS_MAX_SEMAPHORES), -KOS_EMFILE is THIS task's capability table. Every
// create below can return either.
// -> 0; -KOS_ENOMEM (sem pool); -KOS_EMFILE (table full); -KOS_EINVAL when `initial` is outside
// [0, KOS_SEM_COUNT_MAX] (abi.h) or out_cap is null/misaligned; -KOS_EFAULT (out_cap is
// not writable by the caller).
int kos_sem_create(int initial, kos_cap_t* out_cap);
// 0, or -KOS_EBADF (bad/stale/closed cap) / -KOS_EPERM (cap lacks WAIT/SIGNAL). Check
// the return: a stale cap must not be mistaken for a completed wait/post.
int kos_sem_wait(kos_cap_t sem);
// Also -KOS_EOVERFLOW with no waiter and the count at KOS_SEM_COUNT_MAX; the token is
// not banked.
int kos_sem_post(kos_cap_t sem);

// Priority-inheritance mutex. Like a semaphore, the handle is an OPAQUE per-task
// CAPABILITY: share it with a child by delegating it via kos_thread_params.caps.
// Possession IS the authority to lock and unlock (no rights split); create grants a
// CAP_TRANSFER-only cap. While a lower-priority holder is contended by a
// higher-priority waiter, the holder is boosted to the waiter's priority until it
// unlocks (bounded priority inversion). Not recursive: locking a mutex you already
// hold returns -KOS_EDEADLK. No trylock/timed lock (parity with the unexposed sem_trywait).
int kos_mutex_create(kos_cap_t* out_cap); // -> 0, or -KOS_ENOMEM/-KOS_EMFILE/-KOS_EINVAL/-KOS_EFAULT
// Acquire. Return codes (ALL error-shaped codes are negative: see <kickos/sys/errno.h>):
//   0               acquired, protected state consistent
//   -KOS_EOWNERDEAD acquired BUT the previous owner died holding it (state may be torn):
//                   the lock IS HELD. Repair the invariant, then unlock as normal.
//   -KOS_EBADF      bad/stale cap, NOT acquired
//   -KOS_EDEADLK    self/recursive lock or a lock that would close a wait cycle, NOT acquired
// CAVEAT (robust mutex): a negative return does NOT uniformly mean "not held". Treating
// every rc < 0 as a failed acquire would STRAND the mutex on the -KOS_EOWNERDEAD path (the
// lock is held). Callers MUST special-case rc == -KOS_EOWNERDEAD as HELD, distinct from
// the other negatives which mean not-held.
int kos_mutex_lock(kos_cap_t mtx);
// 0, -KOS_EBADF (bad cap), or -KOS_EPERM (caller is not the owner). Only the owner unlocks.
int kos_mutex_unlock(kos_cap_t mtx);

// Synchronous IPC rendezvous endpoint. The handle is an OPAQUE per-task CAPABILITY
// (like a sem/mutex): delegate it to a child via kos_thread_params.caps. create grants
// a full-rights cap (send needs SIGNAL, recv needs WAIT). send and recv block until the
// peer arrives; the kernel copies min(sent, capacity) bytes (receiver-side truncation is
// not an error). A send above KOS_EP_MSG_MAX is rejected (-KOS_EINVAL); recv clamps its capacity.
int kos_endpoint_create(kos_cap_t* out_cap); // -> 0, or -KOS_ENOMEM/-KOS_EMFILE/-KOS_EINVAL/-KOS_EFAULT
#if KICKOS_TIMED_WAIT
// Send `len` bytes, giving up after `timeout_us` RELATIVE microseconds, or never if that is
// KOS_TIMEOUT_NONE. The deadline bounds the PARK only: a receiver already waiting
// rendezvouses regardless of it.
// -> as kos_send below, plus -KOS_ETIMEDOUT (the deadline passed with NO receiver: the send
// did NOT happen and no bytes crossed).
int32_t kos_send_timed(kos_cap_t ep, void const* buf, size_t len, uint32_t timeout_us);
#endif
// Send with no deadline: park until a receiver arrives, however long that takes.
// -> bytes transferred (>= 0), or a negative -KOS_E*: EINVAL (len > MSG_MAX), EFAULT (bad
// buffer), EBADF/EPERM (bad cap / no SIGNAL right), EPIPE (dead endpoint, or the last
// receiver went away while parked). n == 0 is a valid zero-length signal, not an error.
int32_t kos_send(kos_cap_t ep, void const* buf, size_t len);
// Receive up to `cap_len` bytes into buf; `info` (if non-null) receives the sender badge
// and reply cap (kos_recv_info: reply_cap == KOS_CAP_NONE for a plain kos_send, a real
// one-shot CAP_REPLY handle for a kos_call). Passing info == NULL is an INFO-LESS recv: it
// REJECTS calls (the caller's kos_call fails -KOS_ENOSYS) and behaves as before for plain
// sends.
// -> bytes received (>= 0), or a negative -KOS_E*: EFAULT (bad buffer / out-ptr), EINVAL
// (misaligned out-ptr), EBADF/EPERM (bad cap / no WAIT right).
int32_t kos_recv(kos_cap_t ep, void* buf, size_t cap_len, struct kos_recv_info* info);
#if KICKOS_TIMED_WAIT
// The same receive, giving up after opts->timeout_us RELATIVE microseconds (or never, if
// that is KOS_TIMEOUT_NONE). The deadline travels in a struct because kos_recv already
// spends all four argument slots, and in its OWN struct (with the out-struct nested at
// opts->info) so that a plain kos_recv cannot reach a timeout field at all. `opts` is
// in-out: it must be non-null, and readable as well as writable. The kernel writes only
// opts->info, so opts->timeout_us survives and a recv loop may reuse one struct.
// -> as kos_recv, plus -KOS_ETIMEDOUT (the deadline passed with no sender: nothing was
// received) and -KOS_EINVAL for opts == NULL.
int32_t kos_recv_timed(kos_cap_t ep, void* buf, size_t cap_len,
                       struct kos_recv_timed_opts* opts);
#endif

// Synchronous call/reply (L4-style). kos_call delivers `send_len` request bytes and
// blocks until the server replies into the SAME buffer (in-place, up to `recv_cap`); it
// mints a one-shot reply cap in the server's recv info. -> reply bytes (>= 0), or a
// negative -KOS_E*: EINVAL (request > KOS_EP_MSG_MAX), EFAULT (bad buffer), EBADF/EPERM
// (bad cap / no SIGNAL), EPIPE (dead endpoint or server died mid-call), EMFILE (the
// SERVER's cap table is full, so the reply cap cannot be minted; nothing the caller can
// widen), ENOSYS (server took an info-less recv, so it hosts no calls).
int32_t kos_call(kos_cap_t ep, void* buf, size_t send_len, size_t recv_cap);
#if KICKOS_TIMED_WAIT
// The same call, giving up after `timeout_us` RELATIVE microseconds, or never if that is
// KOS_TIMEOUT_NONE. The deadline bounds the WHOLE call, both phases: the wait for a server
// to take the request AND the wait for its reply. That differs from kos_send_timed, which
// bounds a park because a plain send has no second phase.
// -> as kos_call, plus -KOS_ETIMEDOUT, on which NO reply was received. A request already
// taken by a server stays taken: the server still holds its one-shot reply cap, its
// eventual kos_reply gets -KOS_ESRCH, and the cap is consumed there. Nothing is retried and
// no bytes land in the buffer after this returns.
int32_t kos_call_timed(kos_cap_t ep, void* buf, size_t send_len, size_t recv_cap,
                       uint32_t timeout_us);
#endif
// Complete the call named by `reply_cap` (from kos_recv_info.reply_cap): copy `len` reply
// bytes to the parked caller and wake it. The cap is one-shot (consumed here; a server
// loop must reply or kos_handle_close it on EVERY path, else the caller parks forever).
// -> 0, or a negative -KOS_E*: EBADF (bad / non-reply cap), EFAULT (bad reply buffer),
// ESRCH (the caller is already gone, aborted or its slot reused; cap consumed anyway).
int kos_reply(kos_cap_t reply_cap, void const* buf, size_t len);

// Hand the kernel console UART over to a userspace driver serving endpoint `ep`.
// Privileged-only (-KOS_EPERM for an unprivileged caller, -KOS_EBADF for a bad cap). After this the
// kernel chip path drops (RTT, if built, still carries kernel output); libc stdout routes
// through the driver via cap index 0 (seated into children spawned AFTER the publish).
// It also seats the CALLER's own cap index 0 to `ep`, so the publishing thread (the
// init/root thread) can itself print through the driver afterwards. Re-callable to
// re-point at a fresh driver (which also re-points the caller's cap 0). -> 0,
// -KOS_EPERM (unprivileged), -KOS_EBADF (bad / non-endpoint / stale cap), or
// -KOS_EOVERFLOW (the endpoint's reference count is at its ceiling; nothing was
// published and the kernel console is untouched).
int kos_console_publish(kos_cap_t ep);

// Drop THIS thread's capability. Type-agnostic (a cap knows its own type) and
// refcounted: the underlying object is destroyed only at the LAST close across all
// holders. Always succeeds on a live cap, even while other holders remain open (it
// touches no waiters). Returns 0, -KOS_EBADF (bad/stale cap), or -KOS_EBUSY (refused: you
// are trying to close a mutex you still hold, unlock it first).
int kos_handle_close(kos_cap_t cap);
int kos_sem_destroy(kos_cap_t cap); // alias of kos_handle_close (source compatibility)

// Start a thread. A thread handle spends the whole 32-bit word (abi.h, kos_thread_t) and
// cannot share a return value with an errno: this returns 0 and writes the child's handle
// to `*out_thread`, or a negative -KOS_E* (EINVAL/EFAULT malformed params or out-pointer,
// EPERM privilege or authority, EBADF a grant naming no live cap, EBUSY an MMIO window a
// live thread holds, ENOMEM thread pool / stack arena / domain pool, EOVERFLOW a delegated
// object's refcount at its ceiling). `*out_thread` is ALWAYS written, KOS_THREAD_NONE on
// every failure.
//
// The out-pointer is validated BEFORE the child is created: a spawn that succeeded and then
// could not deliver its handle would leave a thread nothing can name or kill.
int kos_thread_spawn(struct kos_thread_params const* params, kos_thread_t* out_thread);

// End the CALLING thread with `code`, or the whole system when the caller is root: root's
// exit is a kos_shutdown(code), exactly as returning from main is, so plain C exit() and
// abort() from main end the system with children still alive. Root therefore needs
// KOS_AUTH_SYSTEM to call this at all (see <kickos/sys/init.h>); it panics without it,
// there being no way to report a refusal through a noreturn call.
void kos_exit(int code) __attribute__((noreturn));

// Cancel a thread YOU spawned, named by the handle kos_thread_spawn delivered. Returns 0,
// -KOS_EBADF (bad / stale / already-exited handle, KOS_THREAD_NONE included), -KOS_EPERM
// (you did not spawn it) or -KOS_EINVAL (naming yourself; that is kos_exit).
//
// COOPERATIVE, and the caller must treat it that way: it marks the target and, if the
// target is parked in kos_irq_wait, wakes it there with -KOS_ECANCELED. The target then
// runs its own exit. A thread parked in kos_recv, sleeping, or looping without ever
// reaching kos_irq_wait is marked and KEEPS RUNNING: 0 means the request was accepted,
// never that the thread is gone. A caller that must OBSERVE the death has to wait for it
// separately, which is what kos_thread_join below does when KICKOS_TIMED_WAIT is on.
int kos_thread_kill(kos_thread_t thread);

#if KICKOS_TIMED_WAIT
// Wait for a thread YOU spawned to be gone, giving up after `timeout_us` RELATIVE
// microseconds, or never if that is KOS_TIMEOUT_NONE. Returns 0 (the target is gone),
// -KOS_ETIMEDOUT (it outlived the deadline and is still running), -KOS_EBADF (a handle
// naming no slot, or one reclaimed under this handle, KOS_THREAD_NONE included),
// -KOS_EPERM (you did not spawn it) or -KOS_EDEADLK (naming yourself).
//
// A target that had ALREADY exited returns 0, not -KOS_EBADF: a thread handle stays valid
// until its slot is reused, which is exactly the window a joiner needs. Only a spawn that
// has since REUSED the slot invalidates the handle, and then the answer is -KOS_EBADF.
//
// The gate is the same parenthood thread_kill takes, and for the same reason: it is
// non-transferable, there being no capability to delegate.
int kos_thread_join(kos_thread_t thread, uint32_t timeout_us);

// Wait until the CALLING thread is the last live one (idle aside), then return 0. This is
// the shutdown condition, not a wait for an event, so it takes no deadline: no caller can
// know a bound for it. Returns immediately when the caller is already the last.
//
// It is the only way to await a thread you cannot NAME: a spawn hands out a handle to the
// child alone, so a main whose children spawn their own workers has nothing to join, and
// returning from main would end the system under them.
//
// ROOT ONLY: -KOS_EPERM to anyone else. It reaches outside the caller's own spawn subtree,
// and it is single-seat, so an ordinary thread parking here first would deny root its
// shutdown condition for as long as it waits.
int kos_wait_last(void);
#endif

// End the WHOLE system with `status`: drain the buffered console, then hand over to the
// chip's shutdown. This is what a returning kickos_init_entry does (see
// <kickos/sys/init.h>); an app that wants to stop only its own thread wants kos_exit.
// Privileged-only, so it is NOT noreturn: it returns -KOS_EPERM to a caller that may
// not end the system, and does not return at all on success.
int kos_shutdown(int status);

// End the system through the kernel's panic path, printing `msg`: mask interrupts,
// force the console back to a polled channel, flush, print KERNEL PANIC. Does not
// return. Needs no authority.
//
// The reclaim to a polled channel happens on EVERY posture, not only after a console
// handover: kpanic_enter reclaims from any state that is not already RECLAIMED, so a
// thread holding the console window can garble the channel with no publish at all and
// the banner still reaches the wire.
//
// This is the ONLY way an unprivileged thread can panic. The kernel's panic entry
// touches kernel state no unprivileged thread may touch, so a thread that panics in
// its own frame faults there instead and the message is lost.
//
// `msg` is copied into a bounded kernel buffer (longer messages are truncated) and
// checked byte by byte, so an unreadable pointer costs the text, not the panic.
void kos_panic(char const* msg) __attribute__((noreturn));

void kos_irq_inject(int irq);

#if defined(KICKOS_ENABLE_SELFTEST)
// Reboot into the chip's bootloader (firmware-download mode), so a board can be
// reflashed with no button press. Privileged-only, so like kos_shutdown it is NOT
// noreturn: the gate can refuse with -KOS_EPERM, and a chip with no bootloader entry
// returns -KOS_ENOSYS. Does not return on success.
int kos_reboot(void);
// Test-only: address of a page that faults on unprivileged access, for the MPU
// privilege self-test. Not part of the production syscall ABI.
void* kos_guard_addr(void);
// Test-only: count of IRQs that fired on a line with no driver (masked by the
// default handler). For the spurious-IRQ self-test.
uint32_t kos_irq_spurious_count(void);
// Test-only: exercise a Rule 7 grant predicate directly (no descriptor forged).
// `op` is an enum kos_grant_op (abi.h):
//   HITS_RESERVED -> grant_hits_reserved(base,size)                  (0/1)
//   RAM_PRIVILEGED/RAM_UNPRIVILEGED -> grant_region_admissible RAM   (0/1)
//   DEV_PRIVILEGED/DEV_UNPRIVILEGED -> grant_region_admissible DEV   (0/1)
//   RESERVED_COUNT -> reserved-block count; RESERVED_BASE/RESERVED_SIZE -> block[base].{base,size}
// Only meaningful under enforcement (returns -KOS_EINVAL where the kernel has no
// grant module). For the Rule 7 overlap-matrix self-test.
uintptr_t kos_grant_probe(uintptr_t op, uintptr_t base, uintptr_t size);
// Test-only: enable a controller line directly, so an injected raise reaches the
// default handler on masked-by-default controllers (ARM NVIC, RX). Privileged.
int kos_irq_unmask(int line); // 0, or -KOS_EPERM (unprivileged) / -KOS_EINVAL (bad line)
#endif

// Bind device line `irq` so that firing it posts semaphore `sem_id` from ISR context
// (tier-2, privileged in-kernel handler). Returns 0, or -KOS_EPERM (unprivileged),
// -KOS_EINVAL (bad irq line), -KOS_EBADF (bad sem cap / no SIGNAL right -> EPERM),
// -KOS_EBUSY (the line is already bound: no stealing).
int kos_irq_attach(int irq, kos_cap_t sem_cap);

// Tier-1 IRQ-as-event. The line IS a capability: a privileged bring-up path CLAIMS it
// (needs KOS_AUTH_IRQ) and delegates the resulting cap to the unprivileged driver at
// spawn, which then waits for the line to fire (thread context) and unmasks it once
// serviced. The first-level ISR masks the line and posts the bound notification.
// Possession of the cap, not an authority bit, is what authorises wait/ack/notify.
// `flags` is a kos_irq_claim_flags set; the trigger type is fixed for the line's life.
// -> 0, or -KOS_EPERM/EINVAL/EBUSY/EFAULT, -KOS_ENOMEM (binding pool) or -KOS_EMFILE (the
// caller's cap table); the cap lands in *out_cap.
int kos_irq_claim(int line, unsigned int flags, kos_cap_t* out_cap);
int kos_irq_wait(kos_cap_t irq_cap);   // block until the line fires; 0, or -KOS_EBADF/-KOS_EPERM
int kos_irq_ack(kos_cap_t irq_cap);    // unmask the line; 0, or -KOS_EBADF/-KOS_EPERM
// Post the binding WITHOUT touching the controller: the doorbell a service thread
// rings so the IRQ thread, sole owner of the peripheral registers, primes a transfer.
// The woken waiter must tolerate finding nothing asserted. Needs KOS_CAP_SIGNAL.
int kos_irq_notify(kos_cap_t irq_cap); // 0, or -KOS_EBADF/-KOS_EPERM
// Drop the controller's latched pending for the line. An EDGE binding's rearm
// deliberately KEEPS that latch (a raise that arrived while the line was masked must
// still be delivered), and the controller is a reserved block no grant can reach, so
// this is the only way to retire a pending the driver knows is stale: a raise that
// predates the driver owning the device, or one it has just serviced out of band.
// Neither masks nor unmasks: use it between a wait return and the ack, where the ISR
// has already left the line masked. Needs KOS_CAP_WAIT.
int kos_irq_discard(kos_cap_t irq_cap); // 0, or -KOS_EBADF/-KOS_EPERM
uint64_t kos_clock_now(void);   // monotonic nanoseconds

// Running core clock in Hz, so an app can do its own cycle<->ns math without the
// kernel hardwiring SystemCoreClock for it. 0 if the backend has no silicon core
// clock (the host sim).
uint32_t kos_cpu_clock_hz(void);

// Read-only oracle: the branch (peripheral) clock in Hz feeding the register block
// at `base`, so a driver derives its own baud/prescaler instead of hardwiring a
// number. `base` is the peripheral register-BLOCK base (e.g. UART0 @ 0x4006A000).
// Returns 0 when the chip does not know this block's clock (or the host sim): the
// driver then keeps its explicit fallback. Cascade-free (no rate-change notify).
uint32_t kos_periph_clock_hz(uintptr_t base);

// Ungate the clock and drop the bus-side supervisor-protect for the register block at
// `base`, both derived by the kernel from `base`. Call it as the driver's first act:
// where the bus gates the block, earlier reads BusFault or return stale values and
// earlier writes are silently discarded. Authorised by possession, not a capability:
// the caller must hold a live MMIO grant whose base is exactly `base`. Idempotent.
// Returns 0, -KOS_EPERM (caller does not hold that window), -KOS_EINVAL (no entry for
// that base, including bases the chip refuses), or -KOS_ENOSYS (no chip backend).
int kos_periph_enable(uintptr_t base);

// Write `value` to the register at `base + offset` PRIVILEGED, for the registers whose
// WRITE side the bus classifies supervisor-only inside a window this thread legitimately
// holds (XMC4800 USIC FDR/BRG/CCR). Such a store from an unprivileged thread is
// SILENTLY DISCARDED by the bus, so a driver that needs one must come here.
//
// Authorised by possession, like kos_periph_enable: the caller must hold a live MMIO
// grant whose base is exactly `base`. Possession is not blanket write access: the chip
// carries an ALLOWLIST of (base, offset) pairs and everything else is refused.
// Returns 0, -KOS_EPERM (caller does not hold that window), -KOS_EINVAL (base+offset is
// not on the allowlist), or -KOS_ENOSYS (no chip backend).
int kos_periph_reg_write(uintptr_t base, uintptr_t offset, uint32_t value);

// Drop authority: narrow the caller's authority word to `mask` (kos_cap_authority bits),
// which can only CLEAR bits. A mask naming a bit it lacks does not add it. `cap` must be
// KOS_CAP_AUTHORITY, a pseudo-handle: the authority word is TCB state, not a table entry.
// Narrowing to 0 gives up every authority.
//
// Irreversible for the caller: nothing widens an authority word, and only a spawning
// parent can seat one. The kernel refuses those calls from the same thread from then
// on, including from application code that has gone wrong.
//
// Needs no authority itself: a bit required to drop bits would be one a thread could
// never give up. Returns 0, -KOS_EBADF (cap does not resolve), or -KOS_EINVAL (cap is
// not an authority cap: narrowing object rights is not supported).
int kos_cap_narrow(kos_cap_t cap, uint8_t mask);

// One-shot init-time pin-function config: point pin `pin` of port `port` at raw
// chip function code `func` (the PC/PCR encoding, opaque here). Needs AUTH_PINMUX.
// Returns 0, -KOS_EPERM (no authority), -KOS_EINVAL (out of range), -KOS_EBUSY
// (kernel-owned pin, e.g. the console/diag-LED), or -KOS_ENOSYS (no chip backend).
int kos_pinmux_set(uint32_t port, uint32_t pin, uint32_t func);

// Retune the core clock to a P-state (the MECHANISM seam; policy belongs to a future
// userspace power manager). Returns the ACTUALLY-LANDED core Hz: compare it against
// what the requested point implies to learn whether you got it. Returns 0 when the
// chip cannot change its clock, the caller is unprivileged, or a userspace driver owns
// the console (a retune would garble a baud the kernel cannot relocate). Privileged.
uint32_t kos_cpu_clock_set(kos_pstate_t pstate);

// Set the Unix-epoch wall clock: unix_ns is the current time, and the offset
// stored is unix_ns - kos_clock_now(). Backs newlib's _gettimeofday (see
// newlib_stubs.cc), so std::chrono::system_clock::now() reads true epoch time
// after this is called; default offset 0 leaves wall time reading boot-relative.
// Does NOT affect kos_clock_now(): that stays a pure monotonic counter.
void kos_clock_set_realtime(uint64_t unix_ns);

// Allocate a page-aligned block from the MPU-governed user-RAM pool, to hand to
// a thread as its domain data region (see kos_thread_params.mem_base). NULL if
// exhausted. On MCU this pool is a linker-defined region.
//
// Allocating does NOT make the block reachable by the caller: it reserves arena
// memory and grants nothing. Hand it to a spawn, or ask for it explicitly with
// kos_mem_self_grant below.
void* kos_ram_alloc(size_t size);

// Add [base, base+size) to the CALLING thread's own region set, so the caller may
// dereference memory it allocated: kos_ram_alloc reserves, this grants, and nothing
// grants implicitly.
//
// Requires AUTH_MEMORY, the same authority kos_ram_alloc takes. The region is run
// through the same Rule 7 admission predicate as a spawn-time grant, so it must be
// inside the user arena and clear of every kernel-reserved block; the committed
// extent is rounded up to what the MPU can describe (arch_ram_region_size), exactly
// as a domain data region is. The gate admits ANY reserved-clear in-arena range,
// not only memory the caller itself allocated: AUTH_MEMORY is arena-wide authority.
//
// BOUNDED by the hardware region budget, and loud when it runs out. A thread already
// spends up to 5 of KICKOS_MPU_MAX_REGIONS on code, static data, its domain and its
// stack, so self-grants draw on a small remainder. Grant the regions a thread needs
// for its lifetime at start-up; this is not a general mapping call.
//
// Returns 0 on success (including when the range is ALREADY reachable, which costs
// no descriptor), or:
//   -KOS_EPERM   no AUTH_MEMORY, or the range is inadmissible (outside the arena,
//                or overlapping a reserved block)
//   -KOS_EINVAL  size 0, the range wraps, or (under an MPU) the base is not
//                naturally aligned to the rounded region size (a base from
//                kos_ram_alloc never trips this)
//   -KOS_ENOMEM  the caller's region budget is full
int kos_mem_self_grant(void* base, size_t size);

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
