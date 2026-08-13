// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The arch-independent syscall table + dispatch. The arch entry (sim
// trampoline / ARM SVC handler) reads the number + args and calls
// syscall_dispatch(); the result is delivered back to the caller frame.
// Kernel objects addressable from userspace (semaphores, threads) live in
// static pools referenced by small integer handles: no pointers cross the
// boundary.

#include <kickos/arch/arch.h>
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
#include <kickos/console_tx.h>

#include <kickos/sys/abi.h>

#include "syscall_internal.h"

#include <span>

namespace kickos
{
    // syscall_dispatch answers at REGISTER width (8 bytes on the host), and the userspace
    // stub narrows that to a fixed 4. The byte-count producers must therefore already be
    // 4 bytes here, or the two halves of the boundary disagree on which target: see the
    // matching static_assert in user/src/syscall_stubs.cc.
    static_assert(sizeof(endpoint_send(0, 0, 0, 0)) == 4, "must be exactly 4 bytes");
    static_assert(sizeof(endpoint_recv(0, 0, 0, 0, false)) == 4, "must be exactly 4 bytes");
    static_assert(sizeof(endpoint_call(0, 0, 0, 0, 0)) == 4, "must be exactly 4 bytes");

    namespace
    {
        // Max yield passes kos_console_publish waits for the in-flight chip-writer count
        // to reach 0 before declaring a stuck writer.
        constexpr uint32_t CONSOLE_PUBLISH_DRAIN_MAX = KICKOS_POLL_SPIN_MAX;

        // Privileged in-kernel IRQ handler bound by KOS_SYS_IRQ_ATTACH: posts a
        // semaphore from ISR context, driving the interrupt-exit switch (trigger #4).
        // arg is the GLOBAL sem handle irq_attach resolved+stored (NOT a cap): an ISR
        // must never resolve a cap (current() is a random interrupted thread's table).
        void irq_sem_post(void* arg)
        {
            int handle = static_cast<int>(reinterpret_cast<intptr_t>(arg));
            Semaphore* s = kernel().sems.resolve(handle);
            if (s != nullptr)
            {
                sem_post(s);
            }
        }

        // A minting syscall's out-pointer, checked BEFORE the object is created: a mint
        // that succeeded and then could not deliver its handle would leave an object
        // nothing can name and nothing can close. Same rules as CLOCK_NOW's and RECV's
        // out-pointers: the kernel writes it privileged, so an unprivileged caller must
        // own it, and a misaligned pointer is a malformed argument. Serves the THREAD mint
        // too: kos_thread_t and kos_cap_t are different codecs, both 32-bit words.
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

        // Deliver a minted handle. Nothing is written on failure: the stub seated its
        // codec's NONE before trapping, so the sys.h "always written" guarantee already
        // holds.
        uintptr_t cap_out_deliver(uintptr_t out, int rc, uint32_t handle)
        {
            if (rc == 0)
            {
                kaccess_to_user(out, &handle, sizeof(handle));
            }
            return static_cast<uintptr_t>(rc);
        }

        // KOS_SYS_PANIC's body. noinline is load-bearing: the message buffer must not
        // widen syscall_dispatch's frame, which sits on the CALLING thread's stack and
        // is sized by KICKOS_MIN_STACK_SIZE against the deepest ordinary dispatch.
        // Depth past kpanic does not matter (it never returns).
        __attribute__((noinline, noreturn)) void user_panic(uintptr_t msg)
        {
            char buf[64];
            buf[0] = '\0';
            // A privileged caller passes user_readable_ok wholesale, so null is
            // rejected here rather than by the per-byte check.
            if (msg != 0)
            {
                // Check EACH source byte before the privileged copy dereferences it:
                // the kernel must not fault on, or leak another domain's page through,
                // a bad message pointer. This BOUNDS the walk at the first unreachable
                // byte, so a string with no NUL in a granted region stops there.
                size_t i = 0;
                for (; i + 1 < sizeof(buf); i++)
                {
                    if (not user_readable_ok(msg + i, 1))
                    {
                        break;
                    }
                    kaccess_from_user(&buf[i], msg + i, 1);
                    if (buf[i] == '\0')
                    {
                        break;
                    }
                    // This message prints after the kernel's trusted "KERNEL PANIC: "
                    // prefix, so no control byte may reach the console: a newline lets the
                    // caller continue on fresh lines that read as kernel output, an
                    // embedded "=== MPU FAULT ===" included. Every such byte is REPLACED,
                    // so the message is not cut short at the first one.
                    unsigned char const c = static_cast<unsigned char>(buf[i]);
                    if (c < 0x20u or c == 0x7Fu)
                    {
                        buf[i] = '?';
                    }
                }
                buf[i] = '\0';
                // A truncation must be visible, because <kickos/sys.h> promises one.
                // The marker OVERWRITES kept bytes, so buf's size is unchanged. The
                // probe byte is the first one dropped: unreadable there means nothing
                // was dropped that the kernel could have copied.
                if (i + 1 == sizeof(buf) and user_readable_ok(msg + i, 1))
                {
                    char probe = '\0';
                    kaccess_from_user(&probe, msg + i, 1);
                    if (probe != '\0')
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

    // RAII SYSCALL_ENTER/EXIT bracket. The EXIT fires in the destructor on EVERY
    // ordinary return path. But KOS_SYS_EXIT switches away permanently inside the
    // dispatch (never returns to this frame), so its destructor never runs and it
    // is recorded as ENTER-only (the decoder handles the missing EXIT).
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
    uintptr_t syscall_body(uintptr_t nr, uintptr_t a0, uintptr_t a1,
                           uintptr_t a2, uintptr_t a3);
}

// THE death point of a cancelled thread, and the reason a cancel is more than a request. A
// cancel breaks whatever park the target is in, so it returns to userspace with
// -KOS_ECANCELED and gets ONE window to clean up over memory it already holds; the next time
// it asks the kernel for anything, it ends here instead. That covers the primitives with no
// error channel to report through (a semaphore wait, a sleep) and the target that simply
// ignores the code, and the only survivor is a thread that never enters the kernel again.
//
// Checked on ENTRY and deliberately not on exit: on exit it would pre-empt the cleanup window
// the broken park exists to give.
extern "C" uintptr_t syscall_dispatch(uintptr_t nr,
                                      uintptr_t a0, uintptr_t a1,
                                      uintptr_t a2, uintptr_t a3)
{
    Thread* const caller = sched::current();
    if (caller != nullptr and caller->cancelled and not caller->dying)
    {
        sched::exit_current(KOS_EXIT_CANCELLED); // noreturn
    }
    return syscall_body(nr, a0, a1, a2, a3);
}

namespace
{
uintptr_t syscall_body(uintptr_t nr,
                       uintptr_t a0, uintptr_t a1,
                       uintptr_t a2, uintptr_t a3)
{
    KTRACE_SYSCALL_SCOPE(nr);
    switch (nr)
    {
        case KOS_SYS_KCONSOLE_WRITE:
        {
            // Explicit (buf, len): the kernel must never strlen a user pointer.
            // Clamp len (a garbage/huge value must not walk off RAM or hog the UART),
            // then bound buf against the caller's memory so an unprivileged thread
            // cannot launder another domain's arena page out through the console
            // (the kernel reads buf privileged). Reject => wrote nothing.
            constexpr size_t MAX_CONSOLE_WRITE = 4096;
            // MMU-era NOTE: this hands a user pointer straight to kconsole_write, which
            // streams it privileged. It is the one kernel-side user read NOT funnelled
            // through kaccess_from_user.
            char const* buf = reinterpret_cast<char const*>(a0);
            size_t len = static_cast<size_t>(a1);
            if (len > MAX_CONSOLE_WRITE)
            {
                len = MAX_CONSOLE_WRITE;
            }
            if (not user_readable_ok(a0, len))
            {
                // Cross-domain / bad buffer: write nothing. Negative code, NOT 0: a
                // len-0 write legitimately returns 0, so 0 must not double as reject.
                return static_cast<uintptr_t>(-KOS_EFAULT);
            }
            kconsole_write(buf, len); // fan-out (chip + RTT), not the raw transport
            return len;
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
                return static_cast<uintptr_t>(rc);
            }
            uint32_t h = KCAP_INVALID;
            rc = sem_create(static_cast<int>(a0), &h);
            return cap_out_deliver(a1, rc, h);
        }
        case KOS_SYS_HANDLE_CLOSE:
        {
            // Type-agnostic close: drop THIS task's cap (a cap knows its own type).
            // Refcounted: the object is freed only at the last close.
            IrqLock lock;
            return static_cast<uintptr_t>(handle_close(sched::current(), static_cast<uint32_t>(a0)));
        }
        case KOS_SYS_SEM_WAIT:
        {
            // Resolve and use under one lock (sem_wait/sem_post nest their own):
            // otherwise a concurrent close could free the slot between resolve and use.
            IrqLock lock;
            int err = 0;
            Semaphore* s = static_cast<Semaphore*>(
                cap_resolve_e(sched::current(), static_cast<uint32_t>(a0), CapType::CAP_SEM, CAP_WAIT, &err));
            if (s == nullptr)
            {
                return static_cast<uintptr_t>(-err); // EBADF (bad/closed cap) or EPERM (no WAIT right)
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
                return static_cast<uintptr_t>(-err); // EBADF (bad/closed cap) or EPERM (no SIGNAL right)
            }
            if (not sem_post(s))
            {
                return static_cast<uintptr_t>(-KOS_EOVERFLOW); // count already at the ceiling
            }
            return 0;
        }
        case KOS_SYS_MUTEX_CREATE:
        {
            int rc = cap_out_check(a0);
            if (rc != 0)
            {
                return static_cast<uintptr_t>(rc);
            }
            uint32_t h = KCAP_INVALID;
            rc = mutex_create(&h);
            return cap_out_deliver(a0, rc, h);
        }
        case KOS_SYS_MUTEX_LOCK:
        {
            // Resolve under a short lock; mutex_lock then takes its OWN lock for the
            // acquire/park and (critically) releases it before the resume barrier +
            // wait_result read, so the ARM deferred-PendSV block completes first (a
            // continuous lock across mutex_lock would reintroduce the stale read).
            // The resolve->call window needs no lock: the caller's own cap pins the
            // mutex (mutex_refs >= 1), and there is no cross-thread close/kill path
            // that could free it, so the resolved pointer stays valid. need == 0:
            // possession is the authority.
            Mutex* m;
            int err = 0;
            {
                IrqLock lock;
                m = static_cast<Mutex*>(
                    cap_resolve_e(sched::current(), static_cast<uint32_t>(a0), CapType::CAP_MUTEX, 0, &err));
            }
            if (m == nullptr)
            {
                return static_cast<uintptr_t>(-err); // -KOS_EBADF (need == 0, so never EPERM here)
            }
            // 0 / -KOS_EOWNERDEAD (HELD, owner died) / -KOS_EDEADLK (NOT held). EOWNERDEAD is
            // negative but still an ACQUIRE: the wrapper decl documents the held-vs-not caveat.
            return static_cast<uintptr_t>(mutex_lock(m));
        }
        case KOS_SYS_MUTEX_UNLOCK:
        {
            IrqLock lock;
            int err = 0;
            Mutex* m = static_cast<Mutex*>(
                cap_resolve_e(sched::current(), static_cast<int>(a0), CapType::CAP_MUTEX, 0, &err));
            if (m == nullptr)
            {
                return static_cast<uintptr_t>(-err); // -KOS_EBADF (bad cap)
            }
            return static_cast<uintptr_t>(mutex_unlock(m)); // 0, or -KOS_EPERM if not owner (no panic)
        }
        case KOS_SYS_ENDPOINT_CREATE:
        {
            int rc = cap_out_check(a0);
            if (rc != 0)
            {
                return static_cast<uintptr_t>(rc);
            }
            uint32_t h = KCAP_INVALID;
            rc = endpoint_create(&h);
            return cap_out_deliver(a0, rc, h);
        }
        case KOS_SYS_SEND:
        {
            // FULLY LOCKLESS (no dispatch IrqLock): endpoint_send takes its own lock for
            // the resolve/deliver/park, then releases it before the resume barrier: a
            // spanning caller lock would livelock ARM (design section 3).
            return static_cast<uintptr_t>(
                endpoint_send(static_cast<uint32_t>(a0), a1, static_cast<size_t>(a2),
                              KOS_TIMEOUT_NONE));
        }
        case KOS_SYS_SEND_TIMED:
        {
            return static_cast<uintptr_t>(
                endpoint_send(static_cast<uint32_t>(a0), a1, static_cast<size_t>(a2),
                              static_cast<uint32_t>(a3)));
        }
        case KOS_SYS_RECV:
        {
            return static_cast<uintptr_t>(
                endpoint_recv(static_cast<uint32_t>(a0), a1, static_cast<size_t>(a2), a3,
                              /*timed=*/false));
        }
        case KOS_SYS_RECV_TIMED:
        {
            // The deadline is not an argument: a3 names a kos_recv_timed_opts holding it,
            // with the ordinary kos_recv_info nested inside. endpoint_recv reads the
            // deadline through the validated-pointer path and hands the rest of itself the
            // nested struct, so nothing downstream knows the difference.
            return static_cast<uintptr_t>(
                endpoint_recv(static_cast<uint32_t>(a0), a1, static_cast<size_t>(a2), a3,
                              /*timed=*/true));
        }
        case KOS_SYS_CALL:
        {
            // FULLY LOCKLESS (no dispatch IrqLock), same as SEND/RECV: a spanning caller
            // lock would keep BASEPRI raised across the resume barrier and livelock ARM.
            return static_cast<uintptr_t>(
                endpoint_call(static_cast<uint32_t>(a0), a1, static_cast<size_t>(a2),
                              static_cast<size_t>(a3), KOS_TIMEOUT_NONE));
        }
        case KOS_SYS_CALL_TIMED:
        {
            // a2 carries both lengths so a3 can carry the deadline. Unpacking is this arm's
            // whole job: the bound checks stay in endpoint_call, which is the sole validator
            // (the stub only saturates, so an oversize length still arrives out of range).
            return static_cast<uintptr_t>(
                endpoint_call(static_cast<uint32_t>(a0), a1, kos_call_lens_send(a2),
                              kos_call_lens_recv(a2), static_cast<uint32_t>(a3)));
        }
        case KOS_SYS_REPLY:
        {
            // Does not block the replier (it wakes the caller and returns), so it does
            // its whole job under endpoint_reply's own lock.
            return static_cast<uintptr_t>(
                endpoint_reply(static_cast<uint32_t>(a0), a1, static_cast<size_t>(a2)));
        }
        case KOS_SYS_CONSOLE_PUBLISH:
        {
            // Hand the console UART to a userspace driver named by an endpoint cap.
            // AUTH_CONSOLE, its own bit: the driver that publishes and the thread that
            // ends the system are different threads once root is only a spawner, so this
            // cannot share shutdown's bit. See the handover design (D3).
            Thread* c = sched::current();
            if (not cap_check_authority(c, AUTH_CONSOLE))
            {
                return static_cast<uintptr_t>(-KOS_EPERM);
            }
            int handle = -1;
            {
                IrqLock lock;
                // Resolve the endpoint cap to its GLOBAL gen-encoded handle, NOT the pool
                // index (S3). cap_lookup validates the cap-gen; re-check type + object
                // liveness (mirrors irq_attach's resolve-once pattern). Any rights: the
                // publish is identity-only.
                CapEntry* e = cap_lookup(c, static_cast<uint32_t>(a0));
                if (e == nullptr or e->type != static_cast<uint8_t>(CapType::CAP_ENDPOINT)
                    or kernel().endpoints.resolve(e->obj) == nullptr)
                {
                    return static_cast<uintptr_t>(-KOS_EBADF); // bad / non-endpoint / stale cap
                }
                handle = e->obj;
                // Must precede the relinquish below: this is the only remaining step that
                // can fail, and a refusal has to leave a working console behind. Children
                // spawned after this get slot 0 via cap_install_defaults.
                if (not cap_console_publish(c, handle))
                {
                    return static_cast<uintptr_t>(-KOS_EOVERFLOW); // endpoint refcount ceiling
                }
                if (console_owner_is_kernel() != 0)
                {
                    console_tx_deinit(); // idempotent; skipped on re-publish
                }
                console_owner_set_user();    // must be LAST
            }
            // Drains, with the lock RELEASED, any stale chip writer that raced past the
            // pre-flip state read. Root spawns the driver only after this returns, so the
            // preempted writer is off the device before the driver touches it.
            //
            // A bare busy-spin here LIVELOCKS under strict priority: an in-flight writer
            // preempted mid arch_console_write_sync (a polled loop run WITHOUT IrqLock) can
            // only finish once rescheduled, and it may be LOWER priority than this
            // publisher. Hence the drop to the minimum real priority plus a yield each
            // pass. Draining to zero terminates because the state is already USER_OWNED so
            // nothing increments the count, and a polled writer never blocks between enter
            // and leave, so a non-zero count always means a RUNNABLE writer.
            Thread* pub = sched::current();
            uint8_t const saved_prio = pub->prio;
            sched::set_prio(pub, KICKOS_PRIO_MIN);
            // Each pass is a full scheduler round and a poke is a handful of polled bytes,
            // so a count that never drains is a real bug. Panicking beats hanging silently
            // or, worse, proceeding while a writer still pokes the UART.
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
            sched::set_prio(pub, saved_prio);
            return 0;
        }
        case KOS_SYS_CPU_CLOCK_SET:
        {
            // Privileged-only (like console_publish / ram_alloc): it mutates
            // SystemCoreClock, retimes every thread's SysTick basis, and moves the
            // shared console baud: an unprivileged retune could DoS every task's
            // timing. Return 0 (== the cannot-change sentinel) on the unprivileged
            // path so the caller needs only ONE error test. The coherence sequence
            // (mask / disarm / flush / retune / re-arm) lives in cpu_clock_set.
            // NOTE: this syscall stays OUT of the -KOS_E* scheme: it returns a u32 Hz
            // whose 0 sentinel already means cannot/unsupported/not-permitted, and the
            // console-owned refusal (an EBUSY-shaped condition) surfaces as "unchanged Hz".
            Thread* c = sched::current();
            if (not cap_check_authority(c, AUTH_PSTATE))
            {
                return 0;
            }
            return static_cast<uintptr_t>(
                cpu_clock_set(static_cast<kos_pstate_t>(a0)));
        }
        case KOS_SYS_THREAD_SPAWN:
        {
            // Checked BEFORE the child is created: a spawn that succeeded and then could not
            // deliver its handle would leave a thread nothing can name or kill.
            int rc = cap_out_check(a1);
            if (rc != 0)
            {
                return static_cast<uintptr_t>(rc);
            }
            kos_thread_t h = KOS_THREAD_NONE;
            rc = thread_spawn(reinterpret_cast<kos_thread_params const*>(a0), &h);
            return cap_out_deliver(a1, rc, h);
        }
        case KOS_SYS_THREAD_KILL:
        {
            // UNGATED by authority, gated by parenthood inside (syscall_thread.cc).
            return static_cast<uintptr_t>(thread_kill(static_cast<kos_thread_t>(a0)));
        }
        case KOS_SYS_TASK_CREATE:
        {
            // Same shape as the cap creators: a task handle spends the whole word, so the
            // status is the return value and the handle rides an out-parameter -- and the
            // out-pointer is checked BEFORE the group exists, or a mint that cannot deliver
            // leaves a task nothing can name and nothing can kill.
            int rc = cap_out_check(a2);
            if (rc != 0)
            {
                return static_cast<uintptr_t>(rc);
            }
            kos_task_t h = KOS_TASK_NONE;
            rc = task_create_call(reinterpret_cast<void*>(a0), static_cast<size_t>(a1), &h);
            return cap_out_deliver(a2, rc, h);
        }
        case KOS_SYS_TASK_KILL:
        {
            // UNGATED by authority, gated by creatorship inside, exactly as the thread
            // cancel above is gated by parenthood.
            return static_cast<uintptr_t>(task_kill(static_cast<kos_task_t>(a0)));
        }
        case KOS_SYS_THREAD_JOIN:
        {
            // Blocks, so no dispatch IrqLock, same as SEND/RECV/CALL. Parenthood-gated
            // inside, like the cancel above.
            return static_cast<uintptr_t>(
                thread_join(static_cast<kos_thread_t>(a0), static_cast<uint32_t>(a1)));
        }
        case KOS_SYS_WAIT_LAST:
        {
            return static_cast<uintptr_t>(thread_wait_last());
        }
        case KOS_SYS_EXIT:
        {
            Thread* c = sched::current();
            // Root's exit ends the SYSTEM, through the same terminal path a returning main
            // takes: ending the system is a right root already holds (AUTH_SYSTEM), and
            // root's slot must never reach EXITED, since the pool, the domain table and the
            // boot arena are all sized for root holding its slot for the whole run, and the
            // reclaim sweep would strip the spawner_tag off every child root ever spawned.
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
            sched::exit_current(static_cast<int>(a0)); // noreturn
            return 0;
        }
        case KOS_SYS_SHUTDOWN:
        {
            // End the system through the shared terminal path. A syscall because it
            // is not reachable from an unprivileged thread. AUTH_SYSTEM: an ungated
            // shutdown would be a kill switch in every worker thread.
            Thread* c = sched::current();
            if (not cap_check_authority(c, AUTH_SYSTEM))
            {
                return static_cast<uintptr_t>(-KOS_EPERM);
            }
            kickos_terminate(static_cast<int>(a0)); // noreturn
            return 0;
        }
#if defined(KICKOS_ENABLE_SELFTEST)
        case KOS_SYS_REBOOT:
        {
            // AUTH_SYSTEM, fused with shutdown (docs/design-unprivileged-root.md 9).
            Thread* c = sched::current();
            if (not cap_check_authority(c, AUTH_SYSTEM))
            {
                return static_cast<uintptr_t>(-KOS_EPERM);
            }
            // Flush synchronously: the buffered console would lose its tail, and the
            // last lines are what tells a deliberate reboot apart from a hang.
            console_tx_flush_sync(); // empties the ring only
            // A byte still in the UART FIFO / shift register outruns the reset (the
            // RP2350 bootrom reboots after ~10 ms), truncating the tail.
            arch_console_flush_sync();
            return static_cast<uintptr_t>(arch_reboot());
        }
        case KOS_SYS_IRQ_INJECT:
        {
            // Test scaffolding only (real IRQs come from devices), so compiled out
            // of the production ABI (like guard_addr below). The line is
            // unprivileged-user-reachable: validate it at the boundary and reject a
            // bad value with -KOS_EINVAL rather than passing it to the controller. (Never
            // KICKOS_UNREACHABLE a user-supplied number: that would let a user
            // halt the kernel.)
            // Deliberately NOT privilege-gated (unlike irq_unmask/irq_attach): this
            // simulates a DEVICE firing, not an arm of the controller, and the tier-1
            // model has unprivileged drivers receive IRQs (selftest injects from an
            // unprivileged thread). Test-only + one-owner attach already bound the line.
            int irq = static_cast<int>(a0);
            if (irq < 0 or irq >= KICKOS_MAX_IRQ)
            {
                return static_cast<uintptr_t>(-KOS_EINVAL); // bad irq line
            }
            arch_irq_inject(irq);
            return 0;
        }
        case KOS_SYS_GUARD_ADDR:
        {
            return arch_mpu_probe_addr();
        }
        case KOS_SYS_IRQ_SPURIOUS:
        {
            return static_cast<uintptr_t>(irq_spurious_count());
        }
#if KICKOS_HAVE_MPU
        case KOS_SYS_GRANT_PROBE:
        {
            // Test scaffolding: exercise the Rule 7 grant predicates directly, so the
            // overlap arithmetic (equal / contained / partial / one-byte-edge / alias)
            // and the RAM/DEV admission rules are unit-testable without forging a real
            // MPU descriptor. Pure reads, no state change, so not privilege-gated (like
            // guard_addr). op selects the predicate + posture; the kernel supplies the
            // attr so userspace needs no ARCH_MPU_* enum. Compiled only under enforcement
            // (grant_hits_reserved / arch_reserved_blocks exist only then).
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
                    return static_cast<uintptr_t>(-KOS_EINVAL);
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
            // Test scaffolding: enable an UNBOUND line so an injected raise reaches
            // the default (spurious) handler on masked-by-default controllers (ARM
            // NVIC, RX), which else drop it. AUTH_IRQ (it arms a controller line),
            // like irq_attach.
            if (not cap_check_authority(sched::current(), AUTH_IRQ))
            {
                return static_cast<uintptr_t>(-KOS_EPERM); // arms a controller line
            }
            int irq = static_cast<int>(a0);
            if (irq < 0 or irq >= KICKOS_MAX_IRQ)
            {
                return static_cast<uintptr_t>(-KOS_EINVAL); // bad irq line
            }
            arch_irq_unmask(irq);
            return 0;
        }
#endif
        case KOS_SYS_IRQ_ATTACH:
        {
            // Tier-2 installs a privileged in-kernel handler, so AUTH_IRQ: a thread
            // without it cannot bind (or steal) a line's dispatch.
            if (not cap_check_authority(sched::current(), AUTH_IRQ))
            {
                return static_cast<uintptr_t>(-KOS_EPERM);
            }
            // Resolve + attach + unmask under one lock (like sem_wait/post): otherwise a
            // concurrent close between the resolve check and the attach could bind the
            // line to an already-dead handle.
            IrqLock lock;
            int irq = static_cast<int>(a0);
            uint32_t const cap_handle = static_cast<uint32_t>(a1);
            if (irq < 0 or irq >= KICKOS_MAX_IRQ)
            {
                return static_cast<uintptr_t>(-KOS_EINVAL); // bad irq line
            }
            // Resolve the CAP once, HERE (requires CAP_SIGNAL: an ISR posts), and store
            // the GLOBAL sem handle in the binding: irq_sem_post re-resolves that global
            // via the pool per fire (an ISR must NEVER resolve a cap: current() is a
            // random interrupted thread's table). The binding holds no ref, so a
            // last-close (now reachable via a thread exit) makes it a dead binding that
            // fails safe, not a wrong post.
            CapEntry* e = cap_lookup(sched::current(), cap_handle);
            if (e == nullptr or e->type != static_cast<uint8_t>(CapType::CAP_SEM)
                or kernel().sems.resolve(e->obj) == nullptr)
            {
                return static_cast<uintptr_t>(-KOS_EBADF); // bad / non-sem / stale cap
            }
            if ((e->rights & CAP_SIGNAL) != CAP_SIGNAL)
            {
                return static_cast<uintptr_t>(-KOS_EPERM); // cap lacks SIGNAL (an ISR posts)
            }
            int const sem_handle = e->obj;
            // irq_attach fails if the line is already owned: no stealing (EBUSY).
            if (not irq_attach(irq, irq_sem_post,
                               reinterpret_cast<void*>(static_cast<intptr_t>(sem_handle))))
            {
                return static_cast<uintptr_t>(-KOS_EBUSY);
            }
            // Enable the line: a userspace tier-2 binding has no separate unmask
            // syscall (tier-1 unmasks via register/irq_ack), so attach must arm it.
            // Required on default-masked controllers (ARM NVIC, RX); sim/riscv were
            // unmasked-by-default and only worked by that leniency. In-kernel
            // irq_attach (console) still unmasks on its own schedule, untouched.
            arch_irq_unmask(irq);
            return 0;
        }
        case KOS_SYS_CLOCK_NOW:
        {
            // Out-pointer for a 64-bit store: reject null and misalignment, then bound it
            // against the caller's writable regions: the kernel writes it privileged, so an
            // unprivileged caller must own it. The stub passes a stack local (in its stack
            // region); privileged callers bypass the ownership check. Closes the privileged-
            // kernel-writes-a-user-pointer hole. Alignment is alignof(uint64_t), arch-specific
            // (4 on RX, 8 on ARM/RISC-V) and what makes the typed store below well-defined.
            if (a0 == 0 or (a0 & (alignof(uint64_t) - 1)) != 0)
            {
                return static_cast<uintptr_t>(-KOS_EINVAL); // null or misaligned out-ptr
            }
            if (not user_writable_ok(a0, sizeof(uint64_t)))
            {
                return static_cast<uintptr_t>(-KOS_EFAULT); // out-ptr not owned by the caller
            }
            uint64_t const now = arch_clock_now();
            kaccess_to_user(a0, &now, sizeof(now));
            return 0;
        }
        case KOS_SYS_CPU_CLOCK_HZ:
        {
            // Read-only, no user pointer: the u32 fits a register, so return it
            // directly rather than via an out-ptr. Stays OUT of the -KOS_E* scheme:
            // it is a u32 Hz whose 0 sentinel already means unknown/no-silicon-clock.
            return static_cast<uintptr_t>(arch_cpu_clock_hz());
        }
        case KOS_SYS_PERIPH_CLOCK_HZ:
        {
            // Read-only branch-clock oracle: report the branch clock feeding the
            // register block at a0. Ungated (any thread), mirroring CPU_CLOCK_HZ:
            // a u32 Hz whose 0 sentinel already means unknown, OUT of the -KOS_E*
            // scheme. Cascade-free; a wrong value only garbles the caller's OWN
            // divisor math, and the caller granted that block anyway.
            return static_cast<uintptr_t>(arch_periph_clock_hz(a0));
        }
        case KOS_SYS_PINMUX_SET:
        {
            // One-shot init-time pin-function config (the clock->pinmux->gpio bring-up DAG
            // middle). AUTH_PINMUX: the mux registers live in the shared SCU/PORT block the
            // kernel keeps. a0=port, a1=pin, a2=func (all chip-opaque; neutrality is the
            // {port,pin,func} shape, not the encoding). Backend rejects kernel-owned pins.
            // IrqLock: the backends read-modify-write shared mux state unguarded (RX PMR
            // plus a chip-global PWPR unlock bracket, XMC IOCR, SAM ABSR), so a preempting
            // second caller silently drops the loser's write. Bounded: register writes only,
            // no backend waits.
            IrqLock lock;
            Thread* c = sched::current();
            if (not cap_check_authority(c, AUTH_PINMUX))
            {
                return static_cast<uintptr_t>(-KOS_EPERM);
            }
            return static_cast<uintptr_t>(arch_pinmux_set(a0, a1, a2));
        }
        case KOS_SYS_RAM_ALLOC:
        {
            // AUTH_MEMORY: domains are carved by the setup path, not by arbitrary user
            // threads (avoids a DoS on the shared pool and matches static-allocation-first).
            // IrqLock: arch_ram_alloc does an unguarded read-modify-write of the bump
            // pointer.
            // POINTER return: OUT of the -KOS_E* scheme. A negative errno cast to
            // void* would be a non-NULL pointer. EVERY failure path returns 0 (NULL) so
            // the documented `if (p == NULL)` check is correct: the not-permitted reject
            // and the arena-exhausted reject both yield NULL (arch_ram_alloc already
            // returns 0 when exhausted).
            IrqLock lock;
            if (not cap_check_authority(sched::current(), AUTH_MEMORY))
            {
                return 0; // NULL, not (uintptr_t)-1
            }
            return reinterpret_cast<uintptr_t>(
                arch_ram_alloc(static_cast<size_t>(a0)));
        }
        case KOS_SYS_MEM_SELF_GRANT:
        {
            // The explicit half of allocate-then-grant: KOS_SYS_RAM_ALLOC reserves arena
            // memory and grants nothing.
            //
            // Added to the CALLER's own region set, not to its domain: a domain is
            // shared, and widening it would silently hand the same window to every
            // sibling thread.
            IrqLock lock;
            Thread* const c = sched::current();
            if (c == nullptr or not cap_check_authority(c, AUTH_MEMORY))
            {
                return static_cast<uintptr_t>(-KOS_EPERM);
            }
            uintptr_t const base = a0;
            size_t const size = static_cast<size_t>(a1);
            if (size == 0 or (base + size) < base)
            {
                return static_cast<uintptr_t>(-KOS_EINVAL);
            }
            // Already reachable costs no descriptor: the call is idempotent, and a
            // privileged caller (whole-arena region) always lands here.
            if (user_range_ok(base, size, ARCH_MPU_R | ARCH_MPU_W))
            {
                return 0;
            }
            // Rule 7, the same admission a spawn-time data grant takes, on the
            // geometry that will actually be committed: a window rounded up AFTER
            // admission could cover a neighbour the unrounded extent did not.
            size_t const rsz = arch_ram_region_size(size);
            if (rsz == 0)
            {
                return static_cast<uintptr_t>(-KOS_EINVAL);
            }
            // Nameable by one descriptor, the same admission the stack grant takes
            // (syscall_thread.cc): PMSAv7 MPU_RBAR MASKS the base down to the region size,
            // so an unaligned base would be programmed as a window starting BELOW what the
            // caller named. On a no-MPU arch it still demands a 16-aligned base.
            if (not arch_ram_region_admissible(base, rsz))
            {
                return static_cast<uintptr_t>(-KOS_EINVAL);
            }
            if (not grant_region_admissible(base, rsz, ARCH_MPU_R | ARCH_MPU_W,
                                            cap_check_authority(c, AUTH_MEMORY)))
            {
                return static_cast<uintptr_t>(-KOS_EPERM);
            }
            // Full budget is a returned error; truncating the set instead would fault
            // the thread on memory it was told it had. NOT -KOS_EMFILE: that code names the
            // capability table, and the knob here is KICKOS_MPU_MAX_REGIONS.
            if (c->region_count >= KICKOS_MPU_MAX_REGIONS)
            {
                return static_cast<uintptr_t>(-KOS_ENOMEM);
            }
            c->regions[c->region_count].base = base;
            c->regions[c->region_count].size = rsz;
            c->regions[c->region_count].attr = ARCH_MPU_R | ARCH_MPU_W;
            c->region_count++;
            // Must be effective BEFORE the return: the caller's next instruction may
            // dereference the region, and on a deferred-switch arch apply() only
            // STASHES; the commit is what programs the hardware
            // (docs/design-mpu-commit-deferred.md). Sound because no switch is
            // involved: outgoing and incoming are the same thread.
            arch_mpu_apply(c->regions, c->region_count);
            kickos_arch_mpu_commit();
            return 0;
        }
        case KOS_SYS_PERIPH_ENABLE:
        {
            // Possession, not an authority bit: holding the window is the right to
            // enable the device.
            IrqLock lock;
            if (not caller_holds_mmio_block(a0))
            {
                return static_cast<uintptr_t>(-KOS_EPERM);
            }
            return static_cast<uintptr_t>(arch_periph_enable(a0));
        }
        case KOS_SYS_PERIPH_REG_WRITE:
        {
            // Malformed request before possession: the store is one 32-bit word, so an
            // unaligned or wrapping target is -KOS_EINVAL whatever the caller holds.
            if ((a1 & (sizeof(uint32_t) - 1u)) != 0)
            {
                return static_cast<uintptr_t>(-KOS_EINVAL);
            }
            uintptr_t const top = ~static_cast<uintptr_t>(0);
            if (a1 > top - a0 or (a0 + a1) > top - (sizeof(uint32_t) - 1u))
            {
                return static_cast<uintptr_t>(-KOS_EINVAL);
            }
            // Possession of the block at a0 AND of the word at a0+a1 inside it. The
            // allowlist bounds which registers of a held block are writable; it is not a
            // bound on the ADDRESS, so without containment a 32-byte window would reach
            // any register the table names anywhere in the block.
            IrqLock lock;
            if (not caller_holds_mmio_reg(a0, a1))
            {
                return static_cast<uintptr_t>(-KOS_EPERM);
            }
            return static_cast<uintptr_t>(
                arch_periph_reg_write(a0, a1, static_cast<uint32_t>(a2)));
        }
        case KOS_SYS_CAP_NARROW:
        {
            // UNGATED, and it has to be: giving up authority you hold needs no authority,
            // and a gate would be a bit a thread must keep in order to drop the others.
            // It can only clear bits in the CALLER's own table.
            IrqLock lock;
            return static_cast<uintptr_t>(
                cap_narrow_authority(sched::current(), static_cast<uint32_t>(a0),
                                     static_cast<uint8_t>(a1)));
        }
        case KOS_SYS_PANIC:
        {
            // UNGATED, and it has to be: kpanic masks IRQs and reads kernel .bss, so a
            // thread that called it from its own unprivileged frame would fault there
            // and lose the diagnostic. An authority bit would make that the default for
            // any thread that had dropped it.
            user_panic(a0); // noreturn
            return 0;
        }
        case KOS_SYS_IRQ_CLAIM:
        {
            // The tier-1 mint takes a bare line number out of the namespace and makes it
            // owned, so it is gated like IRQ_ATTACH and IRQ_UNMASK. USING an already-claimed
            // line needs no authority: possession of the cap is the authorisation, checked
            // in cap_resolve_e.
            if (not cap_check_authority(sched::current(), AUTH_IRQ))
            {
                return static_cast<uintptr_t>(-KOS_EPERM); // claims a line namespace-wide
            }
            int rc = cap_out_check(a2);
            if (rc != 0)
            {
                return static_cast<uintptr_t>(rc);
            }
            uint32_t h = KCAP_INVALID;
            rc = irq_claim(sched::current(), static_cast<int>(a0),
                           static_cast<unsigned int>(a1), &h);
            return cap_out_deliver(a2, rc, h);
        }
        case KOS_SYS_IRQ_WAIT:
        {
            return static_cast<uintptr_t>(irq_wait(sched::current(), static_cast<uint32_t>(a0)));
        }
        case KOS_SYS_IRQ_ACK:
        {
            return static_cast<uintptr_t>(irq_ack(sched::current(), static_cast<uint32_t>(a0)));
        }
        case KOS_SYS_IRQ_NOTIFY:
        {
            return static_cast<uintptr_t>(irq_notify(sched::current(), static_cast<uint32_t>(a0)));
        }
        case KOS_SYS_IRQ_DISCARD:
        {
            return static_cast<uintptr_t>(irq_discard(sched::current(), static_cast<uint32_t>(a0)));
        }
        case KOS_SYS_DIAG_LED_SET:
        {
            // Benign single LED (the kernel's diagnostic pin, borrowed): left
            // unprivileged like the console. A no-op on boards with no LED.
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
            // Unknown syscall from userspace is a caller error, not a kernel
            // invariant violation: fault the caller (EINVAL), never panic the kernel.
            return static_cast<uintptr_t>(-KOS_EINVAL);
        }
    }
}
}
