// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The KickOS syscall ABI shared by userspace stubs and the kernel dispatch table. Numbers
// are stable contract; argument packing is uintptr_t-wide.

#ifndef KICKOS_SYS_ABI_H
#define KICKOS_SYS_ABI_H

#include <stddef.h>
#include <stdint.h>

#include <kickos/sys/cap_index.h> // KOS_CAP_NONE, KOS_CAP_AUTHORITY, the well-known indices
#include <kickos/sys/cap_rights.h> // kos_cap_rights, kos_cap_authority
#include <kickos/sys/errno.h> // KOS_E* taxonomy: failures return -KOS_Exxx (see below)
#include <kickos/sys/exit_status.h> // KOS_EXIT_FAULT, KOS_EXIT_CANCELLED, KOS_EXIT_PANIC (generated)
#include <kickos/sys/static_assert.h> // KOS_STATIC_ASSERT

// Return-encoding contract (see errno.h). A syscall that can fail returns its error as
// -KOS_Exxx (negative); success is a non-negative byte-count / count, so the two are
// collision-free. Exceptions, all outside the scheme: ram_alloc returns a pointer, 0/NULL on
// ANY failure, cpu_clock_hz / cpu_clock_set return a u64 Hz and periph_clock_hz a u32 Hz,
// each with a 0 == cannot/unknown sentinel.

// A capability handle. 16 index bits + 16 generation bits, so a live handle spends the
// WHOLE 32-bit word and may have bit 31 set: `h < 0` is not an error test on a capability,
// and every capability-MINTING call returns a status and delivers the handle through an
// out-parameter.
typedef uint32_t kos_cap_t;

// A thread handle: 16 index bits + 16 generation bits over the THREAD pool, whose slots and
// generations are unrelated to kos_cap_t's. Both are plain 32-bit words, so the compiler
// will NOT catch a cap handle passed where a thread handle belongs; it just resolves against
// the wrong table. A slot aged past 32768 reclaims mints a handle with bit 31 set, so
// `h < 0` is not an error test here either.
typedef uint32_t kos_thread_t;

// "No thread". The thread pool never seats the all-ones index, so no generation can mint
// this word.
#define KOS_THREAD_NONE 0xFFFFFFFFu

// Bit 31 of a word the kernel waits on (a mutex's, a plain word's): a thread is or was parked
// on it, so the word's release or wake enters the kernel. A plain word's value is bits 30..0
// and a wake rewrites bit 31, so an update of the value masks to 31 bits and keeps bit 31.
#define KOS_WORD_WAITERS 0x80000000u

// A mutex is a word in the memory of the threads that share it, with no kernel object and no
// handle: bits 30..0 name its holder by the holder's lock id (an opaque value only the kernel
// decodes), 0 meaning free, and KOS_WORD_WAITERS says a thread is or was parked on it. Naturally
// aligned, in memory every sharer can write; a holder that died leaves its id, and the next taker
// answers -KOS_EOWNERDEAD.
typedef struct kos_mutex
{
    uint32_t word;
} kos_mutex_t;
#define KOS_MUTEX_INIT {0u}

// A counting semaphore is a word in the memory of the threads that share it, with no kernel
// object and no handle: bits 30..0 hold its count, and KOS_WORD_WAITERS says a thread is or was
// parked on it. Naturally aligned, in memory every sharer can write, which a thread's stack is not
// for any other thread.
typedef struct kos_sem
{
    uint32_t word;
} kos_sem_t;
#define KOS_SEM_INIT(n) {(uint32_t)(n)}

// A condition variable is a word in the memory of the threads that share it, with no kernel
// object and no handle: bits 30..0 count its signals and broadcasts modulo 2^31, and
// KOS_WORD_WAITERS says a thread is or was parked on it. Naturally aligned, in memory every
// sharer can write. All zero is its initial state.
typedef struct kos_cond
{
    uint32_t word;
} kos_cond_t;
#define KOS_COND_INIT {0u}

// A task handle: 16 generation bits over a BIASED index into the task pool, whose slots are
// unrelated to either pool above. The bias is what makes the all-zero word unmintable.
typedef uint32_t kos_task_t;

// "No task": a task answer that names none, and kos_task_cores' own-task argument.
#define KOS_TASK_NONE 0u

// A program handle: a capability on a program of the image table, minted by kos_program_find and
// closed with kos_handle_close.
typedef kos_cap_t kos_program_t;

// KOS_EXIT_FAULT, from <kickos/sys/exit_status.h>, is the exit code a thread killed by a CPU fault
// reports: its task's status (kos_task_state), and the process status when it was the last
// thread live. A clean kos_exit(139) aliases it. KOS_EXIT_CANCELLED is the code a CANCELLED thread
// reports: the kernel ends it, so it never picks a code of its own. Also the status of a task
// whose members were all cancelled. KOS_EXIT_PANIC is the status of a task one of whose threads
// called kos_panic without KOS_AUTH_SYSTEM, and of one whose thread called abort().

// KOS_SYS_EXIT's scope: the calling thread (kos_exit), or its whole task (_exit, so exit(),
// abort() and a fatal signal).
#define KOS_EXIT_THREAD 0u
#define KOS_EXIT_TASK 1u

enum kos_syscall_nr
{
    KOS_SYS_KCONSOLE_WRITE = 1, // (buf, len, flags)     -> bytes written, WHICH CAN BE SHORT
                                //   where the console took part or a page went away
                                //   mid-stream, or with no input byte completed -KOS_EFAULT
                                //   (bad buffer), -KOS_EBUSY (the caller's stdout takes it
                                //   now), -KOS_ECANCELED (cancelled while waiting), -KOS_EINVAL
                                //   (a flag other than KOS_KCONSOLE_NOWAIT) or, with
                                //   KOS_KCONSOLE_NOWAIT, -KOS_ETIMEDOUT where it would have
                                //   to wait. Waits, with no
                                //   bound, for room in the console, for a peer node's claim
                                //   on a shared UART, and while a dead console driver's task
                                //   awaits its reclaim
    KOS_SYS_YIELD = 2,          // ()                    -> 0
    KOS_SYS_SLEEP_NS = 3,       // (ns_lo, ns_hi)        -> 0
    KOS_SYS_HANDLE_CLOSE = 17,  // (cap)   -> 0, -KOS_EBADF (bad cap), -KOS_EPERM (a reserved
                                //   index)
    KOS_SYS_THREAD_CREATE = 7,   // (kos_thread_params*, kos_thread_t* out) -> 0, or -KOS_E*
                                //   (EINVAL/EFAULT/EPERM/EACCES/EBADF/EBUSY/ENOMEM/EAGAIN/
                                //   EOVERFLOW/ENOTSUP). A RAM range it names, data grant,
                                //   stack or memory window, answers ENOTSUP for a memory type
                                //   this chip cannot honour, then EINVAL where no one region
                                //   descriptor names it, then EPERM, then EBUSY. Across
                                //   arguments the first refusal answers, in the order:
                                //   priority, the stack, the data grant, the windows, the
                                //   authority word, the capabilities
    KOS_SYS_EXIT = 8,           // (code, scope)         -> does not return. Ends the calling
                                //   thread (KOS_EXIT_THREAD), or with KOS_EXIT_TASK its task
                                //   as a member's fault does, every other member stopped and
                                //   code the task's status. Ends the SYSTEM when the caller is
                                //   root, which needs KOS_AUTH_SYSTEM for it and panics
                                //   without
    KOS_SYS_IRQ_RAISE = 9,      // (irq_cap) -> 0, or -KOS_EBADF (no live line), -KOS_EACCES (cap
                                //   lacks SIGNAL), -KOS_ENOTSUP (a line the controller cannot
                                //   raise from software, or a banked one)
    KOS_SYS_AMP_COUNT = 10,     // (node, enum kos_amp_count_id, uint32_t* out) -> 0, or
                                //   -KOS_EINVAL (out, a node outside the partition, an unknown
                                //   id), -KOS_EFAULT (out). AMP nodes only.
    KOS_SYS_NOTIFY_CREATE = 11, // (kos_cap_t* out) -> 0, or -KOS_E* (ENOMEM notification
                                //   pool, EMFILE caller's cap table, EAGAIN task's
                                //   notification budget, EINVAL/EFAULT out-ptr, EPERM)
    KOS_SYS_CLOCK_NOW = 12,     // ()  -> monotonic nanoseconds (u64, in registers; cannot fail).
                                //   Where KICKOS_CLOCK_USER is set kos_clock_now reads the same
                                //   counter in user mode and does not make this call
    KOS_SYS_RAM_ALLOC = 13,     // (size)                -> user-RAM ptr, or 0/NULL on ANY failure
    KOS_SYS_IRQ_CLAIM = 14,     // (line, flags, kos_cap_t* out) -> 0, or -KOS_E*: EPERM (lacks
                                //   KOS_AUTH_IRQ, or the arch dispatches the line to a kernel
                                //   vector of its own and no holder ever frees it), EINVAL
                                //   (line/flags/out-ptr), EFAULT (out-ptr), EBUSY (line owned),
                                //   EAGAIN (line still retiring from its last holder, or the
                                //   task's binding budget), ENOMEM (binding pool), EMFILE (cap
                                //   table)
    KOS_SYS_NOTIFY_WAIT = 15,   // (notify_cap, accept mask, timeout_us, uint32_t* out_bits)
                                //   -> 0 with the consumed bits in *out_bits, or -KOS_E*:
                                //   EBADF, EACCES (cap lacks WAIT), EPERM (the caller is not
                                //   the bound thread), EINVAL (an empty mask, or a bad out-ptr),
                                //   EFAULT (out-ptr), ETIMEDOUT, ECANCELED. KOS_TIMEOUT_NONE
                                //   waits forever
    KOS_SYS_IRQ_ACK = 16,       // (irq_cap) -> 0, or -KOS_EBADF / -KOS_EACCES (cap lacks WAIT)
                                //   / -KOS_EINVAL (the line signals no notification)
    KOS_SYS_DIAG_LED_SET = 19,  // (on)                  -> 0 (kernel diagnostic LED)
    KOS_SYS_CPU_CLOCK_HZ = 22,  // ()  -> running core clock in Hz (u64), 0 if unknown (NO KOS_E*).
                                //   On x86_64 the timestamp counter's rate, the one the kernel's
                                //   clock and kos_clock_now scale by
    KOS_SYS_MUTEX_LOCK = 24,    // (kos_mutex_t*, limit lo, limit hi) -> 0 held;
                                //   -KOS_EOWNERDEAD held, its holder having died;
                                //   -KOS_ETIMEDOUT (the time ran out, or a try found it held) /
                                //   -KOS_EDEADLK / -KOS_ECANCELED / -KOS_EINVAL (misaligned, not
                                //   normal memory, the AMP share, a word disagreeing with the
                                //   waiters the kernel holds, a word threads wait on with
                                //   KOS_SYS_WORD_WAIT, or a limit hi the ABI does not define) /
                                //   -KOS_EFAULT (a word the caller cannot write, or one whose
                                //   page went away while it waited) NOT held. The limit is
                                //   KOS_TIMEOUT_REALTIME's: KOS_TIMEOUT_NONE waits forever, 0 and
                                //   a passed realtime deadline are a try. kos_mutex_lock enters
                                //   only on contention
    KOS_SYS_MUTEX_UNLOCK = 25,  // (kos_mutex_t*) -> 0, -KOS_EPERM (caller not the holder),
                                //   -KOS_EINVAL / -KOS_EFAULT (as the lock). Never the death
                                //   point of a kill: the release completes, and the kill lands
                                //   at the next entry
    KOS_SYS_ENDPOINT_CREATE = 26, // (kos_cap_t* out) -> 0, or -KOS_E* (ENOMEM endpoint pool,
                                  //   EMFILE caller's cap table, EAGAIN task's endpoint
                                  //   budget, EINVAL/EFAULT)
    KOS_SYS_NOTIFY_BIND = 28,   // (notify_cap) -> 0, or -KOS_E*: EBADF, EACCES (cap lacks
                                //   WAIT), EBUSY (another thread is bound, or the caller is
                                //   already bound to a different object), EOVERFLOW (the
                                //   object's reference count is at its ceiling)
    KOS_SYS_CPU_CLOCK_SET = 30,  // (kos_pstate_t as u32) -> landed core Hz (u64); 0 == cannot-change
    KOS_SYS_PERIPH_CLOCK_HZ = 32, // (base) -> peripheral branch clock in Hz (u32), 0 if unknown (NO KOS_E*)
    KOS_SYS_PINMUX_SET = 33,  // (port, pin, func) -> 0, -KOS_EPERM (no KOS_AUTH_PINMUX), -KOS_EINVAL (range), -KOS_EBUSY (kernel-owned pin), -KOS_ENOSYS (no backend)
    KOS_SYS_SHUTDOWN = 36,    // (status, enum kos_shutdown_outcome) -> does not return, or
                              //   -KOS_EPERM (no KOS_AUTH_SYSTEM, or KOS_AUTH_BOOTLOADER for that
                              //   outcome, or a reset asked on an AMP node that does not own it),
                              //   -KOS_EINVAL (an unknown outcome), -KOS_ENOSYS (an outcome
                              //   this chip has no entry for)
    KOS_SYS_MEM_SELF_GRANT = 37, // (base, size, kos_mem_flags) -> 0, or -KOS_E*
                              //   (EPERM/EINVAL/ENOMEM/ENOTSUP/EBUSY/EAGAIN). ENOTSUP is a
                              //   memory type this chip cannot honour; EINVAL an undefined flag
                              //   bit, or a range overlapping the caller's own regions in a way
                              //   its MPU cannot decide (any overlap on ARMv8-M); EBUSY the
                              //   range mapped elsewhere with another type; EAGAIN the page
                              //   tables the map needs, past the task's page budget.
    KOS_SYS_TERMINAL_OUTCOME = 38, // (enum kos_shutdown_outcome) -> 0, or -KOS_EPERM (no
                              //   KOS_AUTH_SYSTEM or no KOS_AUTH_BOOTLOADER, or a reset on an
                              //   AMP node that does not own it), -KOS_EINVAL (an unknown
                              //   outcome), -KOS_EALREADY (set once per boot, and set)
    KOS_SYS_PERIPH_ENABLE = 39, // (base) -> 0, -KOS_EPERM (caller holds no window at that base),
                                //   -KOS_EINVAL (no table entry), -KOS_EIO (the block never left
                                //   reset), -KOS_ENOSYS (no backend).
                                //   Gated on possession, not on an authority bit.
    KOS_SYS_CAP_NARROW = 40,   // (cap, mask) -> 0, -KOS_EBADF (the caller holds no authority
                               //   to give up, or the cap names nothing), -KOS_EPERM (a
                               //   reserved index). Intersects the
                               //   authority word, or any cap's rights. UNGATED by authority.
    KOS_SYS_PANIC = 41,        // (msg) -> does not return. Ends the system for root and for
                               //   a holder of KOS_AUTH_SYSTEM; ends the caller's task with
                               //   KOS_EXIT_PANIC otherwise. msg is copied into kernel
                               //   memory bounded + byte-checked; a message the kernel
                               //   cannot read is replaced, never dereferenced.
    KOS_SYS_PERIPH_REG_WRITE = 42, // (base, offset, value) -> 0, -KOS_EPERM (caller holds no
                               //   window at that base), -KOS_EINVAL (base+offset is not on
                               //   this chip's allowlist), -KOS_ENOSYS (no backend). Gated on
                               //   possession of the block at `base`, not on an authority bit.
    KOS_SYS_NOTIFY = 43,       // (notify_cap) -> 0, -KOS_EALREADY (this capability's badge bit
                               //    was already set and the raise had no effect), or
                               //    -KOS_EBADF/-KOS_EACCES (missing SIGNAL). Raises the bit
                               //    without touching any controller.
    KOS_SYS_IRQ_DISCARD = 44,  // (irq_cap) -> 0, or -KOS_EBADF / -KOS_EACCES (cap lacks WAIT).
                               //   Drops whatever the controller has latched for the line.
                               //   Neither masks nor unmasks.
    KOS_SYS_CALL = 46,         // (ep_cap, buf, kos_call_lens_pack(send_len, recv_cap),
                               //   timeout_us) -> reply bytes (>= 0), or -KOS_E* (EINVAL/EFAULT/
                               //   EBADF/EACCES/ENOTSUP, EAGAIN no receiver while a
                               //   KOS_CAP_HANDOUT holder remains, or a far endpoint's peer ring
                               //   is full, and nothing was sent, ECONNREFUSED no receiver and no
                               //   such holder, EPIPE the server died holding the request, EMFILE
                               //   the SERVER's cap table has no slot for the reply cap,
                               //   ETIMEDOUT no reply came in time). KOS_TIMEOUT_NONE waits
                               //   forever
    KOS_SYS_NOTIFY_UNBIND = 47, // (notify_cap) -> 0, or -KOS_E*: EBADF, EACCES (cap lacks
                               //   WAIT), EPERM (the caller is not the bound thread). Unconsumed
                               //   bits are LEFT pending for the next server.
    KOS_SYS_THREAD_JOIN = 48,  // (kos_thread_t, timeout_us) -> 0 (the target is gone,
                               //   INCLUDING a target that had already exited),
                               //   -KOS_ETIMEDOUT, -KOS_ECANCELED (the CALLER was cancelled
                               //   while waiting), -KOS_EBADF (never allocated / reclaimed
                               //   under this handle), -KOS_EPERM (another task's thread
                               //   the caller did not spawn), -KOS_EDEADLK (naming yourself).
    KOS_SYS_SEND = 50,         // (cap, buf, len, timeout_us) -> bytes transferred, or -KOS_E*:
                               //   EINVAL (len above KOS_EP_MSG_MAX, rejected and never
                               //   clamped), EFAULT (bad buffer), EBADF/EACCES (bad cap / no
                               //   SIGNAL right), EAGAIN (no receiver while a KOS_CAP_HANDOUT
                               //   holder remains, or a far endpoint's peer ring is full),
                               //   ECONNREFUSED (no receiver and no such holder), either at
                               //   once or on waking when the last receiver leaves; nothing
                               //   sent. ETIMEDOUT where the deadline passed with no receiver,
                               //   at once for a timeout of 0, which never parks.
                               //   KOS_TIMEOUT_NONE parks indefinitely, and on the published
                               //   console while its task lives, receiver or not.
                               //   EFAULT also answers a
                               //   rendezvous copy refused at either end (sys.h)
    KOS_SYS_THREAD_CANCEL = 53, // (kos_thread_t, enum kos_cancel, timeout_us) -> 0 (with a timeout
                               //    of 0, the cancel is raised; otherwise the target is EXITED
                               //    and its capability teardown finished), -KOS_ETIMEDOUT (the
                               //    cancel raised, cleanup pending), -KOS_ECANCELED (caller
                               //    cancelled; target still cancelled), -KOS_EBADF (bad, stale or
                               //    exited handle), -KOS_EPERM (different parent), -KOS_EINVAL
                               //    (self, idle, or an unknown kind). A timeout other than 0
                               //    parks the caller before the cancel is raised
    KOS_SYS_TASK_SLAY = 54,    // (kos_task_t, timeout_us) -> 0 (a timeout of 0: every live member
                               //   is stopped and the creator's hold dropped, so the handle names
                               //   nothing afterwards; otherwise the group is EMPTY and its slot
                               //   released), -KOS_ETIMEDOUT, -KOS_ECANCELED, -KOS_EBADF (never
                               //   created / freed under this handle / an idle or root task, which is
                               //   unnameable), -KOS_EPERM (the caller did not create it),
                               //   -KOS_EINVAL (a timeout other than 0 from a caller that is
                               //   itself a member, which would wait on its own death).
    KOS_SYS_BENCH = 55,        // (kos_bench_op, a0, a1) -> per-op (see enum kos_bench_op),
                               //   -KOS_EINVAL (bad op, bad line, no span open, or an
                               //   argument outside what the op admits), -KOS_ENOSYS (an op
                               //   this image's core count leaves nothing to measure),
                               //   -KOS_EAGAIN (the end-to-end waiter is not parked yet),
                               //   -KOS_EBUSY (the span that closed was not a wake), -KOS_EBADF
                               //   (the arm's handle names no live IRQ cap), -KOS_EACCES (an
                               //   arm whose cap carries no KOS_CAP_WAIT) or -KOS_EPERM (an op
                               //   that reaches the controller or the doorbell issued without
                               //   KOS_AUTH_IRQ, or
                               //   a thread that is not the armed waiter closing or taring a
                               //   span). The counted ops refuse a count above
                               //   KOS_BENCH_SAMPLES_MAX / KOS_BENCH_ROUNDS_MAX. The
                               //   dispatch arm is compiled out unless KICKOS_BENCH, so a
                               //   normal image returns -KOS_EINVAL for every op. THE RESULT
                               //   IS READ AS A SIGNED 64-BIT WORD.
    KOS_SYS_CALL_REG = 56,     // (ep_cap, kos_call_lens_pack(send_len, recv_cap), payload in
                               //   the remaining argument registers) -> as KOS_SYS_CALL, with
                               //   the reply delivered in registers too. INTERNAL:
                               //   kos_call selects it on size alone. Implemented
                               //   ONLY in the trap-handler fastpath; the generic dispatch
                               //   answers KOS_CALL_REG_FALLBACK, the stub's cue to re-issue
                               //   as KOS_SYS_CALL.
    KOS_SYS_MEM_COUNT = 57,    // (enum kos_mem_count_id, uint32_t* out) -> 0, or -KOS_EPERM (no
                               //   AUTH_MEMORY), -KOS_EINVAL (out, an unknown id, or
                               //   KOS_MEM_RANGES_FREE with no address space), -KOS_EFAULT
                               //   (out). Translating boards only.
    KOS_SYS_ASPACE_SELF = 58,  // (kos_cap_t* out) -> 0, or -KOS_EINVAL (out), -KOS_EFAULT
                               //   (out), -KOS_EMFILE (cap table). Translating boards only.
    KOS_SYS_FRAME_CREATE = 59, // (bytes, kos_cap_t* out) -> 0, or -KOS_EPERM (no
                               //   AUTH_MEMORY), -KOS_EINVAL (out, a caller with no address
                               //   space, bytes 0 or past a run), -KOS_EFAULT (out), -KOS_EAGAIN
                               //   (the task's run budget), -KOS_ENOMEM (the run pool, or no
                               //   cleared run that long), -KOS_EMFILE (cap table). Translating
                               //   boards only.
    KOS_SYS_FRAME_MAP = 60,    // (frame cap, address-space cap, uintptr_t* va, KOS_MEM_*)
                               //   -> 0 with *va where the run was mapped, or -KOS_EPERM
                               //   without AUTH_MEMORY, -KOS_EINVAL (a null or misaligned va,
                               //   an address off a page boundary, an undefined flag),
                               //   -KOS_EFAULT (va), -KOS_EBADF on a cap that does not
                               //   resolve, -KOS_ENOMEM when the space cannot take the range
                               //   there, -KOS_EBUSY while another mapping of the run carries
                               //   another memory type, -KOS_EAGAIN when the page tables the
                               //   map needs pass the page budget of the space's task. *va 0
                               //   maps at the run's own address.
    KOS_SYS_FRAME_UNMAP = 61,  // (frame cap, address-space cap, virtual address) -> 0, or
                               //    -KOS_EBADF for an invalid cap, -KOS_EINVAL for a nontranslating space,
                               //    -KOS_EPERM for a range not mapped through KOS_SYS_FRAME_MAP.
                               //    Receivers using the unmapped range are woken with -KOS_EFAULT.
    KOS_SYS_THREAD_AFFINITY = 63, // (kos_thread_t, core mask or KOS_AFFINITY_QUERY) -> the mask the
                               //   thread now runs on (>= 0), or -KOS_EPERM (no allowed core or
                               //   another task's thread), -KOS_EINVAL (nonzero mask with no
                               //   machine core), -KOS_EBADF (bad or exited handle). Zero selects
                               //   the task default; a nonzero mask intersects the machine and
                               //   task grant and may include isolated cores; KOS_AFFINITY_QUERY
                               //   sets nothing. Above one kernel core only: the stub answers
                               //   -KOS_ENOSYS without trapping.
    KOS_SYS_CORE_CURRENT = 65, // () -> the kernel core the caller ran on; cannot fail. Above
                               //   one kernel core only: the stub answers 0 without trapping.
                               //   On armv8a and x86_64 kos_core_current reads a register the
                               //   kernel seats per core and does not make this call
    KOS_SYS_TASK_CORES = 67,   // (kos_task_t, KOS_TASK_NONE for the caller's) -> the task's core
                               //   grant, or -KOS_EBADF, -KOS_EPERM (a task the caller did not
                               //   create). Above one kernel core only, as above.
    KOS_SYS_REPLY_RECV = 68,   // (reply_cap, buf, kos_call_lens_pack(reply_len, recv_cap),
                               //   kos_reply_recv_opts* in-out) -> received bytes, or
                               //   -KOS_ENOTIFY (a notification and no message), -KOS_EINVAL,
                               //   -KOS_EFAULT, -KOS_EPERM, -KOS_EACCES, -KOS_EBADF,
                               //   -KOS_ESRCH, -KOS_EMFILE, -KOS_ENOTSUP, -KOS_EPIPE,
                               //   -KOS_ETIMEDOUT, -KOS_ECANCELED. A null opts replies and
                               //   receives nothing: 0, or -KOS_EBADF (bad / non-reply cap),
                               //   -KOS_ESRCH (stale caller), -KOS_EFAULT (bad buffer)
    KOS_SYS_NOTIFY_BADGE = 69, // (source notify_cap, bit, kos_cap_t* out) -> 0, or -KOS_E*:
                               //   EBADF, EINVAL (bit at or above 32, or a bad out-ptr),
                               //   EFAULT (out-ptr), EACCES (a badged source cannot mint),
                               //   EMFILE (caller's cap table), EOVERFLOW (the object's
                               //   reference count). MINTS a second name for the same object,
                               //   badged with `bit` and carrying the source's rights.
    KOS_SYS_IRQ_BIND_NOTIFY = 70, // (irq_cap, notify_cap) -> 0, or -KOS_E*: EBADF, EACCES (the
                               //   line's cap lacks WAIT, or the notification's lacks SIGNAL),
                               //   EPERM (the notification carries a line claimed on another
                               //   core), EBUSY (this line already signals something), EAGAIN
                               //   (a holding task's notification budget), EOVERFLOW (the
                               //   object's reference count is at its ceiling). ONE-WAY:
                               //   nothing detaches a live binding.
    KOS_SYS_TASK_WATCH = 72,   // (kos_task_t, notify_cap, ready_ep) -> 0, or -KOS_E*: EBADF
                               //   (stale task, or a cap naming nothing of its kind), EPERM
                               //   (not the creator), EACCES (notify cap lacks SIGNAL), EINVAL
                               //   (a ready_ep for a task whose declaration names its own).
                               //   Raises the notify cap's badge when the task ends, when it
                               //   is dead, its members' teardown done, and the first time a
                               //   member waits on ready_ep, at once for what happened before
                               //   the arming. KOS_CAP_NONE for notify_cap disarms.
    KOS_SYS_WINDOW_GET = 74,   // (index, struct kos_window* out) -> 0 with *out the index-th
                               //   window of the caller's spawn list, its base where the caller
                               //   reaches it, or -KOS_EINVAL (index past the list, or out),
                               //   -KOS_EFAULT (out).
    KOS_SYS_PORT_REG_WRITE = 75, // (base, offset, value) -> 0, or -KOS_EPERM (no port window of
                               //   the caller's covers base + offset), -KOS_EINVAL (a port the
                               //   chip does not keep for this write, a value outside its mask,
                               //   or past port 0xffff / a byte), -KOS_ENOSYS (an arch with no
                               //   ports).
    KOS_SYS_TASK_STATE = 76,   // (kos_task_t, int* status or NULL) -> enum kos_task_state bits,
                               //   with *status the status the task ended with where ENDED is
                               //   set and status is not NULL, or -KOS_EBADF (stale task),
                               //   -KOS_EPERM (not the creator), -KOS_EINVAL / -KOS_EFAULT
                               //   (status).
    KOS_SYS_THREAD_SET_PRIORITY = 77, // (priority) -> 0, or -KOS_EINVAL (outside
                               //   KICKOS_PRIO_MIN to KICKOS_PRIO_MAX), -KOS_EPERM (above the
                               //   calling task's priority ceiling). The caller's OWN base
                               //   priority; an inherited boost above it stays.
    KOS_SYS_PERIPH_DISABLE = 79, // (base) -> 0, -KOS_EPERM (caller holds no window at that base),
                                //   -KOS_EINVAL (no table entry), -KOS_ENOSYS (no backend).
                                //   KOS_SYS_PERIPH_ENABLE's undo, gated the same way
    KOS_SYS_WITNESS_UACCESS = 80, // RESERVED for the uaccess witness (<kickos/sys/witness.h>):
                                //   (op, uaddr, ns) -> the byte read, 0 after a write, or
                                //   -KOS_EPERM (a caller without the whole authority word),
                                //   -KOS_EINVAL (an unknown op), -KOS_ENOSYS (no address space
                                //   to leave). Any kernel not built for the witness answers
                                //   -KOS_EINVAL, as for an unknown number
    KOS_SYS_WITNESS_REWINDS = 81, // RESERVED for the rewind witness (<kickos/sys/witness.h>):
                                //   () -> how many times the switcher has moved a stacked PC
                                //   inside the user compare-and-swap's window back to its load
                                //   since boot. Any kernel not built for the witness answers
                                //   -KOS_EINVAL
    KOS_SYS_WORD_WAIT = 82,     // (uint32_t* word, expected, limit lo, limit hi) -> 1 woken
                                //   with a unit, 0 woken without one, -KOS_EAGAIN (the word is
                                //   not `expected`, nothing parked), -KOS_ETIMEDOUT (at once for
                                //   a timeout of 0 or a passed realtime deadline),
                                //   -KOS_ECANCELED, -KOS_EFAULT (a word the caller cannot write,
                                //   or whose reach it lost while parked), -KOS_EINVAL
                                //   (misaligned, not normal memory, the AMP share, a word threads
                                //   wait on as a mutex, or a limit hi the ABI does not define).
                                //   The limit is KOS_TIMEOUT_REALTIME's. A plain word: no owner,
                                //   no priority inheritance
    KOS_SYS_WORD_WAKE = 83,     // (uint32_t* word, n, units) -> k, the waiters woken: the
                                //   min(n, parked) highest, FIFO among equals, the first
                                //   min(units, k) handed a unit (their wait answers 1). Units no
                                //   waiter takes are added to bits 30..0, only once no waiter is
                                //   left, and KOS_WORD_WAITERS is cleared when none is. Refused
                                //   with nothing done: -KOS_EINVAL (units > n, or as the wait),
                                //   -KOS_EFAULT, and -KOS_EOVERFLOW (the count would carry out
                                //   of bit 30). Never a death point of a cancel
    KOS_SYS_ENDPOINT_BADGE = 84, // (source endpoint cap, badge, kos_cap_t* out) -> 0, or -KOS_E*:
                                //   EINVAL (badge KOS_BADGE_NONE, or a bad out-ptr), EFAULT
                                //   (out-ptr), EBADF, EACCES (the source lacks SIGNAL or HANDOUT,
                                //   as every badged copy does), EMFILE (caller's cap table),
                                //   EOVERFLOW (the
                                //   endpoint's reference count). MINTS a copy carrying `badge`,
                                //   with the source's SIGNAL and TRANSFER and nothing else
    KOS_SYS_TASK_CREATE = 85,   // (kos_program_t, struct kos_task_params*, kos_task_t* out) -> 0,
                                //   or -KOS_E*: EFAULT (the params, a string, the vector or the
                                //   out-pointer), EBADF (no program handle), ENOENT (no declared
                                //   instance of that name), EPERM (no KOS_AUTH_TASKS, an `init`
                                //   instance and a caller other than the init, a declared
                                //   authority bit, ceiling or core the caller does not hold, a
                                //   declared device without KOS_AUTH_MEMORY, line without
                                //   KOS_AUTH_IRQ or console without KOS_AUTH_CONSOLE, a line the
                                //   kernel owns, or the block, stack and windows as
                                //   KOS_SYS_THREAD_CREATE), EBUSY (the named instance is live, a
                                //   declared line or window held, the console handing over, or
                                //   the block mapped elsewhere with another memory type), ENOMEM
                                //   (every instance the caller may start is live, or a pool
                                //   full), EINVAL (rsv0 not 0, argv past KICKOS_ARGV_MAX bytes or
                                //   KICKOS_MAX_ARGC strings, a ready endpoint or a lines'
                                //   notification missing from the caps, windows passed to a
                                //   packaged driver's declaration, or the rest as a spawn),
                                //   EAGAIN (a declared line still retiring from its last holder,
                                //   an IRQ binding the last incarnation released still retiring,
                                //   or as KOS_SYS_THREAD_CREATE), EACCES, EOVERFLOW, ENOTSUP and
                                //   EMFILE as KOS_SYS_THREAD_CREATE. A packaged driver's
                                //   declaration publishes its console through the creator's
                                //   capability at `ready`, which keeps WAIT, then claims its
                                //   lines for the entry thread, bound to the notification at
                                //   `notify`, and seats its device windows on it. Starts the
                                //   instance's entry thread. With no instance named and none
                                //   startable, the refusal <kickos/sys.h> states.
    KOS_SYS_PROGRAM_FIND = 86,  // (char const* name, kos_program_t* out) -> 0, or -KOS_E*:
                                //   ENOENT (no program of that name), EFAULT (the name or the
                                //   out-pointer), EINVAL (a name past KICKOS_ARGV_MAX), EMFILE
                                //   (caller's cap table). MINTS a program capability with
                                //   TRANSFER
    KOS_SYS_CLOCK_REALTIME = 87,  // () -> Unix-epoch nanoseconds (u64, in registers; cannot
                                  //   fail): the monotonic clock plus the offset the last
                                  //   KOS_SYS_CLOCK_SET_REALTIME stored, none before it
    KOS_SYS_CLOCK_SET_REALTIME = 88 // (ns_lo, ns_hi) -> 0, or -KOS_EPERM (no KOS_AUTH_REALTIME).
                                    //   The wall clock reads ns from now on
};
// The raw console write's number, refused in an app as kos_kconsole_write is (<kickos/sys.h>).
#if defined(KICKOS_APP_IMAGE)
#pragma GCC poison KOS_SYS_KCONSOLE_WRITE
#endif

// How KOS_SYS_SHUTDOWN ends the system, or KOS_SYS_TERMINAL_OUTCOME every other end. REBOOT and
// BOOTLOADER drain the kernel console and return only where the chip has no such entry, the
// system still running.
enum kos_shutdown_outcome
{
    KOS_SHUTDOWN_HALT = 0,
    KOS_SHUTDOWN_REBOOT = 1,
    KOS_SHUTDOWN_BOOTLOADER = 2
};

// KOS_SYS_KCONSOLE_WRITE's flags: the write never waits for room, and is answered -KOS_ETIMEDOUT
// where it would.
#define KOS_KCONSOLE_NOWAIT 1u

// How KOS_SYS_THREAD_CANCEL ends a thread. A KILL is cooperative: a parked target is woken with
// -KOS_ECANCELED and the target ends at its next system call, so one that makes none is not
// reached. A SLAY is forced: the target runs no further instruction of its own and gets no
// cleanup window.
enum kos_cancel
{
    KOS_CANCEL_KILL = 0,
    KOS_CANCEL_SLAY = 1
};

// The mask KOS_SYS_THREAD_AFFINITY takes to read a thread's mask and set nothing. Bit 31 never
// names a core.
#define KOS_AFFINITY_QUERY 0x80000000u

// What KOS_SYS_TASK_STATE answers for the task a handle names.
enum kos_task_state
{
    KOS_TASK_LIVE = 1 << 0,  // it has members
    KOS_TASK_READY = 1 << 1, // a member has waited on the watched endpoint since it last emptied
    KOS_TASK_DEAD = 1 << 2,  // it is empty and every member's teardown is done: its death
    KOS_TASK_ENDED = 1 << 3  // its status is set and it takes no member; set by its death too
};

// What KOS_SYS_MEM_COUNT reads, each when the call runs.
enum kos_mem_count_id
{
    KOS_MEM_FRAMES_FREE = 0, // frames free in the pool, machine-wide
    KOS_MEM_SPACES_HELD = 1, // address spaces alive, machine-wide
    KOS_MEM_RANGES_FREE = 2  // free range slots in the caller's own task's space
};

// What KOS_SYS_AMP_COUNT reads: one of a node's window counts, as that node last wrote it.
enum kos_amp_count_id
{
    KOS_AMP_COUNT_TOOK = 0,         // messages it took
    KOS_AMP_COUNT_DEPTH = 1,        // takes refused on the far head's depth
    KOS_AMP_COUNT_DEPTH_RESET = 2,  // rings resynchronised after repeated depth refusals
    KOS_AMP_COUNT_TAIL_RESET = 3,   // reply rings whose far tail it stopped believing
    KOS_AMP_COUNT_LENGTH = 4,       // slots refused on the far length
    KOS_AMP_COUNT_PORT = 5,         // slots refused on the far port
    KOS_AMP_COUNT_WRONG_CLASS = 6,  // a reply on the call ring, or the reverse
    KOS_AMP_COUNT_SENT = 7,         // messages it published
    KOS_AMP_COUNT_SEND_REFUSED = 8, // sends it refused
    KOS_AMP_COUNT_REPLY_RESERVE = 9, // calls left unread for want of a reply slot
    KOS_AMP_COUNT_SERVICED = 10,    // doorbell services that drained its inboxes
    KOS_AMP_COUNT_REPLY_DROP = 11,  // replies taken and then refused by the tag validation
    KOS_AMP_COUNT_REPLY_UNSENT = 12, // answers whose bytes were lost
    KOS_AMP_COUNT_DELIVER_FAULT = 13 // arrivals it could not put in a local thread's buffer
};

/* Slots in ONE ring of an ordered pair. The reply-record band the thread pool reserves is sized
   by it, so a second spelling would size that band against a ring the window does not have. */
#define KOS_AMP_RING_SLOTS 4

/* The window layer's own two ports, which every node mints and no node binds. Every OTHER
   port is the partition's: CONFIG_KICKOS_AMP_PORTS names which node serves which, and
   <kickos/amp.h> is how an app names the capability it was handed for one. */
enum
{
    KOS_AMP_PORT_ECHO = 0, /* the payload comes back to the sender's reply port */
    KOS_AMP_PORT_REPLY = 1 /* a reply, routed to whatever local caller its tag names */
};

// The generic dispatch's answer for KOS_SYS_CALL_REG: retry through KOS_SYS_CALL. Outside the
// result range by construction, so it can never collide with a real answer.
#define KOS_CALL_REG_FALLBACK ((int32_t)0x80000000)

// Flags for KOS_SYS_IRQ_CLAIM. The trigger type is fixed at claim time and never changes for
// the binding's life.
enum kos_irq_claim_flags
{
    KOS_IRQ_EDGE = 0,      // default: latch-and-coalesce rearm (bare unmask)
    KOS_IRQ_LEVEL = 1 << 0 // rearm discards the latch first, then unmasks
};

// `flags` for the two calls that create a MAPPING of an arena block: KOS_SYS_MEM_SELF_GRANT
// and KOS_SYS_TASK_CREATE, for its block. They select the memory TYPE the region is committed with; access
// is always read-write and is not expressible here. An undefined bit is -KOS_EINVAL, never
// masked off.
// The flag belongs to the BLOCK: pass it identically to EVERY call that maps it, or two live
// mappings end up disagreeing about its type.
enum kos_mem_flags
{
    // Map the block Normal non-cacheable, for a block a bus master reads or writes. A chip whose
    // region descriptors carry no memory type and whose data cache sits over the arena REFUSES it
    // with -KOS_ENOTSUP; a chip with no cache in that path accepts it; a chip that TRANSLATES
    // answers from its page tables. The kernel's own copies into and out of the block, IPC
    // included, maintain the data cache around themselves, as a translating chip's mapping does,
    // so no line of the kernel's cacheable view of it is read in place of memory or evicted over
    // it.
    KOS_MEM_NOCACHE = 1u << 0
};
#define KOS_MEM_FLAGS_ALL (KOS_MEM_NOCACHE)

// KOS_SYS_BENCH operation IDs (benchmark images only). Append new values.
// KOS_BENCH_OP_DIST_PRINT's rows: the switch row alone, its window run with every vector register
// written before each switch.
#define KOS_BENCH_DIST_SWITCH_VEC 1

// Unknown operations return -KOS_EINVAL. PRINT operations use the kernel console.
// Operations marked AUTH_IRQ require KOS_AUTH_IRQ or return -KOS_EPERM.
enum kos_bench_op
{
    KOS_BENCH_OP_RESET = 0,       // ()          -> 0. Every distribution AND every phase.
    KOS_BENCH_OP_CYCCNT_HZ = 1,   // ()          -> rate of the counter the cycle ops read,
                                  //   which is not always the core clock, as a 64-BIT Hz
                                  //   count. 0 = no rate converts a reading, so report
                                  //   cycles alone.
    KOS_BENCH_OP_DIST_PRINT = 2,  // (rows)      -> switch sample count (the kernel prints one
                                  //   line per workload-fed distribution; a SWEPT one is
                                  //   printed by the op that filled it). rows
                                  //   KOS_BENCH_DIST_SWITCH_VEC prints the switch row alone,
                                  //   labelled as the dirty-vector variant; any other non-zero
                                  //   rows is refused.
    KOS_BENCH_OP_IRQ_SETUP = 3,   // (line)      -> 0, or -KOS_EBUSY if irq_attach refuses the
                                  //   line. AUTH_IRQ. Names the line every IRQ op below uses.
    KOS_BENCH_OP_IRQ_SWEEP = 4,   // (samples)   -> samples taken (the kernel prints the
                                  //   inject->entry row). 0 = the controller never raised it.
                                  //   AUTH_IRQ; samples above KOS_BENCH_SAMPLES_MAX refused.
    KOS_BENCH_OP_IRQ_WCASE = 5,   // (samples)   -> samples taken over every masked span, each
                                  //   span sampled in turn per round (the kernel prints one
                                  //   worst-case row per span). AUTH_IRQ; samples above
                                  //   KOS_BENCH_SAMPLES_MAX refused.
    KOS_BENCH_OP_PHASE_PRINT = 6, // ()          -> 0 (kernel prints the phase table)
    KOS_BENCH_OP_LOCK_PROBE = 7,  // ()          -> 0 (kernel prints one line: how far the
                                  //   lock distributions moved across three nested IrqLocks,
                                  //   and on which core)
    KOS_BENCH_OP_DOORBELL_PROBE = 9, // (core, rounds) -> rounds run, 0 where the calling
                                  //   thread may not run on that core, -KOS_EINVAL for a core
                                  //   this kernel does not schedule, which is how a caller
                                  //   walks the cores without being told how many there are.
                                  //   -KOS_ENOSYS at one kernel core. The kernel prints one
                                  //   line. AUTH_IRQ; rounds above KOS_BENCH_ROUNDS_MAX
                                  //   refused, the rounds running under one IrqLock.
    // The end-to-end span, raise to the woken userspace thread's first device read. ARM,
    // TARE and CLOSE are the WAITER's; the kernel refuses a close from any other thread.
    // RAISE injects on the line the ARM named and takes no authority of its own.
    KOS_BENCH_OP_E2E_ARM = 10,    // (irq_cap)   -> 0, -KOS_EBADF (no such cap) or -KOS_EACCES
                                  //   (the cap carries no KOS_CAP_WAIT). The span's line is
                                  //   the one that cap names.
    KOS_BENCH_OP_E2E_RAISE = 11,  // ()          -> 0, or -KOS_EAGAIN until the waiter has
                                  //   published its own park, which the caller retries.
    KOS_BENCH_OP_E2E_TARE = 12,   // ()          -> 0. Opens a span with no raise, so the
                                  //   close prices the instrument's own tail.
    KOS_BENCH_OP_E2E_CLOSE = 13,  // ()          -> 0, or -KOS_E* for a span that was not a
                                  //   wake (counted as dropped and reported)
    KOS_BENCH_OP_E2E_PRINT = 14,  // (asked)     -> 0 (kernel prints the probe line, the pass
                                  //   denominator and the two locality rows). `asked` is the
                                  //   sweep size the CALLER ran; the kernel echoes it beside
                                  //   the raises it let through and acts on it in no other
                                  //   way.
    KOS_BENCH_OP_SCHED_PRINT = 15, // (tag)      -> 0 (kernel prints the scheduler's report
                                  //   since the last reset, labelled with `tag`). -KOS_ENOSYS
                                  //   at one kernel core and in an image built without
                                  //   KICKOS_BENCH_SCHED, neither of which keeps the report.
    KOS_BENCH_OP_E2E_QUIET = 16,  // ()          -> 1 once every other kernel core runs its idle
                                  //   thread and no thread is on its way to any core, else 0.
                                  //   The raiser polls it before a pass's first raise.
    KOS_BENCH_OP_FASTPATH_WATCH = 17 // ()       -> 0 (kernel prints one line: the register-form
                                  //   call's taken fastpath entries since boot whose saved or
                                  //   restored DPFPU bank stood at 4 mod 8). -KOS_ENOSYS where
                                  //   no backend keeps that count, which is every one but RXv3.
};

// The largest count the counted ops admit. Raising either raises what a caller can make the
// kernel do in one syscall, the rounds under an IrqLock.
#define KOS_BENCH_SAMPLES_MAX 100u
#define KOS_BENCH_ROUNDS_MAX 64u

// The widest end-to-end span KOS_BENCH_OP_E2E_CLOSE accepts, in ns; a wider one is dropped.
#define KOS_BENCH_E2E_SPAN_MAX_NS 0xFFFFFFFFull

// What kos_recv_info.badge carries for a sender holding no badge, and so a badge no mint takes.
#define KOS_BADGE_NONE 0u

// Receive metadata: 8 bytes, aligned to 4. Plain sends have KOS_CAP_NONE;
// calls supply a one-shot reply cap that the receiver must reply to or close.
// Receiving without metadata rejects calls with -KOS_ENOTSUP.
struct kos_recv_info
{
    uint32_t badge;      // the badge of the capability the sender sent through, or
                         // KOS_BADGE_NONE for an unbadged one
    kos_cap_t reply_cap; // KOS_CAP_NONE for a plain send; else a one-shot CAP_REPLY handle
                         // in the receiver's table. Test it against KOS_CAP_NONE: a live
                         // reply cap can have bit 31 set, so no sign test works.
};
KOS_STATIC_ASSERT(sizeof(struct kos_recv_info) == 8, "kos_recv_info must stay 8 bytes (ABI)");

// Receive flags; unknown bits return -KOS_EINVAL.
// KOS_RECV_NO_INFO leaves info untouched and rejects calls with -KOS_ENOTSUP.
#define KOS_RECV_NO_INFO 0x1u

// Arguments for KOS_SYS_REPLY_RECV. ep selects the receive endpoint.
// notify supplies the accepted badge bits on input and consumed bits on output, over
// whatever notification the caller is bound to. Zero accepts none; unaccepted bits remain
// pending. Restore the accepted mask before each call because the kernel overwrites it.
struct kos_reply_recv_opts
{
    kos_cap_t ep;              // IN: the endpoint to receive on (needs CAP_WAIT)
    uint32_t flags;            // IN: KOS_RECV_NO_INFO, or 0
    uint32_t timeout_us;       // IN: relative microseconds, or KOS_TIMEOUT_NONE
    uint32_t notify;           // IN: accepted badge bits; zero accepts none
                               // OUT: consumed bits; unaccepted bits remain pending
    struct kos_recv_info info; // OUT: the arrival, whole-struct
};
KOS_STATIC_ASSERT(sizeof(struct kos_reply_recv_opts) == 24,
                  "kos_reply_recv_opts must stay 24 bytes (ABI)");
KOS_STATIC_ASSERT(offsetof(struct kos_reply_recv_opts, info) == 16,
                  "the nested kos_recv_info must sit at offset 16 (ABI)");

// Initialize receive options and clear outputs. No IRQ notifications are accepted
// by default; set notify before each call that should accept them.
static inline void kos_reply_recv_opts_init(struct kos_reply_recv_opts* o, kos_cap_t ep,
                                            uint32_t flags, uint32_t timeout_us)
{
    o->ep = ep;
    o->flags = flags;
    o->timeout_us = timeout_us;
    o->notify = 0u;
    o->info.badge = 0u;
    o->info.reply_cap = KOS_CAP_NONE;
}

// P-state selector for KOS_SYS_CPU_CLOCK_SET, NOT a raw Hz: the landed Hz is the syscall's
// return value. Carried as a plain u32 in the syscall register, so the width is the stable
// ABI: append new states, never reorder.
enum kos_pstate_e
{
    KOS_PSTATE_MAX = 0, // full PLL (the boot clock: XMC 144 / K64F 120 MHz)
    KOS_PSTATE_MID,     // a reduced locked-PLL / staged point (chip rounds to nearest)
    KOS_PSTATE_LOW      // deep power saving (crystal/RC direct or a low staged point)
};
// The ABI type is the u32 and not the enum: an enum's own width is implementation-defined before
// C23 (arm-none-eabi short-enums this one to a single byte; GNURX does not), and a fixed
// underlying type is not ISO C11. A caller wanting -Wswitch exhaustiveness back switches on
// `(enum kos_pstate_e)x`.
typedef uint32_t kos_pstate_t;

// Shared payload bound: send REJECTS a len above this; recv clamps its capacity to it.
#define KOS_EP_MSG_MAX 256

// A timeout is RELATIVE microseconds; KOS_TIMEOUT_NONE means no deadline. The fleet-wide
// timer floor is 20 us.
#define KOS_TIMEOUT_NONE UINT32_MAX

// The limit KOS_SYS_MUTEX_LOCK and KOS_SYS_WORD_WAIT take, two words (lo, hi). A hi of 0 makes lo
// a timeout as above. A hi carrying KOS_TIMEOUT_REALTIME makes the pair, that bit cleared, an
// absolute CLOCK_REALTIME deadline in Unix-epoch nanoseconds: a set of the wall clock while the
// caller waits moves it, so the wait ends when the wall clock reads it. Any other hi is refused
// -KOS_EINVAL.
#define KOS_TIMEOUT_REALTIME 0x80000000u

// KOS_SYS_CALL packs both message lengths into ONE argument slot so the fourth can carry the
// deadline. Nine bits each, both being bounded by KOS_EP_MSG_MAX.
#define KOS_CALL_LEN_BITS 9
#define KOS_CALL_LEN_MASK ((1u << KOS_CALL_LEN_BITS) - 1u)
KOS_STATIC_ASSERT((unsigned)KOS_EP_MSG_MAX < KOS_CALL_LEN_MASK,
                  "KOS_EP_MSG_MAX must stay strictly below the packed field's saturation value");

// Saturates at the field width: a masked 512 would arrive as 0 and become a silent zero-length
// call, while a saturated 511 still trips the kernel's send_len > KOS_EP_MSG_MAX refusal.
static inline uintptr_t kos_call_len_field(size_t len)
{
    if (len > (size_t)KOS_CALL_LEN_MASK)
    {
        return (uintptr_t)KOS_CALL_LEN_MASK;
    }
    return (uintptr_t)len;
}
static inline uintptr_t kos_call_lens_pack(size_t send_len, size_t recv_cap)
{
    return (kos_call_len_field(recv_cap) << KOS_CALL_LEN_BITS) | kos_call_len_field(send_len);
}
static inline size_t kos_call_lens_send(uintptr_t packed)
{
    return (size_t)(packed & KOS_CALL_LEN_MASK);
}
static inline size_t kos_call_lens_recv(uintptr_t packed)
{
    return (size_t)((packed >> KOS_CALL_LEN_BITS) & KOS_CALL_LEN_MASK);
}

// KOS_SYS_CALL_REG's payload budget, in 32-bit words and in bytes. Five is what an rv32
// psABI leaves after (nr, ep_cap, lens) claim a0-a2; the reply comes back in a1-a5.
#define KOS_CALL_REG_WORDS 5
#define KOS_CALL_REG_BYTES (KOS_CALL_REG_WORDS * 4)

// The counting-semaphore ceiling: bits 30..0 of its word. kos_sem_init refuses an initial count
// above it with -KOS_EINVAL, and a post at it is refused with -KOS_EOVERFLOW.
#define KOS_SEM_COUNT_MAX 0x7FFFFFFF

// 64-bit ARGUMENTS are passed as two uintptr_t halves, identically on 32-bit (ARM M-class)
// and 64-bit (sim) targets: never rely on uintptr_t being 64 bits. sleep_ns takes (lo, hi).
// A 64-bit RESULT comes back whole in the psABI's long-long return register pair, so it
// needs no out-pointer.
static inline uint32_t kos_u64_lo(uint64_t v)
{
    return (uint32_t)(v & 0xffffffffu);
}
static inline uint32_t kos_u64_hi(uint64_t v)
{
    return (uint32_t)(v >> 32);
}
static inline uint64_t kos_u64_join(uint32_t lo, uint32_t hi)
{
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}
// The limit pair (KOS_TIMEOUT_REALTIME) for the wall-clock deadline `unix_ns`, hi in the top
// word. A deadline past the 63 bits the pair holds is the latest one it holds.
static inline uint64_t kos_limit_realtime(uint64_t unix_ns)
{
    uint64_t const latest = ((uint64_t)1 << 63) - 1u;
    if (unix_ns > latest)
    {
        unix_ns = latest;
    }
    return ((uint64_t)KOS_TIMEOUT_REALTIME << 32) | unix_ns;
}

enum kos_policy
{
    KOS_POLICY_FIFO = 0,
    KOS_POLICY_RR = 1
};

// One entry of a spawn delegation list: hand the child a narrowed copy of the parent cap
// `source_cap`. A fresh child table has cap-gen 0, so the child's handle value EQUALS its
// index and is known a priori with no handoff.
//
// Table index of the FIRST delegated cap (i == 0) under DEFAULT placement: delegated cap
// i lands at KOS_SPAWN_DELEGATED_CAP0 + i.
#define KOS_SPAWN_DELEGATED_CAP0 KOS_CAP_FIRST_DYNAMIC
struct kos_cap_grant
{
    kos_cap_t source_cap; // a cap handle in the SPAWNING thread's table
    uint8_t rights_mask;  // subset of the source cap's rights (kos_cap_rights bits)
};

// What one window of a spawn's list reaches.
enum kos_window_kind
{
    // Device registers, R|W and never executable, inside a bus aperture the chip states, which
    // holds no RAM (-KOS_EINVAL), with one holder: overlapping a window a live thread holds, or
    // another device entry of the same list, is -KOS_EBUSY. Needs KOS_AUTH_MEMORY, and
    // KOS_AUTH_BUS_MASTER as well over a device its chip file marks `bus_master` (-KOS_EPERM).
    // Never a system call's buffer (-KOS_EFAULT). Takes no flag, and is what kos_periph_enable
    // and kos_periph_reg_write accept.
    KOS_WINDOW_DEVICE = 0,
    // A block the spawner's task reserved with kos_ram_alloc, or any part on whole granules of
    // an AMP partition's user share where the spawner is in root's task (<kickos/amp.h>), at
    // the address kos_window_get answers; a list names a block in one window at most
    // (-KOS_EINVAL). The block keeps one memory type wherever it is mapped: a window asking
    // another is -KOS_EBUSY.
    KOS_WINDOW_MEMORY = 1,
    // An I/O port range, base the first port and size the count, on an arch with ports
    // (-KOS_ENOTSUP elsewhere): inside an aperture the chip states (-KOS_EINVAL), one holder as
    // for a device window, and reached by the holder's own in and out instructions.
    KOS_WINDOW_PORTS = 2
};

// A memory window's flags; a device window takes none (-KOS_EINVAL).
enum kos_window_flags
{
    KOS_WINDOW_RO = 1 << 0,      // reads only: a write faults the thread
    KOS_WINDOW_UNCACHED = 1 << 1 // as KOS_MEM_NOCACHE; -KOS_ENOTSUP where the chip cannot
};

struct kos_window
{
    uintptr_t base;
    uint32_t size;
    uint8_t kind;  // enum kos_window_kind
    uint8_t flags; // enum kos_window_flags
};

// Thread-creation parameters (kernel allocates TCB + stack from a static pool).
enum kos_thread_flags
{
    // The thread's end by any cause, its entry's return, kos_exit or a cancel, ends its task as a
    // fault does, the task's status KOS_EXIT_FAULT, unless the task has already ended.
    KOS_THREAD_ESSENTIAL = 1u << 0,
    // The thread starts at its creator's base priority, and `prio` is not read.
    KOS_THREAD_INHERIT_PRIO = 1u << 1
};

struct kos_thread_params
{
    void (*entry)(void* arg);
    void* arg;
    char const* name;
    uint8_t prio;
    uint8_t policy;      // enum kos_policy
    uint8_t cap_count;   // number of entries in caps[]; under default placement they land
                         // at child indices 1..cap_count.
                         // Above KICKOS_MAX_SPAWN_GRANTS: -KOS_EINVAL. That bound is the
                         // spawn stager's, NOT the child table's ceiling.
    uint8_t flags;       // enum kos_thread_flags; a bit none names: -KOS_EINVAL
    uint32_t quantum_ns; // RR slice; 0 => none
    // The windows this thread holds, each its own and none its task's (0 => none), read
    // once at the spawn. More than KICKOS_MAX_THREAD_WINDOWS entries: -KOS_ENOMEM.
    // A region backend refuses (-KOS_EINVAL) a child whose data, windows and stack overlap in a
    // way its MPU cannot decide as the kernel does: any overlap on ARMv8-M.
    struct kos_window const* windows;
    uint16_t window_count;
    // The thread's capability-table width; 0 => its task's declared width. Its task's CAP_SLOTS
    // budget is charged it rounded up to whole chunks. Below KOS_CAP_FIRST_DYNAMIC + cap_count:
    // -KOS_EINVAL. Past what that budget has left: -KOS_EAGAIN. Wider than any table the system
    // seats: -KOS_EINVAL.
    uint16_t cap_width;
    void* stack_base;    // caller-owned thread stack; 0 => kernel default (KICKOS_USER_STACK_SIZE).
                         // Must be a block the CALLER'S TASK reserved with kos_ram_alloc, a
                         // sibling task's included. Under translation it must ALSO be
                         // reachable in the task the child joins. App static data is neither,
                         // nor is the AMP user share: -KOS_EPERM.
                         // -KOS_EINVAL unless 16-aligned; where the thread pointer is SP masked
                         // (armv6m, armv7m, rxv3) and the image declares a thread_local, also
                         // unless it is KICKOS_TLS_STRIDE-aligned and stack_size is exactly one
                         // stride.
    uint32_t stack_size; // size of the caller stack (bytes); ignored when stack_base == 0.
                         // -KOS_EINVAL unless a multiple of 16 and at least KICKOS_MIN_STACK_SIZE
                         // plus the carve: the thread's libc state (KICKOS_REENT_CARVE) and the
                         // image's thread_local block rounded to 16.
    struct kos_cap_grant const* caps; // optional caps to delegate to the child (0 => none)
    // Optional uint16_t-aligned destination indices, parallel to caps.
    // Null or zero entries use default placement (grant i -> KOS_SPAWN_DELEGATED_CAP0 + i).
    // The reserved indices below KOS_CAP_FIRST_DYNAMIC are refused.
    // Reject duplicate destinations or indices outside the child table with EINVAL
    // before creating the thread.
    uint16_t const* cap_dest;
    // Allowed child cores. Zero selects task defaults, excluding isolated cores.
    // Explicit masks intersect the machine and task grant; isolated cores require
    // an explicit bit. No machine core returns EINVAL; no granted core returns EPERM.
    // Ignored by single-core kernels.
    uint32_t core_mask;
    // Authority bits (kos_cap_authority KOS_AUTH_*) to seat as the child's authority word;
    // 0 => none. Only a thread that already holds each bit may pass it: narrows, never
    // widens, like a cap_grant mask. A bit no authority names: -KOS_EINVAL.
    uint32_t authority;
};

// sizeof is NOT assertable here: three pointers make it width-dependent (64 B on armv7m,
// 112 B on a 64-bit host). What is width-independent is the PACKING, and that is the part a
// new field would move silently: prio, policy, cap_count and flags fill the word behind `name`,
// cap_width the padding behind window_count, and core_mask and authority are the two 32-bit
// words the tail carries with no padding between.
// The spawn stager copies the whole struct onto the syscall stack, so a field that grows it is
// bytes of every trap red zone.
KOS_STATIC_ASSERT(sizeof(((struct kos_thread_params*)0)->core_mask) == 4,
                  "the core mask is a 32-bit word at every ABI that carries one");
KOS_STATIC_ASSERT(sizeof(((struct kos_thread_params*)0)->authority) == 4,
                  "the authority word is 32 bits at every ABI");
KOS_STATIC_ASSERT(offsetof(struct kos_thread_params, authority)
                      == offsetof(struct kos_thread_params, core_mask) + 4,
                  "authority must sit immediately behind core_mask (ABI)");
KOS_STATIC_ASSERT(offsetof(struct kos_thread_params, cap_width)
                      == offsetof(struct kos_thread_params, window_count) + 2,
                  "cap_width must sit in the padding behind window_count (ABI)");
KOS_STATIC_ASSERT(offsetof(struct kos_thread_params, cap_count)
                      == offsetof(struct kos_thread_params, policy) + 1,
                  "cap_count must sit immediately behind policy (ABI)");
KOS_STATIC_ASSERT(offsetof(struct kos_thread_params, flags)
                      == offsetof(struct kos_thread_params, cap_count) + 1,
                  "flags must sit immediately behind cap_count (ABI)");
KOS_STATIC_ASSERT(offsetof(struct kos_thread_params, quantum_ns)
                      == offsetof(struct kos_thread_params, prio) + 4,
                  "prio, policy, cap_count and flags must share one word (ABI)");

// kos_task_create's parameters. Each pointer is read once, at the call.
struct kos_task_params
{
    // NULL-terminated, at most KICKOS_MAX_ARGC strings and KICKOS_ARGV_MAX bytes with their NULs,
    // copied to the entry thread's stack. NULL: { the instance's name, NULL }.
    char const* const* argv;
    // A block the caller reserved, granted to every member, or NULL; `flags` its kos_mem_flags.
    void* block;
    uint32_t size;
    uint32_t flags;
    // The declared instance to start, or NULL for the lowest-numbered one the caller may start
    // that is not live.
    char const* instance;
    // Delegated into the entry thread's table and placed as kos_thread_params' are.
    struct kos_cap_grant const* caps;
    uint16_t const* cap_dest;
    // KOS_WINDOW_* windows of the entry thread, admitted as kos_thread_params' are.
    struct kos_window const* windows;
    // The entry thread's stack, as kos_thread_params' stack_base, or NULL for the kernel's.
    void* stack;
    uint32_t stack_size;
    uint16_t window_count;
    uint8_t cap_count;
    uint8_t rsv0; // 0, else -KOS_EINVAL
};

// What the entry thread's argument points at, on its own stack: argv[argc] is NULL and the
// strings follow the vector.
struct kos_task_args
{
    uintptr_t argc;
    uintptr_t decl;  // the declaration's index in its image
    uintptr_t block; // the shared block kos_task_params named, or 0
    char* argv[];
};

#endif
