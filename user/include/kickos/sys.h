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
// caller cannot read. THE COUNT CAN BE SHORT, and the usual cause is a FULL TRANSMIT RING
// rather than a bad buffer: the walk stops at the first chunk the console refuses, which is
// whatever an earlier burst has not drained yet. A page unmapped mid-write stops it too, the
// chunks spanning no lock. kos_print discards both, so a line that has to survive a burst
// goes through kickos::emit (sys/emit.h), which retries the remainder.
int32_t kos_kconsole_write(void const* buf, size_t len);
void kos_print(char const* s);

void kos_yield(void);
void kos_sleep_ns(uint64_t ns);

// Capability creation returns status and writes out_cap (KOS_CAP_NONE on failure).
// Handles use all 32 bits and are local to a thread; delegate them through
// kos_thread_params.caps rather than copying their values to another thread.
// Semaphore creation grants WAIT, SIGNAL, and TRANSFER.
// Creation errors are -KOS_ENOMEM for pool exhaustion, -KOS_EMFILE for a full
// capability table, and -KOS_EOVERFLOW for the task's pool budget.
// Also returns EINVAL for initial outside [0, KOS_SEM_COUNT_MAX] or a null/
// misaligned out_cap, and EFAULT if out_cap is not writable (negative codes).
int kos_sem_create(int initial, kos_cap_t* out_cap);
// 0, or -KOS_EBADF (bad/stale/closed cap) / -KOS_EPERM (cap lacks WAIT/SIGNAL).
int kos_sem_wait(kos_cap_t sem);
// Also -KOS_EOVERFLOW with no waiter and the count at KOS_SEM_COUNT_MAX; the token is
// not banked.
int kos_sem_post(kos_cap_t sem);

// Priority-inheritance mutex. The handle is an OPAQUE per-THREAD CAPABILITY, as above.
// Possession IS the authority to lock and unlock (no rights split); create grants a
// CAP_TRANSFER-only cap. A lower-priority holder contended by a higher-priority waiter is
// boosted to the waiter's priority until it unlocks. Not recursive: locking a mutex you
// already hold returns -KOS_EDEADLK.
int kos_mutex_create(kos_cap_t* out_cap); // -> 0, or -KOS_ENOMEM/-KOS_EMFILE/-KOS_EOVERFLOW/-KOS_EINVAL/-KOS_EFAULT
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

// Synchronous IPC endpoint with per-thread capability handles. Create grants
// full rights; send requires SIGNAL and receive requires WAIT. Both block for
// a peer and copy min(sent, capacity) bytes. Truncation is allowed.
// Sends above KOS_EP_MSG_MAX fail with EINVAL; receive capacities are clamped.
int kos_endpoint_create(kos_cap_t* out_cap); // -> 0, or -KOS_ENOMEM/-KOS_EMFILE/-KOS_EOVERFLOW/-KOS_EINVAL/-KOS_EFAULT
// The same endpoint, with its receiver in the kernel running on `node`: locality is settled
// at the mint and never reaches a caller of kos_send. Privileged, and the cap it grants
// carries SIGNAL alone, so a far endpoint is never received on or served locally.
// -> 0, or -KOS_ENOSYS (this image runs one kernel), -KOS_EPERM (unprivileged caller),
// -KOS_EINVAL (the caller's own node, or a port that node did not mint), plus the mint
// refusals above.
int kos_amp_endpoint_create(uint32_t node, uint32_t port, kos_cap_t* out_cap);
// Send `len` bytes, giving up after `timeout_us` RELATIVE microseconds, or never if that is
// KOS_TIMEOUT_NONE. The deadline bounds the PARK only: a receiver already waiting
// rendezvouses regardless of it.
// -> as kos_send below, plus -KOS_ETIMEDOUT (the deadline passed with NO receiver: the send
// did NOT happen and no bytes crossed).
int32_t kos_send_timed(kos_cap_t ep, void const* buf, size_t len, uint32_t timeout_us);
// Wait indefinitely for a receiver. Returns bytes transferred or -KOS_E*:
// EINVAL for len > KOS_EP_MSG_MAX, EFAULT for an invalid buffer, EBADF/EPERM
// for an invalid capability or missing SIGNAL right, EPIPE for a dead endpoint
// or loss of its last receiver. A zero-length message is valid.
//
// For all IPC copies, either buffer may become inaccessible after validation.
// Both parties receive EFAULT; any bytes already copied remain changed.
// See docs/reference/ipc-call-reply.md for partial-copy behavior.
int32_t kos_send(kos_cap_t ep, void const* buf, size_t len);
// Receive through kos_reply_recv with KOS_CAP_NONE as the reply capability.

// Synchronous call/reply. Delivers `send_len` request bytes and blocks until the server
// replies into the SAME buffer, in place, up to `recv_cap`; a one-shot reply cap is minted
// in the server's recv info. -> reply bytes (>= 0), or a negative -KOS_E*: EINVAL (request >
// KOS_EP_MSG_MAX), EFAULT (bad buffer), EBADF/EPERM (bad cap / no SIGNAL), EPIPE (dead
// endpoint or server died mid-call), EMFILE (the SERVER's cap table is full, so the reply cap
// cannot be minted), ENOSYS (server took an info-less recv, so it hosts no calls).
int32_t kos_call(kos_cap_t ep, void* buf, size_t send_len, size_t recv_cap);
// The same call, always through the buffer-carrying KOS_SYS_CALL trap: identical arguments,
// identical result, identical in-place reply. It is the arm kos_call itself falls through to.
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
// Reply and receive using one buffer. lens packs reply_len and recv_cap.
// KOS_CAP_NONE skips the reply. Returns received bytes, -KOS_ENOTIFY for an
// accepted IRQ without a message, or -KOS_E*. Reply errors EBADF and EFAULT
// abort receiving; ESRCH allows it because that transaction has already ended.
int kos_reply_recv(kos_cap_t reply_cap, void* buf, uintptr_t lens,
                   struct kos_reply_recv_opts* opts);

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
int kos_thread_create(struct kos_thread_params const* params, kos_thread_t* out_thread);

// End the CALLING thread with `code`, or the whole system when the caller is root: root's
// exit is a kos_shutdown(code), exactly as returning from main is, so plain C exit() and
// abort() from main end the system with children still alive. Root therefore needs
// KOS_AUTH_SYSTEM to call this at all (see <kickos/sys/init.h>) and panics without it.
void kos_exit(int code) __attribute__((noreturn));

// Request cancellation of a thread created by the caller. Returns 0 or
// -KOS_E*: EBADF for an invalid/exited handle, EPERM for a different parent,
// EINVAL for self-cancellation (use kos_exit).
// Success accepts the request; it does not confirm death. A parked target
// is woken with ECANCELED where supported, then exits at its next syscall.
// A target that makes no further syscalls is not reached. Use join to wait.
int kos_thread_kill(kos_thread_t thread);

// Forcibly end a thread created by the caller and wait timeout_us relative
// microseconds for teardown (KOS_TIMEOUT_NONE: forever; zero: request only).
// Returns 0 after exit, capability teardown, and name release.
// ETIMEDOUT means termination is irreversible but cleanup remains pending.
// ECANCELED means the waiting caller was cancelled; the target still terminates.
// Other errors: EBADF for invalid/exited handles, EPERM for a different parent,
// EINVAL for self, idle, or privileged targets. Error codes are negative.
// The target gets no device-cleanup window and must be scheduled for teardown.
int kos_thread_slay(kos_thread_t thread, uint32_t timeout_us);

// Set allowed cores for a thread in the caller's task; cross-task changes
// require privilege. Intersect nonzero masks with the machine and task grant.
// Zero selects the task default set, excluding isolated cores. An explicit
// mask may include isolated cores; a single bit pins the thread.
// Returns 0 or -KOS_E*: EPERM for no permitted core or unauthorized cross-task
// access, EINVAL for a nonzero mask with no machine core, EBADF for an invalid/
// exited handle, ENOSYS on single-core kernels.
int kos_thread_set_affinity(kos_thread_t thread, uint32_t core_mask);
// The caller's own handle, which is what lets a thread place itself. KOS_THREAD_NONE on a
// single-core kernel, which carries no placement.
kos_thread_t kos_thread_self(void);

// Set a created task's priority ceiling and core grant while it is empty.
// May only narrow the caller's grant. Zero leaves that field unchanged.
// Ignore cores absent from the machine before checking the subset.
// Returns 0 or -KOS_E*: EPERM for wider grants or a different creator, EINVAL
// for invalid ceiling or no machine core, EBADF for an invalid handle,
// EBUSY if the task has a member.
int kos_task_sched_grant(kos_task_t task, uint8_t prio_ceiling, uint32_t core_mask);

// Create an empty task with shared data and fault containment. mem_base/size
// is a region granted read/write to each member, or 0/0 for no shared memory.
// Apply the same memory checks as spawn. Only the creator may add members.
// Members cannot be privileged or supply separate mem_base; MMIO is per thread.
// mem_flags must match any existing self-grant for the block.
// Returns 0 and out_task, or -KOS_EPERM/EINVAL/ENOMEM/EFAULT with KOS_TASK_NONE.
int kos_task_create(void* mem_base, uint32_t mem_size, uint32_t mem_flags,
                    kos_task_t* out_task);

// End a task YOU created: every live member is cancelled, exactly as kos_thread_kill
// cancels one thread and with the same asynchrony, and the handle stops naming anything.
// Returns 0, -KOS_EBADF (bad / stale handle, KOS_TASK_NONE included) or -KOS_EPERM (you did
// not create it). Any MEMBER's death also ends the group.
int kos_task_kill(kos_task_t task);

// Forcibly terminate every member of a task created by the caller, without
// a cleanup window. Wait timeout_us relative microseconds (KOS_TIMEOUT_NONE:
// forever; zero: request only). An empty task succeeds immediately.
// Returns 0 once empty and released. ETIMEDOUT leaves termination pending
// and the handle valid for another wait. ECANCELED cancels only the wait.
// Other errors: EBADF for invalid/implicit tasks, EPERM for a different
// creator, EINVAL if the caller is a member. Error codes are negative.
int kos_task_slay(kos_task_t task, uint32_t timeout_us);

// Wait for a thread created by the caller, with a relative microsecond
// timeout or KOS_TIMEOUT_NONE. Returns 0 if exited, including before this call.
// Returns -KOS_E*: ETIMEDOUT if still running, ECANCELED if the caller is
// cancelled, EBADF for an invalid/reused slot, EPERM for a different parent,
// EDEADLK for self. Handles remain valid after exit until the slot is reused.
int kos_thread_join(kos_thread_t thread, uint32_t timeout_us);

// Wait until the CALLING thread is the last live one (idle aside), then return 0. Returns
// immediately when the caller is already the last, and it is the only wait that reaches
// threads the caller cannot NAME. ROOT ONLY and single-seat: -KOS_EPERM to anyone else.
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

// Raise `irq` at the controller, standing in for a device that fired. Self-test only, and
// gated on the LINE and not on the caller: -KOS_EINVAL for a number out of range,
// -KOS_EPERM for a line the kernel dispatches to a vector of its own (the tick, console TX,
// the doorbell), which is not a device a caller could be standing in for.
int kos_irq_inject(int irq);

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
// identically. Reads 0 on a backend whose calls all take the generic path.
uint32_t kos_ipc_fast_taken(void);
// Test-only: exercise a Rule 7 grant predicate directly (no descriptor forged).
// `op` is an enum kos_grant_op (sys/abi_probe.h):
//   HITS_RESERVED -> grant_hits_reserved(base,size)                  (0/1)
//   RAM_PRIVILEGED/RAM_UNPRIVILEGED -> grant_region_admissible RAM   (0/1)
//   DEV_PRIVILEGED/DEV_UNPRIVILEGED -> grant_region_admissible DEV   (0/1)
//   RESERVED_COUNT -> reserved-block count; RESERVED_BASE/RESERVED_SIZE -> block[base].{base,size}
//   NOCACHE_SUPPORT -> arch_mpu_nocache_support() as a raw enum, not a predicate
//   RAM_NOCACHE -> grant_region_admissible RAM|NOCACHE, unprivileged             (0/1)
// Only meaningful under enforcement (returns -KOS_EINVAL where the kernel has no
// grant module).
uintptr_t kos_grant_probe(uintptr_t op, uintptr_t base, uintptr_t size);
// Test-only: run one address-space seam scenario in the kernel and return its answer (see
// enum kos_aspace_op in sys/abi_probe.h). The map editor has no syscall of its own, so an arm asks
// for a whole scenario rather than for a mapping. -KOS_ENOSYS where the board describes
// regions instead of translating, cast up through the uintptr_t return.
uintptr_t kos_aspace_probe(uintptr_t op, uintptr_t a1);

// Test-only: run one shared-window scenario in the kernel, or read one of its counters (see
// enum kos_amp_op in sys/abi_probe.h). -KOS_EINVAL for a bad op and on an image that is not a node
// of a partition, cast up through the uintptr_t return: a caller reading a counter must read
// the answer as SIGNED first, or a refusal arrives as a very large count.
uintptr_t kos_amp_probe(uintptr_t op, uintptr_t a1);

// Test-only: read one of the cross-core doorbell's per-core counts, or the shape of the matrix
// they are indexed by (see enum kos_doorbell_op in sys/abi_probe.h). -KOS_EINVAL for a bad op, cast
// up through the return, so read the answer as SIGNED before reading it as a number. Total
// over every posture: an image whose doorbell folds out answers a real zero, not a refusal.
uint64_t kos_doorbell_probe(uintptr_t op, uintptr_t a1);

// Test-only: read one item of the CALLER's own scheduling state (see enum kos_sched_op in
// sys/abi_probe.h). -KOS_EINVAL for a bad op and on an image built without KICKOS_ENABLE_SELFTEST,
// cast up through the uintptr_t return.
uintptr_t kos_sched_probe(uintptr_t op);
// Test-only: enable a controller line directly, so an injected raise reaches the
// default handler on masked-by-default controllers (ARM NVIC, RX). Needs KOS_AUTH_IRQ.
int kos_irq_unmask(int line); // 0, or -KOS_EPERM (no KOS_AUTH_IRQ) / -KOS_EINVAL (bad line)
#endif

// Tier-1 IRQ-as-event. The line IS a capability: claiming it needs KOS_AUTH_IRQ, and the
// resulting cap is delegable to an unprivileged driver at spawn. The first-level ISR masks
// the line and posts the bound notification; the holder waits in thread context and unmasks
// once serviced. Possession of the cap, not an authority bit, authorises wait/ack/notify.
// `flags` is a kos_irq_claim_flags set; the trigger type is fixed for the line's life.
// Above one kernel core the claimer must be pinned to the core it runs on (-KOS_EPERM
// otherwise), and the line is routed there; every thread that waits on, acks or discards it
// must be pinned to that same core.
// -> 0, or -KOS_EPERM/EINVAL/EBUSY/EFAULT, -KOS_ENOMEM (binding pool), -KOS_EMFILE (the
// caller's cap table) or -KOS_EOVERFLOW (this TASK's ceiling of bindings, the pool still
// having slots); the cap lands in *out_cap.
int kos_irq_claim(int line, unsigned int flags, kos_cap_t* out_cap);
// Attach this line to a notification as one signaller among others. The ISR raises
// `notify_cap`'s BADGE bit there, so badge the capability first if the object carries more
// than one source. Needs KOS_CAP_WAIT on the line and KOS_CAP_SIGNAL on the notification.
// ONE-WAY and once only: 0, -KOS_EALREADY (this line already signals something), -KOS_EBADF,
// -KOS_EPERM (a missing right, or the notification already carries a line claimed on another
// core) or -KOS_EOVERFLOW.
int kos_irq_bind_notify(kos_cap_t irq_cap, kos_cap_t notify_cap);
// Rearm early after servicing the device, allowing IRQs during later work.
// Optional: kos_notify_wait rearms on entry. Repeated acks have no effect.
// -KOS_EINVAL for a line attached to no notification: arming it would open a source whose
// raise lands nowhere. -KOS_EPERM also for a caller not pinned to the line's claim core.
int kos_irq_ack(kos_cap_t irq_cap);    // unmask the line; 0, -KOS_EBADF/-KOS_EPERM/-KOS_EINVAL
// Drop the controller's latched pending for the line. An EDGE binding's rearm deliberately
// KEEPS that latch, and the controller is a reserved block no grant can reach, so this is
// the only way to retire a pending the driver knows is stale. Neither masks nor unmasks: use
// it between a wait return and the ack, where the ISR has already left the line masked.
// Needs KOS_CAP_WAIT, and a caller pinned to the line's claim core.
int kos_irq_discard(kos_cap_t irq_cap); // 0, or -KOS_EBADF/-KOS_EPERM

// --- Notifications -----------------------------------------------------------------------
// A notification is a word of 32 badge bits with at most one bound waiter. An IRQ line
// attached with kos_irq_bind_notify is one signaller; a holder of a KOS_CAP_SIGNAL copy
// calling kos_notify is another. Nothing here needs an authority bit or a line: a
// notification with no line attached is a complete object, and the badge on a capability is
// what confines its holder to one bit.
//
// Create one and install a full-rights capability naming it. 0, or -KOS_E* (ENOMEM pool,
// EMFILE cap table, EOVERFLOW this TASK's ceiling of notifications, EINVAL/EFAULT out-ptr).
int kos_notify_create(kos_cap_t* out_cap);
// MINT a second name for the same object, badged with `bit` (0..31) and carrying the
// source's rights, so a signal through the new capability raises that bit and no other. The
// badge is set AT THE COPY: an unbadged capability is the unconfined one and a badged copy
// reaches its own bit only, which is why `source_cap` must itself be UNBADGED
// (-KOS_EALREADY otherwise). Also -KOS_EBADF, -KOS_EINVAL (bit out of range), -KOS_EFAULT,
// -KOS_EMFILE (the caller's cap table) or -KOS_EOVERFLOW.
int kos_notify_badge(kos_cap_t source_cap, uint32_t bit, kos_cap_t* out_cap);
// Raise this capability's badge bit. Needs KOS_CAP_SIGNAL; touches no controller, so the
// waiter must be idempotent about finding no work. -KOS_EALREADY where the bit was already
// set: the notification stays pending and retrying is unnecessary.
int kos_notify(kos_cap_t notify_cap);
// Become the object's one waiter. Needs KOS_CAP_WAIT. -KOS_EBUSY where another thread is
// bound, or where the caller is already bound to a different object. The bind holds a
// reference of its own, so closing the last capability does not free the object under it.
int kos_notify_bind(kos_cap_t notify_cap);
// Give the binding up. Unconsumed bits are LEFT pending for the next server, which is how a
// driver hands its device over. -KOS_EPERM where the caller is not the bound thread.
int kos_notify_unbind(kos_cap_t notify_cap);
// Wait for any bit of `mask`, consuming and returning them in *out_bits. Every attached line
// whose badge is in `mask` is rearmed on entry, so a driver that never acks still receives
// every later interrupt. `timeout_us` is relative; KOS_TIMEOUT_NONE waits forever.
// 0, or -KOS_E*: EBADF, EPERM (no KOS_CAP_WAIT, the caller is not the bound thread, or its
// own core mask is not exactly the core the attached lines were claimed on), EINVAL (an empty
// mask), EFAULT, ETIMEDOUT, ECANCELED.
int kos_notify_wait(kos_cap_t notify_cap, uint32_t mask, uint32_t timeout_us,
                    uint32_t* out_bits);
uint64_t kos_clock_now(void);   // monotonic nanoseconds

// Running core clock in Hz. 0 if the backend has no silicon core clock (host sim, QEMU virt).
uint64_t kos_cpu_clock_hz(void);

// The branch (peripheral) clock in Hz feeding the register block at `base`, which is the
// peripheral register-BLOCK base (e.g. UART0 @ 0x4006A000). Returns 0 when the chip does not
// know this block's clock, or on the host sim. The value is a snapshot: a later retune of
// this branch is not signalled.
uint32_t kos_periph_clock_hz(uintptr_t base);

// Ungate the clock and drop the bus-side supervisor-protect for the register block at
// `base`, both derived by the kernel from `base`. Call it as the driver's first act:
// where the bus gates the block, earlier reads BusFault or return stale values and
// earlier writes are silently discarded. Authorised by possession, not a capability:
// the caller must hold a live MMIO grant whose base is exactly `base`. Idempotent.
// Returns 0, -KOS_EPERM (caller does not hold that window), -KOS_EINVAL (no entry for
// that base, including bases the chip refuses), or -KOS_ENOSYS (no chip backend).
int kos_periph_enable(uintptr_t base);

// Write a chip-allowlisted register at base+offset with kernel privilege.
// Requires a live MMIO grant whose base matches exactly. Needed for registers
// whose unprivileged writes are ignored by the bus, such as XMC USIC FDR/BRG/CCR.
// Returns 0, -KOS_EPERM without the grant, -KOS_EINVAL outside the allowlist,
// or -KOS_ENOSYS without a backend.
int kos_periph_reg_write(uintptr_t base, uintptr_t offset, uint32_t value);

// Irreversibly intersect caller authority with mask. cap must be the
// KOS_CAP_AUTHORITY pseudo-handle. Zero drops all authority; absent bits
// cannot be added. Only a spawning parent can set initial authority.
// Returns 0, -KOS_EBADF if no authority remains, or -KOS_EINVAL for another cap.
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
uint64_t kos_cpu_clock_set(kos_pstate_t pstate);

// Set the Unix-epoch wall clock: unix_ns is the current time, and the offset
// stored is unix_ns - kos_clock_now(). Backs newlib's _gettimeofday (see
// newlib_stubs.cc), so std::chrono::system_clock::now() reads true epoch time
// after this is called; default offset 0 leaves wall time reading boot-relative.
// Does NOT affect kos_clock_now(): that stays a pure monotonic counter.
void kos_clock_set_realtime(uint64_t unix_ns);

// Reserve page-aligned user RAM for this task; returns NULL on exhaustion.
// Reservation alone grants no access. Pass it to spawn/task creation or
// use kos_mem_self_grant. Other tasks cannot name this reservation directly.
// On MMU systems the result is an unmapped task address; on MPU systems
// it is an owned arena block. Delegation preserves its address.
// NULL can mean exhausted memory or reservation records. Records are bounded
// and not freed: per address space on MMU, per image on MPU.
void* kos_ram_alloc(size_t size);

// Grant access to RAM reserved by the caller's task. Requires AUTH_MEMORY.
// On MPU systems, add a per-thread region rounded to arch_ram_region_size;
// the rounded range must fit one owned reservation. On MMU systems, map
// the whole reservation into the task's shared address space.
// flags selects memory type. A type change must update the descriptor even
// if the range is already accessible; identical grants consume no new entry.
// MPU grants are limited by hardware region capacity. MMU reservations are
// bounded at allocation.
// Returns 0 or -KOS_E*:
//   EPERM: missing authority, unowned/invalid range, or unsupported memory type.
//   EINVAL: zero size, wraparound, unknown flags, or bad MPU region alignment.
//   ENOMEM: descriptor budget exhausted.
int kos_mem_self_grant(void* base, size_t size, uint32_t flags);

// Borrow the KERNEL'S single diagnostic LED, which the kernel also drives for itself (solid
// on panic). Takes effect on a board whose diag LED the kernel knows.
void kos_kernel_diag_led_set(int on);
void kos_kernel_diag_led_toggle(void);

#if defined(KICKOS_BENCH) && KICKOS_BENCH
// The microbenchmark's own scaffolding, and the ONLY way an app reaches it. `op` is an enum
// kos_bench_op (abi.h); the meaning of a0/a1 and of the return is per-op and documented
// there. Returns -KOS_EINVAL for an unknown op or a bad IRQ line.
int64_t kos_bench(uint32_t op, uint32_t a0, uint32_t a1);
#endif

#ifdef __cplusplus
}
#endif


// Map the frame RUN a capability names into the address space another names, at the
// address the caller chooses. Needs KOS_AUTH_MEMORY. `flags` is KOS_MEM_*; 0 is Normal
// memory. Returns 0 or a negative KOS_E*.
int kos_frame_map(kos_cap_t frame, kos_cap_t space, uintptr_t va, uint32_t flags);

// Unmap a range previously mapped with kos_frame_map. A receive using that
// range fails with -KOS_EFAULT, as does its sender.
int kos_frame_unmap(kos_cap_t frame, kos_cap_t space, uintptr_t va);

#endif
