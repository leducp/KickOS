// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The arch-independent syscall table + dispatch. The arch entry (sim
// trampoline / ARM SVC handler) reads the number + args and calls
// syscall_dispatch(); the result is delivered back to the caller frame.
// Kernel objects addressable from userspace (semaphores, threads) live in
// static pools referenced by small integer handles: no pointers cross the
// boundary.

#include <kickos/irq_route.h>
#include <kickos/arch/arch.h>
#include <kickos/arch/doorbell_cells.h> // arch_doorbell_core: which matrix row this core writes
#include <kickos/aspace.h>
#include <kickos/bench.h>
#include <kickos/cap.h>
#include <kickos/config.h>
#include <kickos/grant.h>
#include <kickos/instance.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/time.h>
#include <kickos/kernel.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>
#include <kickos/ktrace.h>
#include <kickos/ramown.h>
#include <kickos/console_tx.h>
#include <kickos/domain.h>
#include <kickos/task.h>

#include <kickos/sys/abi.h>
#include <kickos/sys/abi_probe.h>

#include "syscall_internal.h"

#include <span>

namespace kickos
{
    // syscall_dispatch answers 8 bytes on every target and the userspace stub narrows that
    // to a fixed 4, so the byte-count producers must already be 4 bytes here. Matching
    // static_assert in user/src/syscall_stubs.cc.
    static_assert(sizeof(endpoint_send(0, 0, 0, 0)) == 4, "must be exactly 4 bytes");
    static_assert(sizeof(endpoint_reply_recv(0, 0, 0, 0)) == 4, "must be exactly 4 bytes");
    static_assert(sizeof(endpoint_call(0, 0, 0, 0, 0)) == 4, "must be exactly 4 bytes");

    namespace
    {
        // Max yield passes kos_console_publish waits for the in-flight chip-writer count
        // to reach 0 before declaring a stuck writer.
        constexpr uint32_t CONSOLE_PUBLISH_DRAIN_MAX = KICKOS_POLL_SPIN_MAX;

        // A minting syscall's out-pointer, checked BEFORE the object is created: a mint
        // that cannot deliver its handle leaves an object nothing can name or close. The
        // kernel writes it privileged, so an unprivileged caller must own it. Serves the
        // THREAD mint too: kos_thread_t and kos_cap_t are different codecs, both 32-bit.
        int cap_out_check(uintptr_t out)
        {
            if (out == 0 or (out & (alignof(uint32_t) - 1)) != 0)
            {
                return -KOS_EINVAL; // null or misaligned out-ptr
            }
            if (not user_writable_ok(out, sizeof(uint32_t)))
            {
                return -KOS_EFAULT; // out-ptr not owned by the caller
            }
            return 0;
        }

        // Nothing is written on failure: the stub seated its codec's NONE before trapping,
        // so the sys.h "always written" guarantee already holds.
        //
        // cap_out_check proved this word writable and 4-aligned, so one granule holds all of
        // it and a refusal here moves NO byte: the mapping went away since that check. The
        // capability stays installed and unnameable until its holder dies.
        uint64_t cap_out_deliver(uintptr_t out, int rc, uint32_t handle)
        {
            if (rc == 0
                and not kaccess_to_user(user_space_of(sched::current()), out, &handle,
                                        sizeof(handle)))
            {
                rc = -KOS_EFAULT;
            }
            return static_cast<uint64_t>(rc);
        }

        // The console's user buffer, moved through the funnel a chunk at a time.
        // kconsole_write streams privileged whatever it is handed, so a user pointer
        // reaching it names whatever the RUNNING process holds at that address.
        //
        // Answers the bytes that REACHED the console. No lock spans the chunks, so a granule
        // the entry proved readable can be unmapped before a later chunk reaches it, and the
        // walk stops there.
        //
        // noinline is load-bearing for user_panic's reason below: the chunk must not widen
        // syscall_dispatch's frame.
        __attribute__((noinline)) size_t console_write_user(uintptr_t buf, size_t len)
        {
            char chunk[64];
            struct arch_aspace* const space = user_space_of(sched::current());
            size_t done = 0;
            while (done < len)
            {
                size_t n = len - done;
                if (n > sizeof(chunk))
                {
                    n = sizeof(chunk);
                }
                if (not kaccess_from_user(chunk, space, buf + done, n))
                {
                    break;
                }
                // STOPS AT THE FIRST CHUNK THE CONSOLE DID NOT TAKE. A line longer than this
                // buffer is several inserts, and under pressure the ring can refuse one and
                // accept the next: carrying on would put a HOLE in the middle of a line whose
                // tail arrived. The caller is told how much landed and decides what to do,
                // which is what a short write is for.
                if (kconsole_write(chunk, n) == 0)
                {
                    break;
                }
                done += n;
            }
            return done;
        }

        // noinline is load-bearing: the message buffer must not widen syscall_dispatch's
        // frame, which sits on the CALLING thread's stack and is sized by
        // KICKOS_MIN_STACK_SIZE against the deepest ordinary dispatch.
        __attribute__((noinline, noreturn)) void user_panic(uintptr_t msg)
        {
            char buf[64];
            buf[0] = '\0';
            struct arch_aspace* const space = user_space_of(sched::current());
            // A privileged caller passes user_readable_ok wholesale, so null must be
            // rejected here and not by the per-byte check.
            if (msg != 0)
            {
                // EACH source byte is checked before the privileged copy dereferences it:
                // the kernel must neither fault on a bad message pointer nor leak another
                // domain's page through it. That BOUNDS the walk at the first unreachable
                // byte, so a string with no NUL stops there.
                size_t i = 0;
                for (; i + 1 < sizeof(buf); i++)
                {
                    if (not user_readable_ok(msg + i, 1))
                    {
                        break;
                    }
                    if (not kaccess_from_user(&buf[i], space, msg + i, 1))
                    {
                        break; // readable a moment ago, gone now: truncate here
                    }
                    if (buf[i] == '\0')
                    {
                        break;
                    }
                    // This message prints after the kernel's trusted "KERNEL PANIC: "
                    // prefix, so no control byte may reach the console: a newline lets the
                    // caller continue on fresh lines that read as kernel output. Every such
                    // byte is REPLACED, so the message is not cut short at the first one.
                    unsigned char const c = static_cast<unsigned char>(buf[i]);
                    if (c < 0x20u or c == 0x7Fu)
                    {
                        buf[i] = '?';
                    }
                }
                buf[i] = '\0';
                // <kickos/sys.h> promises a visible truncation. The marker OVERWRITES kept
                // bytes, so buf's size is unchanged. The probe byte is the first one
                // dropped: unreadable there means nothing was dropped.
                if (i + 1 == sizeof(buf) and user_readable_ok(msg + i, 1))
                {
                    char probe = '\0';
                    // A refused probe leaves it NUL, so no marker is written: the kernel
                    // cannot claim a truncation it could not read.
                    if (kaccess_from_user(&probe, space, msg + i, 1) and probe != '\0')
                    {
                        buf[i - 3] = '.';
                        buf[i - 2] = '.';
                        buf[i - 1] = '.';
                    }
                }
            }
            if (buf[0] == '\0')
            {
                kpanic(diag::kUserPanicNoMsg);
            }
            kpanic(buf);
        }
    }
}

using namespace kickos;

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
namespace
{
    uint16_t syscall_tid()
    {
        Thread* c = sched::current();
        if (c == nullptr)
        {
            return static_cast<uint16_t>(trace::TRACE_NO_THREAD);
        }
        return c->id;
    }

    // RAII SYSCALL_ENTER/EXIT bracket. KOS_SYS_EXIT switches away permanently inside the
    // dispatch, so its destructor never runs and it is recorded as ENTER-only (the decoder
    // handles the missing EXIT).
    struct SyscallTrace
    {
        uint16_t tid;
        uint16_t nr;
        SyscallTrace(uint16_t t, uint16_t n) : tid(t), nr(n)
        {
            ktrace_syscall_enter(tid, nr);
        }
        ~SyscallTrace()
        {
            ktrace_syscall_exit(tid, nr);
        }
    };
}
#define KTRACE_SYSCALL_SCOPE(nr) SyscallTrace _kt_syscall(syscall_tid(), static_cast<uint16_t>(nr))
#else
#define KTRACE_SYSCALL_SCOPE(nr) do { } while (0)
#endif

namespace
{
    uint64_t syscall_body(uintptr_t nr, uintptr_t a0, uintptr_t a1,
                          uintptr_t a2, uintptr_t a3);
}

// THE death point of a cancelled thread. A cancel breaks whatever park the target is in, so it
// returns to userspace with -KOS_ECANCELED and gets ONE window to clean up over memory it already
// holds; the next time it asks the kernel for anything, it ends here instead. Checked on ENTRY
// and never on exit, which would pre-empt that window.
extern "C" uint64_t syscall_dispatch(uintptr_t nr,
                                     uintptr_t a0, uintptr_t a1,
                                     uintptr_t a2, uintptr_t a3)
{
    Thread* const caller = sched::current();
    if (caller != nullptr and caller->cancel_kind != CANCEL_NONE and not caller->dying)
    {
        sched::exit_current(KOS_EXIT_CANCELLED, sched::EXIT_RETURN); // noreturn
    }
    return syscall_body(nr, a0, a1, a2, a3);
}

namespace
{
uint64_t syscall_body(uintptr_t nr,
                      uintptr_t a0, uintptr_t a1,
                      uintptr_t a2, uintptr_t a3)
{
    KTRACE_SYSCALL_SCOPE(nr);
    switch (nr)
    {
        case KOS_SYS_KCONSOLE_WRITE:
        {
            // Explicit (buf, len): the kernel must never strlen a user pointer. Clamp len,
            // then bound buf against the caller's memory: the kernel reads buf privileged,
            // so an unbounded buffer would launder another domain's arena page out through
            // the console. Reject => wrote nothing.
            constexpr size_t MAX_CONSOLE_WRITE = 4096;
            size_t len = static_cast<size_t>(a1);
            if (len > MAX_CONSOLE_WRITE)
            {
                len = MAX_CONSOLE_WRITE;
            }
            if (not user_readable_ok(a0, len))
            {
                // Cross-domain / bad buffer: write nothing. Negative code, NOT 0: a
                // len-0 write legitimately returns 0, so 0 must not double as reject.
                return static_cast<uint64_t>(-KOS_EFAULT);
            }
            // A short count where a granule went away mid-stream.
            return console_write_user(a0, len);
        }
        case KOS_SYS_YIELD:
        {
            sched::yield();
            return 0;
        }
        case KOS_SYS_SLEEP_NS:
        {
            ktime_sleep_ns(kos_u64_join(static_cast<uint32_t>(a0),
                                        static_cast<uint32_t>(a1)));
            return 0;
        }
        case KOS_SYS_SEM_CREATE:
        {
            int rc = cap_out_check(a1);
            if (rc != 0)
            {
                return static_cast<uint64_t>(rc);
            }
            uint32_t h = KCAP_INVALID;
            rc = sem_create(static_cast<int>(a0), &h);
            return cap_out_deliver(a1, rc, h);
        }
        case KOS_SYS_HANDLE_CLOSE:
        {
            // Type-agnostic close: drop THIS thread's cap (a cap knows its own type).
            // Refcounted: the object is freed only at the last close.
            IrqLock lock;
            return static_cast<uint64_t>(handle_close(sched::current(), static_cast<uint32_t>(a0)));
        }
        case KOS_SYS_SEM_WAIT:
        {
            // Resolve and use under one lock (sem_wait/sem_post nest their own): a
            // concurrent close could otherwise free the slot between resolve and use.
            IrqLock lock;
            int err = 0;
            Semaphore* s = static_cast<Semaphore*>(
                cap_resolve_e(sched::current(), static_cast<uint32_t>(a0), CapType::CAP_SEM, CAP_WAIT, &err));
            if (s == nullptr)
            {
                return static_cast<uint64_t>(-err); // EBADF (bad/closed cap) or EPERM (no WAIT right)
            }
            sem_wait(s);
            return 0;
        }
        case KOS_SYS_SEM_POST:
        {
            IrqLock lock;
            int err = 0;
            Semaphore* s = static_cast<Semaphore*>(
                cap_resolve_e(sched::current(), static_cast<uint32_t>(a0), CapType::CAP_SEM, CAP_SIGNAL, &err));
            if (s == nullptr)
            {
                return static_cast<uint64_t>(-err); // EBADF (bad/closed cap) or EPERM (no SIGNAL right)
            }
            if (not sem_post(s))
            {
                return static_cast<uint64_t>(-KOS_EOVERFLOW); // count already at the ceiling
            }
            return 0;
        }
        case KOS_SYS_MUTEX_CREATE:
        {
            int rc = cap_out_check(a0);
            if (rc != 0)
            {
                return static_cast<uint64_t>(rc);
            }
            uint32_t h = KCAP_INVALID;
            rc = mutex_create(&h);
            return cap_out_deliver(a0, rc, h);
        }
        case KOS_SYS_MUTEX_LOCK:
        {
            // mutex_lock takes its OWN lock for the acquire/park and releases it before the
            // resume barrier and the wait_result read; a lock spanning the call would
            // reintroduce the stale read on ARM. The resolve-to-call window needs none, the
            // caller's own cap pinning the mutex. need == 0: possession is the authority.
            Mutex* m;
            int err = 0;
            {
                IrqLock lock;
                m = static_cast<Mutex*>(
                    cap_resolve_e(sched::current(), static_cast<uint32_t>(a0), CapType::CAP_MUTEX, 0, &err));
            }
            if (m == nullptr)
            {
                return static_cast<uint64_t>(-err); // -KOS_EBADF (need == 0, so never EPERM here)
            }
            // 0 / -KOS_EOWNERDEAD (HELD, owner died) / -KOS_EDEADLK (NOT held). EOWNERDEAD is
            // negative but still an ACQUIRE: the wrapper decl documents the held-vs-not caveat.
            return static_cast<uint64_t>(mutex_lock(m));
        }
        case KOS_SYS_MUTEX_UNLOCK:
        {
            IrqLock lock;
            int err = 0;
            Mutex* m = static_cast<Mutex*>(
                cap_resolve_e(sched::current(), static_cast<int>(a0), CapType::CAP_MUTEX, 0, &err));
            if (m == nullptr)
            {
                return static_cast<uint64_t>(-err); // -KOS_EBADF (bad cap)
            }
            return static_cast<uint64_t>(mutex_unlock(m)); // 0, or -KOS_EPERM if not owner (no panic)
        }
        case KOS_SYS_ENDPOINT_CREATE:
        {
            int rc = cap_out_check(a0);
            if (rc != 0)
            {
                return static_cast<uint64_t>(rc);
            }
            uint32_t h = KCAP_INVALID;
            rc = endpoint_create(&h);
            return cap_out_deliver(a0, rc, h);
        }
        case KOS_SYS_AMP_ENDPOINT_CREATE:
        {
            int rc = cap_out_check(a2);
            if (rc != 0)
            {
                return static_cast<uint64_t>(rc);
            }
            uint32_t h = KCAP_INVALID;
            rc = amp_endpoint_create(static_cast<uint32_t>(a0), static_cast<uint32_t>(a1), &h);
            return cap_out_deliver(a2, rc, h);
        }
        case KOS_SYS_SEND:
        {
            // No dispatch IrqLock: endpoint_send takes and releases its own around the
            // park, and a spanning caller lock would livelock ARM (design section 3).
            return static_cast<uint64_t>(
                endpoint_send(static_cast<uint32_t>(a0), a1, static_cast<size_t>(a2),
                              KOS_TIMEOUT_NONE));
        }
        case KOS_SYS_SEND_TIMED:
        {
            return static_cast<uint64_t>(
                endpoint_send(static_cast<uint32_t>(a0), a1, static_cast<size_t>(a2),
                              static_cast<uint32_t>(a3)));
        }
        case KOS_SYS_CALL:
        {
            // No dispatch IrqLock, as for SEND/RECV: a spanning caller lock would keep
            // BASEPRI raised across the resume barrier and livelock ARM.
            return static_cast<uint64_t>(
                endpoint_call(static_cast<uint32_t>(a0), a1, static_cast<size_t>(a2),
                              static_cast<size_t>(a3), KOS_TIMEOUT_NONE));
        }
        case KOS_SYS_CALL_REG:
        {
            // Reached only when the trap-handler fastpath declined, or this backend has
            // none. The register payload never arrives: dispatch is handed five words and
            // the request occupies more, so the retry re-issues from the stub's own copy.
            return static_cast<uint64_t>(static_cast<uintptr_t>(
                static_cast<uint32_t>(KOS_CALL_REG_FALLBACK)));
        }
        case KOS_SYS_CALL_TIMED:
        {
            // a2 carries both lengths so a3 can carry the deadline. endpoint_call is the
            // sole validator of them: the stub only saturates, so an oversize length still
            // arrives here out of range.
            return static_cast<uint64_t>(
                endpoint_call(static_cast<uint32_t>(a0), a1, kos_call_lens_send(a2),
                              kos_call_lens_recv(a2), static_cast<uint32_t>(a3)));
        }
        case KOS_SYS_REPLY_RECV:
        {
            return static_cast<uint64_t>(endpoint_reply_recv(static_cast<uint32_t>(a0), a1, a2,
                                                             a3));
        }
        case KOS_SYS_REPLY:
        {
            // Does not block the replier, so it does its whole job under endpoint_reply's
            // own lock.
            return static_cast<uint64_t>(
                endpoint_reply(static_cast<uint32_t>(a0), a1, static_cast<size_t>(a2)));
        }
        case KOS_SYS_CONSOLE_PUBLISH:
        {
            // Hand the console UART to a userspace driver named by an endpoint cap.
            // AUTH_CONSOLE, its own bit and not shutdown's. See the handover design (D3).
            Thread* c = sched::current();
            if (not cap_check_authority(c, AUTH_CONSOLE))
            {
                return static_cast<uint64_t>(-KOS_EPERM);
            }
            int handle = -1;
            {
                IrqLock lock;
                // Resolve to the GLOBAL gen-encoded handle, NOT the pool index.
                // cap_lookup validates the cap-gen; type and object liveness are re-checked
                // here. Any rights: the publish is identity-only.
                CapEntry* e = cap_lookup(c, static_cast<uint32_t>(a0));
                if (e == nullptr or e->type != static_cast<uint8_t>(CapType::CAP_ENDPOINT)
                    or kernel().endpoints.resolve(e->obj) == nullptr)
                {
                    return static_cast<uint64_t>(-KOS_EBADF); // bad / non-endpoint / stale cap
                }
                handle = e->obj;
                // Must precede the relinquish below: this is the only remaining step that
                // can fail, and a refusal has to leave a working console behind. Children
                // spawned after this get slot 0 via cap_install_defaults.
                if (not cap_console_publish(c, handle))
                {
                    return static_cast<uint64_t>(-KOS_EOVERFLOW); // endpoint refcount ceiling
                }
                // Refuses every NEW kernel chip writer and gives up the ring, WITHOUT
                // handing the device over: a writer already counted below may be mid-message
                // with no buffered path left, and it finishes synchronously here.
                console_handover_begin();
            }
            // Drains, with the lock RELEASED, every chip writer counted before the refusal
            // above. Converges because HANDING_OFF admits no new ones. A bare busy-spin here
            // LIVELOCKS under strict priority: a writer preempted mid arch_console_write_sync
            // can only finish once rescheduled and may be LOWER priority than this publisher,
            // hence the drop to the minimum real priority plus a yield each pass.
            Thread* pub = sched::current();
            uint8_t const saved_prio = pub->prio;
            // RE-ENTERED for the re-seat alone. The lock above was released on purpose and
            // must stay released across the yield below, so the bracket covers the ready-list
            // move and nothing else.
            {
                IrqLock lock;
                sched::set_prio(pub, KICKOS_PRIO_MIN);
            }
            uint32_t guard = 0;
            while (console_chip_writers() != 0)
            {
                sched::yield();
                guard = guard + 1;
                if (guard >= CONSOLE_PUBLISH_DRAIN_MAX)
                {
                    kpanic(diag::kPublishNoDrain);
                }
            }
            {
                IrqLock lock;
                sched::set_prio(pub, saved_prio);
            }
            console_owner_set_user(); // must be LAST, and strictly after the drain
            return 0;
        }
        case KOS_SYS_CPU_CLOCK_SET:
        {
            // AUTH_PSTATE: it mutates SystemCoreClock, retimes every thread's SysTick basis and
            // moves the shared console baud. Out of the -KOS_E* scheme: it returns a u64 Hz whose
            // 0 sentinel already means cannot/unsupported/not-permitted.
            Thread* c = sched::current();
            if (not cap_check_authority(c, AUTH_PSTATE))
            {
                return 0;
            }
            return cpu_clock_set(static_cast<kos_pstate_t>(a0));
        }
        case KOS_SYS_THREAD_CREATE:
        {
            // Checked BEFORE the child is created: a spawn that cannot deliver its handle
            // leaves a thread nothing can name or kill.
            int rc = cap_out_check(a1);
            if (rc != 0)
            {
                return static_cast<uint64_t>(rc);
            }
            kos_thread_t h = KOS_THREAD_NONE;
            rc = thread_create_call(reinterpret_cast<kos_thread_params const*>(a0), &h);
            return cap_out_deliver(a1, rc, h);
        }
        case KOS_SYS_THREAD_KILL:
        {
            // UNGATED by authority, gated by parenthood inside (syscall_thread.cc).
            return static_cast<uint64_t>(thread_kill(static_cast<kos_thread_t>(a0)));
        }
        case KOS_SYS_TASK_CREATE:
        {
            // A task handle spends the whole word, so the status is the return value and
            // the handle rides an out-parameter, checked BEFORE the group exists.
            int rc = cap_out_check(a2);
            if (rc != 0)
            {
                return static_cast<uint64_t>(rc);
            }
            uint32_t attr = 0u;
            if (not mem_flags_to_attr(a3, &attr))
            {
                return static_cast<uint64_t>(-KOS_EINVAL);
            }
            kos_task_t h = KOS_TASK_NONE;
            rc = task_create_call(reinterpret_cast<void*>(a0), static_cast<size_t>(a1), attr,
                                  &h);
            return cap_out_deliver(a2, rc, h);
        }
        case KOS_SYS_TASK_KILL:
        {
            // UNGATED by authority, gated by creatorship inside (syscall_thread.cc).
            return static_cast<uint64_t>(task_kill(static_cast<kos_task_t>(a0)));
        }
        case KOS_SYS_THREAD_JOIN:
        {
            // Blocks, so no dispatch IrqLock. Parenthood-gated inside.
            return static_cast<uint64_t>(
                thread_join(static_cast<kos_thread_t>(a0), static_cast<uint32_t>(a1)));
        }
        case KOS_SYS_THREAD_SET_AFFINITY:
        {
            return static_cast<uint64_t>(
                thread_set_affinity(static_cast<kos_thread_t>(a0), static_cast<uint32_t>(a1)));
        }
        case KOS_SYS_TASK_SCHED_GRANT:
        {
            return static_cast<uint64_t>(
                task_sched_grant(static_cast<kos_task_t>(a0), static_cast<uint8_t>(a1),
                                 static_cast<uint32_t>(a2)));
        }
        case KOS_SYS_THREAD_SLAY:
        {
            // Blocks, so no dispatch IrqLock. Parenthood-gated inside, reaching exactly the
            // set kill reaches.
            return static_cast<uint64_t>(
                thread_slay(static_cast<kos_thread_t>(a0), static_cast<uint32_t>(a1)));
        }
        case KOS_SYS_TASK_SLAY:
        {
            // Blocks. Creatorship-gated inside.
            return static_cast<uint64_t>(
                task_slay(static_cast<kos_task_t>(a0), static_cast<uint32_t>(a1)));
        }
        case KOS_SYS_WAIT_LAST:
        {
            return static_cast<uint64_t>(thread_wait_last());
        }
        case KOS_SYS_EXIT:
        {
            Thread* c = sched::current();
            // Root's exit ends the SYSTEM, this being the image's completion and its status
            // the image's own: hence the shutdown authority gate below. Not
            // sched::exit_current, which would report that status to nobody and leave root's
            // slot EXITED but unreclaimable (ThreadPool::alloc retires ROOT_INDEX). A
            // kernel-internal death of root, a fault or a group cancel, deliberately does
            // leave the system running (user/apps/common/rootgone).
            if (kernel().threads.is_root(c))
            {
                if (not cap_check_authority(c, AUTH_SYSTEM))
                {
                    // Cannot be reported: kos_exit is noreturn and _exit spins after it, so
                    // a returned refusal would loop root forever with nothing on the wire.
                    kpanic(diag::kRootExitRefused);
                }
                kickos_terminate(static_cast<int>(a0)); // noreturn
            }
            sched::exit_current(static_cast<int>(a0), sched::EXIT_RETURN); // noreturn
            return 0;
        }
        case KOS_SYS_SHUTDOWN:
        {
            // AUTH_SYSTEM: ends the system through the shared terminal path.
            Thread* c = sched::current();
            if (not cap_check_authority(c, AUTH_SYSTEM))
            {
                return static_cast<uint64_t>(-KOS_EPERM);
            }
            kickos_terminate(static_cast<int>(a0)); // noreturn
            return 0;
        }
#if defined(KICKOS_ENABLE_SELFTEST)
        case KOS_SYS_REBOOT:
        {
            // AUTH_SYSTEM, fused with shutdown.
            Thread* c = sched::current();
            if (not cap_check_authority(c, AUTH_SYSTEM))
            {
                return static_cast<uint64_t>(-KOS_EPERM);
            }
            console_tx_flush_sync(); // empties the ring only
            arch_console_flush_sync(); // FIFO and shift register: a byte here outruns the reset
            return static_cast<uint64_t>(arch_reboot());
        }
        case KOS_SYS_IRQ_INJECT:
        {
            // Test scaffolding, compiled out of the production ABI. Never
            // KICKOS_UNREACHABLE a user-supplied number: that would let a user halt the
            // kernel. Not gated on the CALLER: this simulates a DEVICE firing, and selftest
            // injects it from an unprivileged thread at authority 0. The gate is on the LINE
            // instead, the narrower unit.
            int irq = static_cast<int>(a0);
            if (irq < 0 or irq >= KICKOS_MAX_IRQ)
            {
                return static_cast<uint64_t>(-KOS_EINVAL); // bad irq line
            }
            // THE SAME REFUSAL irq_claim MAKES: a line the kernel drives (the tick, console
            // TX, the doorbell) is not a device a caller could stand in for, and raising one
            // reaches kernel state no capability named. Unconditional, as at the claim: every
            // in-tree injector uses a soft-only or a selftest-base line.
            if (arch_irq_line_kernel_owned(irq))
            {
                return static_cast<uint64_t>(-KOS_EPERM);
            }
            // The image-wide masked and pending words are read-modify-written here, and the
            // backend's own bracket excludes this core's handler alone.
            {
                IrqLock lock;
                arch_irq_inject(irq);
            }
            return 0;
        }
        case KOS_SYS_GUARD_ADDR:
        {
            return arch_mpu_probe_addr();
        }
        case KOS_SYS_IPC_FAST_TAKEN:
        {
            return ipc_fast_taken_count();
        }
        case KOS_SYS_IRQ_SPURIOUS:
        {
            return static_cast<uint64_t>(irq_spurious_count());
        }
#if defined(KICKOS_ENABLE_SELFTEST) && defined(__riscv)
        case KOS_SYS_NEST_WITNESS:
        {
            // Test scaffolding, and a plain counter read: a print HERE would put the
            // console writer inside the syscall red zone, so the caller prints. Not
            // privilege-gated, and not range-checked beyond what the accessor does: an
            // unknown selector answers KOS_NEST_UNSET.
            return static_cast<uint64_t>(kickos_nestwitness_count(static_cast<int>(a0)));
        }
#endif
// KICKOS_KERNEL_CORES AND NOT ONLY THE SELFTEST FLAG: every op here is placement
// scaffolding, and an arm at one core would put the dispatch arm, its switch and its
// IrqLock into an image whose placement half is otherwise provably absent. The ceiling
// arms need no probe, reading refusals rather than state.
#if defined(KICKOS_ENABLE_SELFTEST) && KICKOS_KERNEL_CORES > 1
        case KOS_SYS_SCHED_PROBE:
        {
            // Test scaffolding for placement. Pure reads, so not privilege-gated and no op
            // takes an argument. Every op but KOS_SCHED_OP_PREEMPTED reads the CALLER's own
            // scheduling state; that one is machine-wide.
            IrqLock lock;
            Thread const* const c = sched::current();
            switch (static_cast<kos_sched_op>(a0))
            {
                case KOS_SCHED_OP_CORE:
                {
                    return kickos_kernel_core();
                }
                case KOS_SCHED_OP_AFFINITY:
                {
                    return c->affinity;
                }
                case KOS_SCHED_OP_TASK_CORES:
                {
                    return task_core_set(c->task);
                }
                case KOS_SCHED_OP_CEILING:
                {
                    return task_prio_ceiling(c->task);
                }
                case KOS_SCHED_OP_ISOLATED:
                {
                    return static_cast<uint32_t>(KICKOS_ISOLATED_CORES);
                }
                case KOS_SCHED_OP_PREEMPTED:
                {
                    return ktime_slice_preempt_cores();
                }
                default:
                {
                    break;
                }
            }
            return static_cast<uint64_t>(-KOS_EINVAL);
        }
#endif
#if KICKOS_HAVE_ASPACE && defined(KICKOS_ENABLE_SELFTEST)
        case KOS_SYS_ASPACE_PROBE:
        {
            // Test scaffolding for the address-space seam. Gated PER OP and not here: three
            // ops take a caller-supplied address (FRAME_AT, MEMTYPE_AT, UNMAP_HERE) and two
            // name pool frames with a capability in the caller's table, which
            // syscall_aspace.cc refuses without AUTH_MEMORY.
            return aspace_probe(a0, a1);
        }
#elif defined(KICKOS_ENABLE_SELFTEST)
        case KOS_SYS_ASPACE_PROBE:
        {
            // A REGION BOARD DECLINES BY NAME rather than falling to the unknown-number arm:
            // an arm that reads this refusal to decide whether the board translates cannot
            // use -KOS_EINVAL, which also means "bad op".
            return static_cast<uint64_t>(-KOS_ENOSYS);
        }
#endif
// Neither the address-space gate nor the partition's: the matrix is indexed by MACHINE core,
// so every posture can answer. Where the doorbell folds out of the image the counters read a
// real zero.
#if defined(KICKOS_ENABLE_SELFTEST)
        case KOS_SYS_DOORBELL_PROBE:
        {
            // Pure reads, so not privilege-gated.
            switch (static_cast<kos_doorbell_op>(a0))
            {
                case KOS_DOORBELL_OP_COUNTS:
                {
                    return arch_ipi_counts(static_cast<uint32_t>(a1));
                }
                case KOS_DOORBELL_OP_WIDTH:
                {
                    return static_cast<uint64_t>(KICKOS_DOORBELL_CORES);
                }
                case KOS_DOORBELL_OP_SELF:
                {
                    return static_cast<uint64_t>(arch_doorbell_core());
                }
                case KOS_DOORBELL_OP_KERNEL_LINE:
                {
                    for (uintptr_t line = a1; line < static_cast<uintptr_t>(KICKOS_MAX_IRQ); line++)
                    {
                        if (arch_irq_line_kernel_owned(static_cast<int>(line)))
                        {
                            return static_cast<uint64_t>(line);
                        }
                    }
                    return static_cast<uint64_t>(static_cast<int64_t>(-1));
                }
                default:
                {
                    break;
                }
            }
            return static_cast<uint64_t>(-KOS_EINVAL);
        }
#endif
#if KICKOS_AMP_NODE && defined(KICKOS_ENABLE_SELFTEST)
        case KOS_SYS_AMP_PROBE:
        {
            // Not privilege-gated as a whole: the ops that forge a publication carry their
            // own root-task gate, and the rest are counter reads.
            return amp_probe(a0, a1);
        }
#endif
#if KICKOS_HAVE_MPU
        case KOS_SYS_GRANT_PROBE:
        {
            // Test scaffolding for the Rule 7 grant predicates. Pure reads, so not
            // privilege-gated. op selects the predicate and posture; the kernel supplies the
            // attr, so userspace needs no ARCH_MPU_* enum. Keyed on the descriptor fact, its
            // arms being region-backend shaped.
            uintptr_t const op = a0;
            uintptr_t const base = a1;
            size_t const size = static_cast<size_t>(a2);
            uint32_t const rw = ARCH_MPU_R | ARCH_MPU_W;
            uint32_t const dev = ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_DEV;
            bool result = false;
            switch (op)
            {
                case KOS_GRANT_OP_HITS_RESERVED:
                {
                    result = grant_hits_reserved(base, size);
                    break;
                }
                case KOS_GRANT_OP_RAM_PRIVILEGED:
                {
                    result = grant_region_admissible(base, size, rw, true);
                    break;
                }
                case KOS_GRANT_OP_RAM_UNPRIVILEGED:
                {
                    result = grant_region_admissible(base, size, rw, false);
                    break;
                }
                case KOS_GRANT_OP_DEV_PRIVILEGED:
                {
                    result = grant_region_admissible(base, size, dev, true);
                    break;
                }
                case KOS_GRANT_OP_DEV_UNPRIVILEGED:
                {
                    result = grant_region_admissible(base, size, dev, false);
                    break;
                }
                case KOS_GRANT_OP_NOCACHE_SUPPORT:
                {
                    // A raw enum arch_mpu_nocache, not the 0/1 predicate every other op
                    // answers.
                    return static_cast<uint64_t>(arch_mpu_nocache_support());
                }
                case KOS_GRANT_OP_RAM_NOCACHE:
                {
                    result = grant_region_admissible(base, size, rw | ARCH_MPU_NOCACHE, false);
                    break;
                }
                case KOS_GRANT_OP_RESERVED_COUNT:
                {
                    struct arch_reserved_block blk[KICKOS_MAX_RESERVED];
                    return arch_reserved_blocks(blk, KICKOS_MAX_RESERVED);
                }
                case KOS_GRANT_OP_RESERVED_BASE:
                {
                    struct arch_reserved_block blk[KICKOS_MAX_RESERVED];
                    std::span const blocks{blk,
                                           arch_reserved_blocks(blk, KICKOS_MAX_RESERVED)};
                    if (base >= blocks.size())
                    {
                        return 0;
                    }
                    return blocks[base].base;
                }
                case KOS_GRANT_OP_RESERVED_SIZE:
                {
                    struct arch_reserved_block blk[KICKOS_MAX_RESERVED];
                    std::span const blocks{blk,
                                           arch_reserved_blocks(blk, KICKOS_MAX_RESERVED)};
                    if (base >= blocks.size())
                    {
                        return 0;
                    }
                    return blocks[base].size;
                }
                default:
                {
                    return static_cast<uint64_t>(-KOS_EINVAL);
                }
            }
            if (result)
            {
                return 1u;
            }
            return 0u;
        }
#endif
        case KOS_SYS_IRQ_UNMASK:
        {
            // Test scaffolding: enable an UNBOUND line so an injected raise reaches the
            // default (spurious) handler on masked-by-default controllers (ARM NVIC, RX),
            // which else drop it. AUTH_IRQ, like irq_attach: it arms a controller line.
            if (not cap_check_authority(sched::current(), AUTH_IRQ))
            {
                return static_cast<uint64_t>(-KOS_EPERM);
            }
            int irq = static_cast<int>(a0);
            if (irq < 0 or irq >= KICKOS_MAX_IRQ)
            {
                return static_cast<uint64_t>(-KOS_EINVAL); // bad irq line
            }
            // As the inject arm above: the image-wide masked word is read-modify-written
            // here, and the backend's own bracket excludes this core's handler alone.
            {
                IrqLock lock;
                irq_line_op(irq, LineOp::UNMASK);
            }
            return 0;
        }
#endif
        case KOS_SYS_CLOCK_NOW:
        {
            // No user pointer and no way to fail: every value in the u64 range is a valid
            // instant, so this arm is OUT of the -KOS_E* scheme.
            return arch_clock_now();
        }
        case KOS_SYS_CPU_CLOCK_HZ:
        {
            // OUT of the -KOS_E* scheme: a u64 Hz whose 0 sentinel already means
            // unknown / no silicon clock.
            return arch_cpu_clock_hz();
        }
        case KOS_SYS_PERIPH_CLOCK_HZ:
        {
            // Reports the branch clock feeding the register block at a0. Ungated, and OUT
            // of the -KOS_E* scheme: a u32 Hz whose 0 sentinel already means unknown.
            return static_cast<uint64_t>(arch_periph_clock_hz(a0));
        }
        case KOS_SYS_PINMUX_SET:
        {
            // a0=port, a1=pin, a2=func, all chip-opaque. AUTH_PINMUX: the mux registers
            // live in the shared SCU/PORT block the kernel keeps. The backend rejects
            // kernel-owned pins.
            // IrqLock: the backends read-modify-write shared mux state unguarded (RX PMR
            // plus a chip-global PWPR unlock bracket, XMC IOCR, SAM ABSR), so a preempting
            // second caller silently drops the loser's write. Bounded: register writes only,
            // no backend waits.
            IrqLock lock;
            Thread* c = sched::current();
            if (not cap_check_authority(c, AUTH_PINMUX))
            {
                return static_cast<uint64_t>(-KOS_EPERM);
            }
            return static_cast<uint64_t>(arch_pinmux_set(a0, a1, a2));
        }
        case KOS_SYS_RAM_ALLOC:
        {
            // IrqLock: arch_ram_alloc does an unguarded read-modify-write of the bump
            // pointer.
            // POINTER return, OUT of the -KOS_E* scheme: a negative errno cast to void*
            // would be a non-NULL pointer, so EVERY failure path returns 0 (NULL) and the
            // documented `if (p == NULL)` check stays correct.
            IrqLock lock;
            Thread* const c = sched::current();
            if (c == nullptr or not cap_check_authority(c, AUTH_MEMORY))
            {
                return 0; // NULL, not (uintptr_t)-1
            }
#if KICKOS_HAVE_ASPACE
            // A page-aligned range RESERVED in the calling task's own space, mapped
            // nowhere. The number is a virtual address in that space, and the frames under it
            // are what makes it a globally unique name the handoff can carry. A privileged
            // caller gets null, holding the kernel domain, which carries no space.
            return aspace_reserve(domain_ranges_mut(task_domain(c->task)),
                                  static_cast<size_t>(a0));
#else
            // The block AND the record of who reserved it (ramown.h).
            return reinterpret_cast<uintptr_t>(
                ram_owner_alloc(c->task, static_cast<size_t>(a0)));
#endif
        }
#if KICKOS_HAVE_ASPACE
        case KOS_SYS_FRAME_MAP:
        case KOS_SYS_FRAME_UNMAP:
        {
            // One lock spans resolve-to-use for BOTH capabilities and the edit they drive.
            IrqLock lock;
            Thread* const c = sched::current();
            if (c == nullptr or not cap_check_authority(c, AUTH_MEMORY))
            {
                // The authority word and not a rights bit: the rights field is full, and
                // widening it spends the reply sequence packed beside it.
                return static_cast<uint64_t>(-KOS_EPERM);
            }
            int ferr = 0;
            FrameRun* const run = static_cast<FrameRun*>(
                cap_resolve_e(c, static_cast<uint32_t>(a0), CapType::CAP_FRAME, 0, &ferr));
            if (run == nullptr)
            {
                if (ferr == 0)
                {
                    ferr = KOS_EBADF;
                }
                return static_cast<uint64_t>(-ferr);
            }
            CapEntry const* const fe = cap_lookup(c, static_cast<uint32_t>(a0));
            if (fe == nullptr)
            {
                return static_cast<uint64_t>(-KOS_EBADF);
            }
            int const run_obj = fe->obj;
            int aerr = 0;
            Domain* const target = static_cast<Domain*>(
                cap_resolve_e(c, static_cast<uint32_t>(a1), CapType::CAP_ASPACE, 0, &aerr));
            if (target == nullptr)
            {
                if (aerr == 0)
                {
                    aerr = KOS_EBADF;
                }
                return static_cast<uint64_t>(-aerr);
            }
            struct arch_aspace* const sp = domain_space(target);
            VirtualRanges* const vr = domain_ranges_mut(target);
            if (sp == nullptr or vr == nullptr)
            {
                return static_cast<uint64_t>(-KOS_EINVAL);
            }
            if (nr == KOS_SYS_FRAME_UNMAP)
            {
                return static_cast<uint64_t>(aspace_cap_unmap(sp, vr, a2, run_obj));
            }
            enum arch_map_memtype type = ARCH_MAP_NORMAL;
            if ((static_cast<uint32_t>(a3) & KOS_MEM_NOCACHE) != 0)
            {
                type = ARCH_MAP_NOCACHE;
            }
            if ((static_cast<uint32_t>(a3) & ~static_cast<uint32_t>(KOS_MEM_FLAGS_ALL)) != 0)
            {
                return static_cast<uint64_t>(-KOS_EINVAL);
            }
            return static_cast<uint64_t>(
                aspace_cap_map(sp, vr, a2, run_obj, run->base, run->pages,
                               ARCH_MAP_R | ARCH_MAP_W, type));
        }
#endif
        case KOS_SYS_MEM_SELF_GRANT:
        {
            // The grant half of KOS_SYS_RAM_ALLOC, added to the CALLER's own region set.
            // AUTHORITY is thread-local by contract; how wide the resulting reach is belongs to
            // the backend, and a translating one maps into the task's space, so no caller may
            // infer sibling denial from this.
            IrqLock lock;
            Thread* const c = sched::current();
            if (c == nullptr or not cap_check_authority(c, AUTH_MEMORY))
            {
                return static_cast<uint64_t>(-KOS_EPERM);
            }
            uintptr_t const base = a0;
            size_t const size = static_cast<size_t>(a1);
            if (size == 0 or (base + size) < base)
            {
                return static_cast<uint64_t>(-KOS_EINVAL);
            }
            uint32_t attr = ARCH_MPU_R | ARCH_MPU_W;
            if (not mem_flags_to_attr(a2, &attr))
            {
                return static_cast<uint64_t>(-KOS_EINVAL);
            }
            // BEFORE the already-reachable short-circuit: a chip that cannot honour the
            // memory type would otherwise answer 0 to a request it silently drops.
            if (not grant_nocache_admissible(attr))
            {
                return static_cast<uint64_t>(-KOS_EPERM);
            }
            // Already reachable costs no descriptor. Where the mapping CARRIES the memory type
            // the question is asked of the TYPE in both directions: a request naming no type
            // over a block mapped non-cacheable is the way back from a DMA buffer to ordinary
            // memory. Privileged reach comes from the CACHEABLE background map on a region chip.
            bool already = false;
            if (grant_memtype_programmed())
            {
                already = user_range_typed_ok(base, size, attr);
            }
            else
            {
                already = user_range_ok(base, size, ARCH_MPU_R | ARCH_MPU_W);
            }
            if (already)
            {
                return 0;
            }
#if KICKOS_HAVE_ASPACE
            // The map half: the range must be one this task RESERVED, which is what
            // refuses an address another task reserved. The arena and natural-alignment arms
            // below do not run here, a reservation naming frame-pool frames.
            enum arch_map_memtype mtype = ARCH_MAP_NORMAL;
            if ((attr & ARCH_MPU_NOCACHE) != 0)
            {
                mtype = ARCH_MAP_NOCACHE;
            }
            int const grc = aspace_self_grant(domain_space(task_domain(c->task)),
                                              domain_ranges_mut(task_domain(c->task)), base,
                                              size, ARCH_MAP_R | ARCH_MAP_W, mtype);
            // The range list already carries this mapping, at the EXACT extent, so there is no
            // region record beside it. That is what moves the -KOS_ENOMEM budget onto
            // KICKOS_ASPACE_RANGES and what makes the grant reach every sibling, the array
            // being per-THREAD while the mapping is task-wide.
            return static_cast<uint64_t>(grc);
#else
            // Rule 7 admission on the geometry that will actually be committed: a window
            // rounded up AFTER admission could cover a neighbour the unrounded extent did
            // not.
            size_t const rsz = arch_ram_region_size(size);
            if (rsz == 0)
            {
                return static_cast<uint64_t>(-KOS_EINVAL);
            }
            // Nameable by one descriptor, as for the stack grant (syscall_thread.cc):
            // PMSAv7 MPU_RBAR MASKS the base down to the region size, so an unaligned base
            // would be programmed as a window starting BELOW what the caller named. On a
            // no-MPU arch it still demands a 16-aligned base.
            if (not arch_ram_region_admissible(base, rsz))
            {
                return static_cast<uint64_t>(-KOS_EINVAL);
            }
            if (not grant_region_admissible(base, rsz, attr,
                                            cap_check_authority(c, AUTH_MEMORY)))
            {
                return static_cast<uint64_t>(-KOS_EPERM);
            }
            // Rule 7 answers for the arena's BOUNDS and not for who inside it reserved what:
            // without this a caller could make a sibling task's block reachable to itself.
            // Only a NEW window: the already-reachable short circuit above is what a
            // privileged caller's whole-arena background map answers on.
            if (not ram_owner_nameable(c->task, base, size))
            {
                return static_cast<uint64_t>(-KOS_EPERM); // never reserved by this task
            }
            // Full budget, or a region this backend seats no descriptor for, is a returned
            // error: truncating the set or carrying the grant unenforced would fault the thread
            // on memory it was told it had. The knob here is KICKOS_MPU_MAX_REGIONS, so the code
            // is not -KOS_EMFILE.
            //
            // A block this thread already names with another memory type is RETYPED in place;
            // two descriptors over one block would leave the range checks and the hardware
            // reading different ones. It sits inside MpuSet because a `prior` region held across
            // the call here is caller stack the SVC red zone is measured on.
            if (not c->mpu.add_enforced_retyping(base, rsz, attr))
            {
                return static_cast<uint64_t>(-KOS_ENOMEM);
            }
            // Must be effective BEFORE the return: the caller's next
            // instruction may dereference the region, and on a deferred-switch
            // arch apply() only STASHES, the commit being what programs the
            // hardware.
            c->mpu.apply();
            kickos_arch_mpu_commit();
            return 0;
#endif
        }
        case KOS_SYS_PERIPH_ENABLE:
        {
            // Possession, not an authority bit: holding the window is the right to
            // enable the device.
            IrqLock lock;
            if (not caller_holds_mmio_block(a0))
            {
                return static_cast<uint64_t>(-KOS_EPERM);
            }
            return static_cast<uint64_t>(arch_periph_enable(a0));
        }
        case KOS_SYS_PERIPH_REG_WRITE:
        {
            // Checked before possession: the store is one 32-bit word, so an unaligned or
            // wrapping target is -KOS_EINVAL whatever the caller holds.
            if ((a1 & (sizeof(uint32_t) - 1u)) != 0)
            {
                return static_cast<uint64_t>(-KOS_EINVAL);
            }
            uintptr_t const top = ~static_cast<uintptr_t>(0);
            if (a1 > top - a0 or (a0 + a1) > top - (sizeof(uint32_t) - 1u))
            {
                return static_cast<uint64_t>(-KOS_EINVAL);
            }
            // Possession of the block at a0 AND of the word at a0+a1 inside it. The
            // allowlist bounds which registers of a held block are writable, not the
            // ADDRESS, so without the containment a 32-byte window would reach any register
            // the table names anywhere in the block.
            IrqLock lock;
            if (not caller_holds_mmio_reg(a0, a1))
            {
                return static_cast<uint64_t>(-KOS_EPERM);
            }
            return static_cast<uint64_t>(
                arch_periph_reg_write(a0, a1, static_cast<uint32_t>(a2)));
        }
        case KOS_SYS_CAP_NARROW:
        {
            // UNGATED: giving up authority needs none, and a gate would be a bit a thread
            // must keep in order to drop the others. It can only clear bits in the CALLER's
            // own table.
            IrqLock lock;
            return static_cast<uint64_t>(
                cap_narrow_authority(sched::current(), static_cast<uint32_t>(a0),
                                     static_cast<uint8_t>(a1)));
        }
        case KOS_SYS_PANIC:
        {
            // UNGATED: kpanic masks IRQs and reads kernel .bss, so a thread running it from
            // its own unprivileged frame would fault there and lose the diagnostic.
            user_panic(a0); // noreturn
            return 0;
        }
        case KOS_SYS_IRQ_ATTACH:
        {
            // No authority: possession of a WAIT-bearing cap is the authorisation, as it is
            // for wait and ack. The out-pointer carries the bit this line will arrive in, so
            // it is checked BEFORE the bind, which a refused write could not undo.
            int rc = cap_out_check(a1);
            if (rc != 0)
            {
                return static_cast<uint64_t>(rc);
            }
            uint32_t mask = 0;
            rc = irq_notify_bind(sched::current(), static_cast<uint32_t>(a0), &mask);
            return cap_out_deliver(a1, rc, mask);
        }
        case KOS_SYS_IRQ_CLAIM:
        {
            // AUTH_IRQ, like IRQ_UNMASK: the tier-1 mint takes a bare line number out of the
            // namespace and makes it owned. USING an already-claimed line needs no
            // authority; possession of the cap is the authorisation, checked in
            // cap_resolve_e.
            if (not cap_check_authority(sched::current(), AUTH_IRQ))
            {
                return static_cast<uint64_t>(-KOS_EPERM);
            }
            int rc = cap_out_check(a2);
            if (rc != 0)
            {
                return static_cast<uint64_t>(rc);
            }
            uint32_t h = KCAP_INVALID;
            rc = irq_claim(sched::current(), static_cast<int>(a0),
                           static_cast<unsigned int>(a1), &h);
            return cap_out_deliver(a2, rc, h);
        }
        case KOS_SYS_IRQ_WAIT_TIMED:
        {
            return static_cast<uint64_t>(irq_wait_timed(sched::current(),
                                                        static_cast<uint32_t>(a0),
                                                        static_cast<uint32_t>(a1)));
        }
        case KOS_SYS_IRQ_WAIT:
        {
            return static_cast<uint64_t>(irq_wait(sched::current(), static_cast<uint32_t>(a0)));
        }
        case KOS_SYS_IRQ_ACK:
        {
            return static_cast<uint64_t>(irq_ack(sched::current(), static_cast<uint32_t>(a0)));
        }
        case KOS_SYS_IRQ_NOTIFY:
        {
            return static_cast<uint64_t>(irq_notify(sched::current(), static_cast<uint32_t>(a0)));
        }
        case KOS_SYS_IRQ_DISCARD:
        {
            return static_cast<uint64_t>(irq_discard(sched::current(), static_cast<uint32_t>(a0)));
        }
#if KICKOS_BENCH
        case KOS_SYS_BENCH:
        {
            // The ONLY route to the bench helpers from an app: each reads kernel .data or a
            // peripheral, so an app calling them directly runs them at ITS privilege and faults.
            // Both prints run here, in thread context and holding no IrqLock.
            //
            // BEING A BENCH IMAGE GRANTS NO THREAD ANYTHING. An op that attaches or rings a
            // doorbell carries the AUTH_IRQ its non-bench counterpart carries. RAISE injects
            // too, but carries none: the arm already chose its line from a cap, so RAISE takes
            // no line of its own to guard. Every caller-supplied count is bounded here rather
            // than inside the helper, so a sweep body stays a measurement and not a gate.
            switch (a0)
            {
                case KOS_BENCH_OP_RESET:
                {
                    // The fastpath's own count, handed in rather than reached for: the bench
                    // has no business including a syscall-private header, and a counter of its
                    // own beside this one would be a second truth about the same swap.
                    bench_reset(ipc_fast_taken_count());
                    return 0;
                }
                case KOS_BENCH_OP_CYCCNT_HZ:
                {
                    return bench_cyccnt_hz();
                }
                case KOS_BENCH_OP_DIST_PRINT:
                {
                    return bench_dist_print(ipc_fast_taken_count());
                }
                case KOS_BENCH_OP_IRQ_SETUP:
                {
                    // bench_irq_setup calls irq_attach, whose own syscall (KOS_SYS_IRQ_ATTACH)
                    // takes AUTH_IRQ; reaching it through here must not be the cheaper route.
                    if (not cap_check_authority(sched::current(), AUTH_IRQ))
                    {
                        return static_cast<uint64_t>(-KOS_EPERM);
                    }
                    int const line = static_cast<int>(a1);
                    if (line < 0 or line >= KICKOS_MAX_IRQ)
                    {
                        return static_cast<uint64_t>(-KOS_EINVAL);
                    }
                    return static_cast<uint64_t>(bench_irq_setup(line));
                }
                case KOS_BENCH_OP_IRQ_SWEEP:
                {
                    if (not cap_check_authority(sched::current(), AUTH_IRQ))
                    {
                        return static_cast<uint64_t>(-KOS_EPERM);
                    }
                    if (a1 > KOS_BENCH_SAMPLES_MAX)
                    {
                        return static_cast<uint64_t>(-KOS_EINVAL);
                    }
                    return bench_irq_sweep(static_cast<uint32_t>(a1));
                }
                case KOS_BENCH_OP_IRQ_WCASE:
                {
                    if (not cap_check_authority(sched::current(), AUTH_IRQ))
                    {
                        return static_cast<uint64_t>(-KOS_EPERM);
                    }
                    if (a2 > KOS_BENCH_SAMPLES_MAX)
                    {
                        return static_cast<uint64_t>(-KOS_EINVAL);
                    }
                    return bench_irq_wcase_sweep(static_cast<uint32_t>(a1),
                                                 static_cast<uint32_t>(a2));
                }
                case KOS_BENCH_OP_WCASE_SPANS:
                {
                    return bench_irq_wcase_spans();
                }
                case KOS_BENCH_OP_E2E_ARM:
                {
                    // The span's line is the one the caller's OWN cap names, never a bare
                    // number: the raise that follows takes no authority, so this is where the
                    // line stops being attacker-chosen. Resolve and use under one lock, as
                    // every other cap arm does.
                    IrqLock lock;
                    int err = 0;
                    IrqBinding* const b = static_cast<IrqBinding*>(
                        cap_resolve_e(sched::current(), static_cast<uint32_t>(a1),
                                      CapType::CAP_IRQ, CAP_WAIT, &err));
                    if (b == nullptr)
                    {
                        return static_cast<uint64_t>(-err); // EBADF, or EPERM without WAIT
                    }
                    return static_cast<uint64_t>(bench_e2e_arm(b->line));
                }
                case KOS_BENCH_OP_E2E_RAISE:
                {
                    return static_cast<uint64_t>(bench_e2e_raise());
                }
                case KOS_BENCH_OP_E2E_TARE:
                {
                    return static_cast<uint64_t>(bench_e2e_tare());
                }
                case KOS_BENCH_OP_E2E_CLOSE:
                {
                    return static_cast<uint64_t>(bench_e2e_close());
                }
                case KOS_BENCH_OP_E2E_PRINT:
                {
                    // NOT bounded, unlike the counted ops above: a1 drives no kernel work at
                    // all. It is the sweep size the app ran, echoed beside the raises the
                    // kernel let through, and the sweep's loop is the app's.
                    bench_e2e_print(static_cast<uint32_t>(a1));
                    return 0;
                }
                case KOS_BENCH_OP_PHASE_PRINT:
                {
                    bench_phase_print();
                    return 0;
                }
                case KOS_BENCH_OP_LOCK_PROBE:
                {
                    bench_lock_probe_print();
                    return 0;
                }
                case KOS_BENCH_OP_DOORBELL_PROBE:
                {
#if KICKOS_KERNEL_CORES > 1
                    // Every round runs under ONE IrqLock and raises an IPI on every peer, so
                    // the round count is how long this core stays masked and how hard the
                    // others are hammered. AUTH_IRQ, and never more rounds than the instrument
                    // sweeps.
                    if (not cap_check_authority(sched::current(), AUTH_IRQ))
                    {
                        return static_cast<uint64_t>(-KOS_EPERM);
                    }
                    if (a1 >= KICKOS_KERNEL_CORES or a2 > KOS_BENCH_ROUNDS_MAX)
                    {
                        return static_cast<uint64_t>(-KOS_EINVAL);
                    }
                    return bench_doorbell_probe_print(static_cast<uint32_t>(a1),
                                                      static_cast<uint32_t>(a2));
#else
                    return static_cast<uint64_t>(-KOS_ENOSYS);
#endif
                }
                default:
                {
                    return static_cast<uint64_t>(-KOS_EINVAL);
                }
            }
        }
#endif
        case KOS_SYS_DIAG_LED_SET:
        {
            // The kernel's diagnostic pin, borrowed, and left unprivileged like the
            // console. A no-op on boards with no LED.
            kdiag_led_set(a0 != 0);
            return 0;
        }
        case KOS_SYS_DIAG_LED_TOGGLE:
        {
            kdiag_led_toggle();
            return 0;
        }
        default:
        {
            // An unknown syscall is a caller error, not a kernel invariant violation: fault
            // the caller, never panic the kernel.
            return static_cast<uint64_t>(-KOS_EINVAL);
        }
    }
}
}
