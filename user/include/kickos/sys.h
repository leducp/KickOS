// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Userspace C syscall API. A C++ RAII layer sits on top in <kickos/kos.h>.

#ifndef KICKOS_SYS_H
#define KICKOS_SYS_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/sys/abi.h>

#ifdef __cplusplus
extern "C"
{
#endif

// The byte-count-or-negative-errno returns below must stay int32_t: `long` is 4 bytes on
// the cross toolchains and 8 on the host. One static_assert per name in
// user/src/syscall_stubs.cc holds them at 4, so a name added here needs one there.

// Debug console: unbuffered, polling, straight at the kernel console, so it works in boot
// and panic. NOT stdout: ordinary output is libc stdio over a userspace console driver.
// Returns bytes written (a len-0 write is a legitimate 0), or -KOS_EFAULT for a buffer the
// caller cannot read. kos_print discards both.
int32_t kos_kconsole_write(void const* buf, size_t len);
void kos_print(char const* s);

void kos_yield(void);
void kos_sleep_ns(uint64_t ns);

// EVERY capability-minting call below returns a status and writes the handle to `*out_cap`,
// which is ALWAYS written, KOS_CAP_NONE on every failure. A handle spends the full 32-bit
// word (abi.h, kos_cap_t), leaving no room for an errno.
//
// Counting semaphore. The handle is an OPAQUE per-THREAD CAPABILITY (index + generation in
// THIS thread's table): it is not an array index, and it does NOT name the same object in
// another thread, so a child gets it by delegation through kos_thread_params.caps (see
// kos_cap_grant). Create grants WAIT|SIGNAL|TRANSFER.
// The two exhaustion codes are NOT interchangeable: -KOS_ENOMEM is the object's own shared
// pool (here KICKOS_MAX_SEMAPHORES), -KOS_EMFILE is THIS thread's capability table. Every
// create below can return either.
// -> 0; -KOS_ENOMEM; -KOS_EMFILE; -KOS_EINVAL (`initial` outside [0, KOS_SEM_COUNT_MAX], or a
// null/misaligned out_cap); -KOS_EFAULT (out_cap is not writable by the caller).
int kos_sem_create(int initial, kos_cap_t* out_cap);
// 0, or -KOS_EBADF (bad/stale/closed cap) / -KOS_EPERM (cap lacks WAIT/SIGNAL).
int kos_sem_wait(kos_cap_t sem);
// Also -KOS_EOVERFLOW with no waiter and the count at KOS_SEM_COUNT_MAX; the token is
// not banked.
int kos_sem_post(kos_cap_t sem);

// Priority-inheritance mutex. The handle is an OPAQUE per-THREAD CAPABILITY, as above.
// Possession IS the authority to lock and unlock (no rights split); create grants a
// CAP_TRANSFER-only cap. A lower-priority holder contended by a
// higher-priority waiter is boosted to the waiter's priority until it unlocks. Not
// recursive: locking a mutex you already hold returns -KOS_EDEADLK. No trylock, no timed
// lock.
int kos_mutex_create(kos_cap_t* out_cap); // -> 0, or -KOS_ENOMEM/-KOS_EMFILE/-KOS_EINVAL/-KOS_EFAULT
// Acquire (ALL error-shaped codes are negative: see <kickos/sys/errno.h>):
//   0               acquired, protected state consistent
//   -KOS_EOWNERDEAD acquired and the lock IS HELD, but the previous owner died holding it and
//                   the state may be torn: repair the invariant, then unlock as normal
//   -KOS_EBADF      bad/stale cap, NOT acquired
//   -KOS_EDEADLK    self/recursive lock or a lock that would close a wait cycle, NOT acquired
// A negative return does NOT uniformly mean "not held": treating every rc < 0 as a failed
// acquire STRANDS the mutex on the -KOS_EOWNERDEAD path.
int kos_mutex_lock(kos_cap_t mtx);
// 0, -KOS_EBADF (bad cap), or -KOS_EPERM (caller is not the owner). Only the owner unlocks.
int kos_mutex_unlock(kos_cap_t mtx);

// Synchronous IPC rendezvous endpoint. The handle is an OPAQUE per-THREAD CAPABILITY, as
// above. Create grants a full-rights cap (send needs SIGNAL, recv needs WAIT). send and recv block until the peer arrives; the kernel copies
// min(sent, capacity) bytes and receiver-side truncation is NOT an error. A send above
// KOS_EP_MSG_MAX is rejected (-KOS_EINVAL); recv clamps its capacity.
int kos_endpoint_create(kos_cap_t* out_cap); // -> 0, or -KOS_ENOMEM/-KOS_EMFILE/-KOS_EINVAL/-KOS_EFAULT
// Send `len` bytes, giving up after `timeout_us` RELATIVE microseconds, or never if that is
// KOS_TIMEOUT_NONE. The deadline bounds the PARK only: a receiver already waiting
// rendezvouses regardless of it.
// -> as kos_send below, plus -KOS_ETIMEDOUT (the deadline passed with NO receiver: the send
// did NOT happen and no bytes crossed).
int32_t kos_send_timed(kos_cap_t ep, void const* buf, size_t len, uint32_t timeout_us);
// No deadline: parks until a receiver arrives, however long that takes.
// -> bytes transferred (>= 0), or a negative -KOS_E*: EINVAL (len > KOS_EP_MSG_MAX), EFAULT
// (bad buffer), EBADF/EPERM (bad cap / no SIGNAL right), EPIPE (dead endpoint, or the last
// receiver went away while parked). n == 0 is a valid zero-length signal, not an error.
int32_t kos_send(kos_cap_t ep, void const* buf, size_t len);
// Receive up to `cap_len` bytes into buf; `info` (if non-null) receives the sender badge
// and reply cap (kos_recv_info: reply_cap == KOS_CAP_NONE for a plain kos_send, a real
// one-shot CAP_REPLY handle for a kos_call). info == NULL is an INFO-LESS recv: it REJECTS
// calls, and the caller's kos_call fails -KOS_ENOSYS.
// -> bytes received (>= 0), or a negative -KOS_E*: EFAULT (bad buffer / out-ptr), EINVAL
// (misaligned out-ptr), EBADF/EPERM (bad cap / no WAIT right).
int32_t kos_recv(kos_cap_t ep, void* buf, size_t cap_len, struct kos_recv_info* info);
// The same receive, giving up after opts->timeout_us RELATIVE microseconds (or never, if
// that is KOS_TIMEOUT_NONE). `opts` is in-out: it must be non-null, and readable as well as
// writable. The kernel writes only opts->info, so opts->timeout_us survives and a recv loop
// may reuse one struct.
// -> as kos_recv, plus -KOS_ETIMEDOUT (the deadline passed with no sender: nothing was
// received) and -KOS_EINVAL for opts == NULL.
int32_t kos_recv_timed(kos_cap_t ep, void* buf, size_t cap_len,
                       struct kos_recv_timed_opts* opts);

// Synchronous call/reply. Delivers `send_len` request bytes and blocks until the server
// replies into the SAME buffer, in place, up to `recv_cap`; a one-shot reply cap is minted
// in the server's recv info. -> reply bytes (>= 0), or a negative -KOS_E*: EINVAL (request >
// KOS_EP_MSG_MAX), EFAULT (bad buffer), EBADF/EPERM (bad cap / no SIGNAL), EPIPE (dead
// endpoint or server died mid-call), EMFILE (the SERVER's cap table is full, so the reply cap
// cannot be minted), ENOSYS (server took an info-less recv, so it hosts no calls).
int32_t kos_call(kos_cap_t ep, void* buf, size_t send_len, size_t recv_cap);
// The same call, always through the buffer-carrying KOS_SYS_CALL trap: identical arguments,
// identical result, identical in-place reply, and the register form never attempted. It is the
// arm kos_call itself falls through to.
int32_t kos_call_generic(kos_cap_t ep, void* buf, size_t send_len, size_t recv_cap);
// The same call, giving up after `timeout_us` RELATIVE microseconds, or never if that is
// KOS_TIMEOUT_NONE. The deadline bounds the WHOLE call, both phases: the wait for a server
// to take the request AND the wait for its reply.
// -> as kos_call, plus -KOS_ETIMEDOUT, on which NO reply was received. A request already
// taken by a server stays taken: its eventual kos_reply gets -KOS_ESRCH and the reply cap is
// consumed there. Nothing is retried and no bytes land in the buffer after this returns.
int32_t kos_call_timed(kos_cap_t ep, void* buf, size_t send_len, size_t recv_cap,
                       uint32_t timeout_us);
// Complete the call named by `reply_cap` (from kos_recv_info.reply_cap): copy `len` reply
// bytes to the parked caller and wake it. The cap is ONE-SHOT, consumed here; a server loop
// must reply or kos_handle_close it on EVERY path, else the caller parks forever.
// -> 0, or a negative -KOS_E*: EBADF (bad / non-reply cap), EFAULT (bad reply buffer),
// ESRCH (the caller is already gone, aborted or its slot reused; cap consumed anyway).
int kos_reply(kos_cap_t reply_cap, void const* buf, size_t len);

// Hand the kernel console UART over to a userspace driver serving endpoint `ep`.
// Needs KOS_AUTH_CONSOLE. After this the kernel chip path drops (RTT, if built, still
// carries kernel output) and libc stdout routes through the driver via cap index 0, seated
// both into children spawned AFTER the publish and into the CALLER's own table. Re-callable
// to re-point at a fresh driver, caller's cap 0 included. -> 0, -KOS_EPERM (no
// KOS_AUTH_CONSOLE), -KOS_EBADF (bad / non-endpoint / stale cap), or -KOS_EOVERFLOW (the
// endpoint's reference count is at its ceiling; nothing was published and the kernel
// console is untouched).
int kos_console_publish(kos_cap_t ep);

// Drop THIS thread's capability. Type-agnostic and refcounted: the underlying object is
// destroyed only at the LAST close across all holders, and a close touches no waiters.
// Returns 0, -KOS_EBADF (bad/stale cap), or -KOS_EBUSY (a mutex you still hold; unlock it
// first).
int kos_handle_close(kos_cap_t cap);
int kos_sem_destroy(kos_cap_t cap); // alias of kos_handle_close

// Start a thread. A thread handle spends the whole 32-bit word (abi.h, kos_thread_t) and
// cannot share a return value with an errno: this returns 0 and writes the child's handle
// to `*out_thread`, or a negative -KOS_E* (EINVAL/EFAULT malformed params or out-pointer,
// EPERM privilege or authority, EBADF a grant naming no live cap, EBUSY an MMIO window a
// live thread holds, ENOMEM thread pool / stack arena / domain pool, EOVERFLOW a delegated
// object's refcount at its ceiling). `*out_thread` is ALWAYS written, KOS_THREAD_NONE on
// every failure, and the out-pointer is validated BEFORE the child is created.
int kos_thread_spawn(struct kos_thread_params const* params, kos_thread_t* out_thread);

// End the CALLING thread with `code`, or the whole system when the caller is root: root's
// exit is a kos_shutdown(code), exactly as returning from main is, so plain C exit() and
// abort() from main end the system with children still alive. Root therefore needs
// KOS_AUTH_SYSTEM to call this at all (see <kickos/sys/init.h>) and panics without it.
void kos_exit(int code) __attribute__((noreturn));

// Cancel a thread YOU spawned, named by the handle kos_thread_spawn delivered. Returns 0,
// -KOS_EBADF (bad / stale / already-exited handle, KOS_THREAD_NONE included), -KOS_EPERM
// (you did not spawn it) or -KOS_EINVAL (naming yourself; that is kos_exit).
//
// ASYNCHRONOUS: 0 means the request was accepted, never that the thread is gone. The target
// is broken out of WHATEVER it is parked on (irq_wait, recv, mutex, semaphore, sleep) with
// -KOS_ECANCELED where the primitive has a code to carry one, gets one window to clean up,
// and is ended by the KERNEL at its next syscall. A thread that never asks the kernel for
// anything again is not reached at all. To OBSERVE the death, use kos_thread_join below.
int kos_thread_kill(kos_thread_t thread);

// FORCIBLY end a thread YOU spawned, and wait up to `timeout_us` RELATIVE microseconds
// (KOS_TIMEOUT_NONE: no bound; 0: arm and return) for it to be gone. Same handle, same
// parenthood gate and same reach as kos_thread_kill above.
//   0                  GONE. The target is EXITED, its capability table has been swept and
//                      every name it held is released.
//   -KOS_ETIMEDOUT     CONDEMNED, and irrevocably: the target will never execute another
//                      unprivileged instruction, but its capability table is not yet swept,
//                      so a name it holds is not yet released. Nothing is retryable; only
//                      the cleanup is outstanding.
//   -KOS_ECANCELED     the CALLER was cancelled while waiting. The target is still condemned.
// Plus -KOS_EBADF (bad / stale / already-exited handle), -KOS_EPERM (you did not spawn it)
// and -KOS_EINVAL (naming yourself, idle, or a privileged thread, refused and never masked).
//
// A slain DRIVER thread never gets the window in which it would have quieted its device;
// kos_thread_kill is the call that leaves that window open. The wait can also be starved:
// the target must be SCHEDULED to run its own teardown.
int kos_thread_slay(kos_thread_t thread, uint32_t timeout_us);

// Create a TASK: a group of threads that share one data region and one fate. `mem_base` /
// `mem_size` is the shared region, or 0/0 for a group that shares no memory and is only a
// kill group; it is granted R|W to every member and is admitted exactly as a spawn-time
// mem_base is (arena-confined, reserved-block-clear, KOS_AUTH_MEMORY where that applies).
//
// The task starts EMPTY: kos_thread_params::task is what seats a member, and only THIS
// thread may seat one. Returns 0 with *out_task seated, or -KOS_EPERM / -KOS_EINVAL /
// -KOS_ENOMEM / -KOS_EFAULT with *out_task == KOS_TASK_NONE.
//
// A member may bring NO mem_base of its own and may not be privileged; an mmio_base still
// belongs to the one member that asks for it.
//
// `mem_flags` is kos_mem_flags, and MUST match the flags the same block was self-granted
// with: a mismatch leaves the block with two live mappings that disagree.
int kos_task_create(void* mem_base, uint32_t mem_size, uint32_t mem_flags,
                    kos_task_t* out_task);

// End a task YOU created: every live member is cancelled, exactly as kos_thread_kill
// cancels one thread and with the same asynchrony, and the handle stops naming anything.
// Returns 0, -KOS_EBADF (bad / stale handle, KOS_TASK_NONE included) or -KOS_EPERM (you did
// not create it). Any MEMBER's death also ends the group.
int kos_task_kill(kos_task_t task);

// FORCIBLY end a task YOU created: every live member is SLAIN rather than cancelled, so not
// one of them gets the cleanup window kos_task_kill leaves open. Waits up to `timeout_us`
// RELATIVE microseconds (KOS_TIMEOUT_NONE: no bound; 0: arm and return) for the group to be
// EMPTY, which is a different condition from any single member's death.
//
// Returns 0 (the group is empty and its slot released, so the handle names nothing),
// -KOS_ETIMEDOUT (every member is condemned and irrevocably so, and the handle STILL names
// the group so this can be asked again), -KOS_ECANCELED (the caller was cancelled while
// waiting), -KOS_EBADF (bad / stale handle, or an implicit task, which is unnameable),
// -KOS_EPERM (you did not create it) or -KOS_EINVAL (the caller is itself a member; kos_exit
// is how a member ends its group).
//
// An EMPTY group returns 0 at once.
int kos_task_slay(kos_task_t task, uint32_t timeout_us);

// Wait for a thread YOU spawned to be gone, giving up after `timeout_us` RELATIVE
// microseconds, or never if that is KOS_TIMEOUT_NONE. Returns 0 (the target is gone),
// -KOS_ETIMEDOUT (it outlived the deadline and is still running), -KOS_ECANCELED (the
// CALLER was cancelled while waiting), -KOS_EBADF (a handle naming no slot, or one
// reclaimed under this handle, KOS_THREAD_NONE included), -KOS_EPERM (you did not spawn it)
// or -KOS_EDEADLK (naming yourself).
//
// A target that had ALREADY exited returns 0, not -KOS_EBADF: a thread handle stays valid
// until its slot is reused. Only a spawn that has since REUSED the slot invalidates the
// handle, and then the answer is -KOS_EBADF. The parenthood gate is non-transferable, there
// being no capability to delegate.
int kos_thread_join(kos_thread_t thread, uint32_t timeout_us);

// Wait until the CALLING thread is the last live one (idle aside), then return 0. Takes no
// deadline, and returns immediately when the caller is already the last. It is the only wait
// that reaches threads the caller cannot NAME. ROOT ONLY and single-seat: -KOS_EPERM to
// anyone else.
int kos_wait_last(void);

// End the WHOLE system with `status`: drain the buffered console, then hand over to the
// chip's shutdown, which is also what a returning kickos_init_entry does (see
// <kickos/sys/init.h>). Needs KOS_AUTH_SYSTEM, so it is NOT noreturn: it returns -KOS_EPERM
// to a caller that may not end the system, and does not return at all on success.
int kos_shutdown(int status);

// End the system through the kernel's panic path, printing `msg`: mask interrupts, force the
// console back to a polled channel, flush, print KERNEL PANIC. Does not return, needs no
// authority, and is the only panic path open to an unprivileged thread. The reclaim happens
// on EVERY posture, not only after a console handover.
//
// `msg` is copied into a bounded kernel buffer (longer messages are truncated) and checked
// byte by byte, so an unreadable pointer costs the text, not the panic.
void kos_panic(char const* msg) __attribute__((noreturn));

void kos_irq_inject(int irq);

#if defined(KICKOS_ENABLE_SELFTEST)
// Reboot into the chip's bootloader (firmware-download mode). Needs KOS_AUTH_SYSTEM, so like
// kos_shutdown it is NOT noreturn: the gate can refuse with -KOS_EPERM, and a chip with no
// bootloader entry returns -KOS_ENOSYS. Does not return on success.
int kos_reboot(void);
// Test-only: address of a page that faults on unprivileged access.
void* kos_guard_addr(void);
// Test-only: count of IRQs that fired on a line with no driver (masked by the
// default handler).
uint32_t kos_irq_spurious_count(void);
// Test-only: one nested-trap counter, selected by a KOS_NEST_* constant (sys/abi.h).
// KOS_NEST_UNSET for a figure nothing recorded, and for an unknown selector. The CALLER
// prints: a kernel-side report would put the console writer inside the syscall red zone.
uint32_t kos_nest_witness(int which);
// Test-only: count of calls the trap-handler IPC fastpath COMPLETED. It is the only
// thing that tells a test which of the two call paths ran, since they answer a caller
// identically. Reads 0 where the backend has no fastpath.
uint32_t kos_ipc_fast_taken(void);
// Test-only: exercise a Rule 7 grant predicate directly (no descriptor forged).
// `op` is an enum kos_grant_op (abi.h):
//   HITS_RESERVED -> grant_hits_reserved(base,size)                  (0/1)
//   RAM_PRIVILEGED/RAM_UNPRIVILEGED -> grant_region_admissible RAM   (0/1)
//   DEV_PRIVILEGED/DEV_UNPRIVILEGED -> grant_region_admissible DEV   (0/1)
//   RESERVED_COUNT -> reserved-block count; RESERVED_BASE/RESERVED_SIZE -> block[base].{base,size}
//   NOCACHE_SUPPORT -> arch_mpu_nocache_support() as a raw enum, not a predicate
//   RAM_NOCACHE -> grant_region_admissible RAM|NOCACHE, unprivileged             (0/1)
// Only meaningful under enforcement (returns -KOS_EINVAL where the kernel has no
// grant module).
uintptr_t kos_grant_probe(uintptr_t op, uintptr_t base, uintptr_t size);
// Test-only: enable a controller line directly, so an injected raise reaches the
// default handler on masked-by-default controllers (ARM NVIC, RX). Needs KOS_AUTH_IRQ.
int kos_irq_unmask(int line); // 0, or -KOS_EPERM (no KOS_AUTH_IRQ) / -KOS_EINVAL (bad line)
#endif

// Bind device line `irq` so that firing it posts the semaphore `sem_cap` names, from ISR
// context (tier-2, privileged in-kernel handler). Needs KOS_AUTH_IRQ. Returns 0, or
// -KOS_EPERM (no KOS_AUTH_IRQ, or the cap lacks SIGNAL), -KOS_EINVAL (bad irq line),
// -KOS_EBADF (bad / non-sem / stale cap), -KOS_EBUSY (the line is already bound: no
// stealing).
int kos_irq_attach(int irq, kos_cap_t sem_cap);

// Tier-1 IRQ-as-event. The line IS a capability: claiming it needs KOS_AUTH_IRQ, and the
// resulting cap is delegable to an unprivileged driver at spawn. The first-level ISR masks
// the line and posts the bound notification; the holder waits in thread context and unmasks
// once serviced. Possession of the cap, not an authority bit, authorises wait/ack/notify.
// `flags` is a kos_irq_claim_flags set; the trigger type is fixed for the line's life.
// -> 0, or -KOS_EPERM/EINVAL/EBUSY/EFAULT, -KOS_ENOMEM (binding pool) or -KOS_EMFILE (the
// caller's cap table); the cap lands in *out_cap.
int kos_irq_claim(int line, unsigned int flags, kos_cap_t* out_cap);
int kos_irq_wait(kos_cap_t irq_cap);   // block until the line fires; 0, or -KOS_EBADF/-KOS_EPERM
int kos_irq_ack(kos_cap_t irq_cap);    // unmask the line; 0, or -KOS_EBADF/-KOS_EPERM
// Post the binding WITHOUT touching the controller: the doorbell a service thread rings so
// the IRQ thread, sole owner of the peripheral registers, primes a transfer. The woken
// waiter must tolerate finding nothing asserted. Needs KOS_CAP_SIGNAL.
int kos_irq_notify(kos_cap_t irq_cap); // 0, or -KOS_EBADF/-KOS_EPERM
// Drop the controller's latched pending for the line. An EDGE binding's rearm deliberately
// KEEPS that latch, and the controller is a reserved block no grant can reach, so this is
// the only way to retire a pending the driver knows is stale. Neither masks nor unmasks: use
// it between a wait return and the ack, where the ISR has already left the line masked.
// Needs KOS_CAP_WAIT.
int kos_irq_discard(kos_cap_t irq_cap); // 0, or -KOS_EBADF/-KOS_EPERM
uint64_t kos_clock_now(void);   // monotonic nanoseconds

// Running core clock in Hz. 0 if the backend has no silicon core clock (the host sim).
uint32_t kos_cpu_clock_hz(void);

// The branch (peripheral) clock in Hz feeding the register block at `base`, which is the
// peripheral register-BLOCK base (e.g. UART0 @ 0x4006A000). Returns 0 when the chip does not
// know this block's clock, or on the host sim. No rate-change notification.
uint32_t kos_periph_clock_hz(uintptr_t base);

// Ungate the clock and drop the bus-side supervisor-protect for the register block at
// `base`, both derived by the kernel from `base`. Call it as the driver's first act:
// where the bus gates the block, earlier reads BusFault or return stale values and
// earlier writes are silently discarded. Authorised by possession, not a capability:
// the caller must hold a live MMIO grant whose base is exactly `base`. Idempotent.
// Returns 0, -KOS_EPERM (caller does not hold that window), -KOS_EINVAL (no entry for
// that base, including bases the chip refuses), or -KOS_ENOSYS (no chip backend).
int kos_periph_enable(uintptr_t base);

// Write `value` to the register at `base + offset` PRIVILEGED, for the registers whose WRITE
// side the bus classifies supervisor-only inside a window this thread legitimately holds
// (XMC4800 USIC FDR/BRG/CCR). Such a store from an unprivileged thread is SILENTLY DISCARDED
// by the bus.
//
// Authorised by possession, like kos_periph_enable: the caller must hold a live MMIO grant
// whose base is exactly `base`. Not blanket write access: the chip carries an ALLOWLIST of
// (base, offset) pairs and everything else is refused.
// Returns 0, -KOS_EPERM (caller does not hold that window), -KOS_EINVAL (base+offset is
// not on the allowlist), or -KOS_ENOSYS (no chip backend).
int kos_periph_reg_write(uintptr_t base, uintptr_t offset, uint32_t value);

// Drop authority: narrow the caller's authority word to `mask` (kos_cap_authority bits),
// which can only CLEAR bits. A mask naming a bit it lacks does not add it. `cap` must be
// KOS_CAP_AUTHORITY, a pseudo-handle: the authority word is TCB state, not a table entry.
// Narrowing to 0 gives up every authority.
//
// Irreversible for the caller: nothing widens an authority word, and only a spawning parent
// can seat one.
//
// Needs no authority itself. Returns 0, -KOS_EBADF (the caller holds no authority to give
// up), or -KOS_EINVAL (cap is not an authority cap: narrowing object rights is not
// supported).
int kos_cap_narrow(kos_cap_t cap, uint8_t mask);

// One-shot init-time pin-function config: point pin `pin` of port `port` at raw
// chip function code `func` (the PC/PCR encoding, opaque here). Needs AUTH_PINMUX.
// Returns 0, -KOS_EPERM (no authority), -KOS_EINVAL (out of range), -KOS_EBUSY
// (kernel-owned pin, e.g. the console/diag-LED), or -KOS_ENOSYS (no chip backend).
int kos_pinmux_set(uint32_t port, uint32_t pin, uint32_t func);

// Retune the core clock to a P-state. Returns the ACTUALLY-LANDED core Hz, which the caller
// must compare against what the requested point implies. Needs KOS_AUTH_PSTATE. Returns 0
// when the chip cannot change its clock, the caller lacks that authority, or a userspace
// driver owns the console (a retune would garble a baud the kernel cannot relocate).
uint32_t kos_cpu_clock_set(kos_pstate_t pstate);

// Set the Unix-epoch wall clock: unix_ns is the current time, and the offset
// stored is unix_ns - kos_clock_now(). Backs newlib's _gettimeofday (see
// newlib_stubs.cc), so std::chrono::system_clock::now() reads true epoch time
// after this is called; default offset 0 leaves wall time reading boot-relative.
// Does NOT affect kos_clock_now(): that stays a pure monotonic counter.
void kos_clock_set_realtime(uint64_t unix_ns);

// Allocate a page-aligned block from the MPU-governed user-RAM pool, to hand to a thread as
// its domain data region (see kos_thread_params.mem_base). NULL if exhausted.
//
// Allocating does NOT make the block reachable by the caller: it reserves arena memory and
// grants nothing. Hand it to a spawn, or ask for it explicitly with kos_mem_self_grant.
void* kos_ram_alloc(size_t size);

// Add [base, base+size) to the CALLING thread's own region set, so the caller may
// dereference memory it allocated: kos_ram_alloc reserves, this grants, and nothing grants
// implicitly.
//
// Requires AUTH_MEMORY. The region is run through the same Rule 7 admission predicate as a
// spawn-time grant, so it must be inside the user arena and clear of every kernel-reserved
// block; the committed extent is rounded up to what the MPU can describe
// (arch_ram_region_size). The gate admits ANY reserved-clear in-arena range, not only memory
// the caller itself allocated.
//
// BOUNDED by the hardware region budget: a thread already spends up to 5 of
// KICKOS_MPU_MAX_REGIONS on code, static data, its domain and its stack, so self-grants draw
// on a small remainder. This is not a general mapping call.
//
// `flags` is kos_mem_flags: the memory TYPE to commit the region with. Where the chip
// PROGRAMS that type, asking for it spends a descriptor even on a block the caller can
// already reach cacheably, a privileged caller's whole-arena background-map reach included.
//
// Returns 0 on success (including when the range is ALREADY reachable with the same memory
// type, which costs no descriptor), or:
//   -KOS_EPERM   no AUTH_MEMORY, the range is inadmissible (outside the arena, or
//                overlapping a reserved block), or this chip cannot honour the memory
//                type asked for
//   -KOS_EINVAL  size 0, the range wraps, an undefined flag bit, or (under an MPU) the
//                base is not naturally aligned to the rounded region size (a base from
//                kos_ram_alloc never trips this)
//   -KOS_ENOMEM  the caller's region budget is full
int kos_mem_self_grant(void* base, size_t size, uint32_t flags);

// Borrow the KERNEL'S single diagnostic LED, which the kernel also drives for itself (solid
// on panic). No-op on boards with no known LED.
void kos_kernel_diag_led_set(int on);
void kos_kernel_diag_led_toggle(void);

#if defined(KICKOS_BENCH) && KICKOS_BENCH
// The microbenchmark's own scaffolding, and the ONLY way an app reaches it. `op` is an enum
// kos_bench_op (abi.h); the meaning of a0/a1 and of the return is per-op and documented
// there. Returns -KOS_EINVAL for an unknown op or a bad IRQ line.
int32_t kos_bench(uint32_t op, uint32_t a0, uint32_t a1);
#endif

#ifdef __cplusplus
}
#endif

#endif
