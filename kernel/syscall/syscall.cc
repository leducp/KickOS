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
#include <kickos/console_tx.h>

#include <kickos/sys/abi.h>

#include "syscall_internal.h"

#include <span>

namespace kickos
{
    // syscall_dispatch answers 8 bytes on every target and the userspace stub narrows that
    // to a fixed 4, so the byte-count producers must already be 4 bytes here. Matching
    // static_assert in user/src/syscall_stubs.cc.
    static_assert(sizeof(endpoint_send(0, 0, 0, 0)) == 4, "must be exactly 4 bytes");
    static_assert(sizeof(endpoint_recv(0, 0, 0, 0, false)) == 4, "must be exactly 4 bytes");
    static_assert(sizeof(endpoint_call(0, 0, 0, 0, 0)) == 4, "must be exactly 4 bytes");

    namespace
    {
        // Max yield passes kos_console_publish waits for the in-flight chip-writer count
        // to reach 0 before declaring a stuck writer.
        constexpr uint32_t CONSOLE_PUBLISH_DRAIN_MAX = KICKOS_POLL_SPIN_MAX;

        // Bound by KOS_SYS_IRQ_ATTACH and run in ISR context. arg is the GLOBAL sem handle
        // irq_attach stored, NOT a cap: an ISR must never resolve a cap, current() being a
        // random interrupted thread's table.
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
        uint64_t cap_out_deliver(uintptr_t out, int rc, uint32_t handle)
        {
            if (rc == 0)
            {
                kaccess_to_user(out, &handle, sizeof(handle));
            }
            return static_cast<uint64_t>(rc);
        }

        // noinline is load-bearing: the message buffer must not widen syscall_dispatch's
        // frame, which sits on the CALLING thread's stack and is sized by
        // KICKOS_MIN_STACK_SIZE against the deepest ordinary dispatch.
        __attribute__((noinline, noreturn)) void user_panic(uintptr_t msg)
        {
            char buf[64];
            buf[0] = '\0';
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
                    kaccess_from_user(&buf[i], msg + i, 1);
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

// THE death point of a cancelled thread. A cancel breaks whatever park the target is in, so
// it returns to userspace with -KOS_ECANCELED and gets ONE window to clean up over memory it
// already holds; the next time it asks the kernel for anything, it ends here instead. The
// only survivor is a thread that never enters the kernel again.
//
// Checked on ENTRY and never on exit: on exit it would pre-empt that cleanup window.
extern "C" uint64_t syscall_dispatch(uintptr_t nr,
                                     uintptr_t a0, uintptr_t a1,
                                     uintptr_t a2, uintptr_t a3)
{
    Thread* const caller = sched::current();
    if (caller != nullptr and caller->cancel_kind != CANCEL_NONE and not caller->dying)
    {
        sched::exit_current(KOS_EXIT_CANCELLED); // noreturn
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
            // MMU-era NOTE: this hands a user pointer straight to kconsole_write, which
            // streams it privileged. The one kernel-side user read NOT funnelled through
            // kaccess_from_user.
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
                return static_cast<uint64_t>(-KOS_EFAULT);
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
                return static_cast<uint64_t>(rc);
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
            // reintroduce the stale read on ARM. The resolve-to-call window needs none: the
            // caller's own cap pins the mutex (mutex_refs >= 1) and no cross-thread
            // close/kill path can free it. need == 0: possession is the authority.
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
        case KOS_SYS_RECV:
        {
            return static_cast<uint64_t>(
                endpoint_recv(static_cast<uint32_t>(a0), a1, static_cast<size_t>(a2), a3,
                              /*timed=*/false));
        }
        case KOS_SYS_RECV_TIMED:
        {
            // The deadline is not an argument: a3 names a kos_recv_timed_opts holding it,
            // with the ordinary kos_recv_info nested inside.
            return static_cast<uint64_t>(
                endpoint_recv(static_cast<uint32_t>(a0), a1, static_cast<size_t>(a2), a3,
                              /*timed=*/true));
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
                // Resolve to the GLOBAL gen-encoded handle, NOT the pool index (S3).
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
                if (console_owner_is_kernel() != 0)
                {
                    console_tx_deinit(); // idempotent; skipped on re-publish
                }
                console_owner_set_user();    // must be LAST
            }
            // Drains, with the lock RELEASED, any chip writer that raced past the pre-flip
            // state read. Root spawns the driver only after this returns, so the preempted
            // writer is off the device before the driver touches it.
            //
            // A bare busy-spin here LIVELOCKS under strict priority: a writer preempted mid
            // arch_console_write_sync (a polled loop run WITHOUT IrqLock) can only finish
            // once rescheduled, and it may be LOWER priority than this publisher. Hence the
            // drop to the minimum real priority plus a yield each pass.
            Thread* pub = sched::current();
            uint8_t const saved_prio = pub->prio;
            sched::set_prio(pub, KICKOS_PRIO_MIN);
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
            // AUTH_PSTATE: it mutates SystemCoreClock, retimes every thread's SysTick
            // basis and moves the shared console baud. The coherence sequence (mask /
            // disarm / flush / retune / re-arm) lives in cpu_clock_set.
            // OUT of the -KOS_E* scheme: it returns a u32 Hz whose 0 sentinel already means
            // cannot/unsupported/not-permitted, so the unprivileged path returns 0 too and
            // the console-owned refusal surfaces as "unchanged Hz".
            Thread* c = sched::current();
            if (not cap_check_authority(c, AUTH_PSTATE))
            {
                return 0;
            }
            return static_cast<uint64_t>(
                cpu_clock_set(static_cast<kos_pstate_t>(a0)));
        }
        case KOS_SYS_THREAD_SPAWN:
        {
            // Checked BEFORE the child is created: a spawn that cannot deliver its handle
            // leaves a thread nothing can name or kill.
            int rc = cap_out_check(a1);
            if (rc != 0)
            {
                return static_cast<uint64_t>(rc);
            }
            kos_thread_t h = KOS_THREAD_NONE;
            rc = thread_spawn(reinterpret_cast<kos_thread_params const*>(a0), &h);
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
            // Root's exit ends the SYSTEM, through the same terminal path a returning main
            // takes. Root's slot must never reach EXITED: the pool, the domain table and the
            // boot arena are all sized for root holding it for the whole run, and the
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
            // AUTH_SYSTEM, fused with shutdown (docs/design-unprivileged-root.md 9).
            Thread* c = sched::current();
            if (not cap_check_authority(c, AUTH_SYSTEM))
            {
                return static_cast<uint64_t>(-KOS_EPERM);
            }
            // Flush synchronously or the buffered console loses its tail.
            console_tx_flush_sync(); // empties the ring only
            // A byte still in the UART FIFO / shift register outruns the reset (the
            // RP2350 bootrom reboots after ~10 ms), truncating the tail.
            arch_console_flush_sync();
            return static_cast<uint64_t>(arch_reboot());
        }
        case KOS_SYS_IRQ_INJECT:
        {
            // Test scaffolding, compiled out of the production ABI. Never
            // KICKOS_UNREACHABLE a user-supplied number: that would let a user halt the
            // kernel. NOT privilege-gated, unlike irq_unmask/irq_attach: this simulates a
            // DEVICE firing, and selftest injects it from an unprivileged thread.
            int irq = static_cast<int>(a0);
            if (irq < 0 or irq >= KICKOS_MAX_IRQ)
            {
                return static_cast<uint64_t>(-KOS_EINVAL); // bad irq line
            }
            arch_irq_inject(irq);
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
#if KICKOS_HAVE_MPU
        case KOS_SYS_GRANT_PROBE:
        {
            // Test scaffolding for the Rule 7 grant predicates. Pure reads, so not
            // privilege-gated. op selects the predicate and posture; the kernel supplies the
            // attr, so userspace needs no ARCH_MPU_* enum. Compiled only under enforcement
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
            arch_irq_unmask(irq);
            return 0;
        }
#endif
        case KOS_SYS_IRQ_ATTACH:
        {
            // Tier-2 installs a privileged in-kernel handler, so AUTH_IRQ: a thread without
            // it cannot bind, or steal, a line's dispatch.
            if (not cap_check_authority(sched::current(), AUTH_IRQ))
            {
                return static_cast<uint64_t>(-KOS_EPERM);
            }
            // Resolve, attach and unmask under one lock: a concurrent close between the
            // resolve check and the attach could otherwise bind the line to a dead handle.
            IrqLock lock;
            int irq = static_cast<int>(a0);
            uint32_t const cap_handle = static_cast<uint32_t>(a1);
            if (irq < 0 or irq >= KICKOS_MAX_IRQ)
            {
                return static_cast<uint64_t>(-KOS_EINVAL); // bad irq line
            }
            // The binding stores the GLOBAL sem handle, not the cap, and irq_sem_post
            // re-resolves that global per fire: an ISR must NEVER resolve a cap. CAP_SIGNAL
            // is required here because an ISR posts. The binding holds no reference, so a
            // last-close leaves a dead binding that fails safe, not a wrong post.
            CapEntry* e = cap_lookup(sched::current(), cap_handle);
            if (e == nullptr or e->type != static_cast<uint8_t>(CapType::CAP_SEM)
                or kernel().sems.resolve(e->obj) == nullptr)
            {
                return static_cast<uint64_t>(-KOS_EBADF); // bad / non-sem / stale cap
            }
            if ((e->rights & CAP_SIGNAL) != CAP_SIGNAL)
            {
                return static_cast<uint64_t>(-KOS_EPERM); // cap lacks SIGNAL (an ISR posts)
            }
            int const sem_handle = e->obj;
            // irq_attach fails if the line is already owned: no stealing (EBUSY).
            if (not irq_attach(irq, irq_sem_post,
                               reinterpret_cast<void*>(static_cast<intptr_t>(sem_handle))))
            {
                return static_cast<uint64_t>(-KOS_EBUSY);
            }
            // Required on default-masked controllers (ARM NVIC, RX): a userspace tier-2
            // binding has no separate unmask syscall (tier-1 unmasks via register/irq_ack),
            // so attach must arm the line. In-kernel irq_attach (console) unmasks on its own
            // schedule.
            arch_irq_unmask(irq);
            return 0;
        }
        case KOS_SYS_CLOCK_NOW:
        {
            // No user pointer and no way to fail: every value in the u64 range is a valid
            // instant, so this arm is OUT of the -KOS_E* scheme.
            return arch_clock_now();
        }
        case KOS_SYS_CPU_CLOCK_HZ:
        {
            // OUT of the -KOS_E* scheme: a u32 Hz whose 0 sentinel already means
            // unknown / no silicon clock.
            return static_cast<uint64_t>(arch_cpu_clock_hz());
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
            if (not cap_check_authority(sched::current(), AUTH_MEMORY))
            {
                return 0; // NULL, not (uintptr_t)-1
            }
            return reinterpret_cast<uintptr_t>(
                arch_ram_alloc(static_cast<size_t>(a0)));
        }
        case KOS_SYS_MEM_SELF_GRANT:
        {
            // KOS_SYS_RAM_ALLOC reserves arena memory and grants nothing; this is the
            // grant half.
            //
            // Added to the CALLER's own region set, not to its domain: a domain is shared,
            // and widening it would hand the same window to every sibling thread.
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
            // Already reachable costs no descriptor. EXCEPT where the chip PROGRAMS the
            // memory type: privileged reach comes from the CACHEABLE background map, so a
            // block initialised through it keeps dirty lines a bus master then reads.
            bool already = user_range_ok(base, size, ARCH_MPU_R | ARCH_MPU_W);
            if ((attr & ARCH_MPU_NOCACHE) != 0 and arch_mpu_nocache_support() == ARCH_MPU_NOCACHE_PROGRAMMED)
            {
                already = user_range_typed_ok(base, size, attr);
            }
            if (already)
            {
                return 0;
            }
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
            // Full budget is a returned error; truncating the set would fault the thread on
            // memory it was told it had. NOT -KOS_EMFILE: that code names the capability
            // table, and the knob here is KICKOS_MPU_MAX_REGIONS.
            if (c->region_count >= KICKOS_MPU_MAX_REGIONS)
            {
                return static_cast<uint64_t>(-KOS_ENOMEM);
            }
            c->regions[c->region_count].base = base;
            c->regions[c->region_count].size = rsz;
            c->regions[c->region_count].attr = attr;
            c->region_count++;
            // Must be effective BEFORE the return: the caller's next instruction may
            // dereference the region, and on a deferred-switch arch apply() only STASHES,
            // the commit being what programs the hardware
            // (docs/design-mpu-commit-deferred.md).
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
        case KOS_SYS_IRQ_CLAIM:
        {
            // AUTH_IRQ, like IRQ_ATTACH and IRQ_UNMASK: the tier-1 mint takes a bare line
            // number out of the namespace and makes it owned. USING an already-claimed line
            // needs no authority; possession of the cap is the authorisation, checked in
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
            // peripheral, so an app calling them directly runs them at ITS privilege and
            // faults (root is unprivileged on every board). Ungated, like KOS_SYS_IRQ_INJECT.
            //
            // Both prints run HERE, in thread context and holding no IrqLock.
            switch (a0)
            {
                case KOS_BENCH_OP_RESET:
                {
                    bench_reset();
                    return 0;
                }
                case KOS_BENCH_OP_CORE_HZ:
                {
                    return bench_core_hz();
                }
                case KOS_BENCH_OP_SWITCH_PRINT:
                {
                    return bench_switch_print();
                }
                case KOS_BENCH_OP_IRQ_SETUP:
                {
                    int const line = static_cast<int>(a1);
                    if (line < 0 or line >= KICKOS_MAX_IRQ)
                    {
                        return static_cast<uint64_t>(-KOS_EINVAL);
                    }
                    bench_irq_setup(line);
                    return 0;
                }
                case KOS_BENCH_OP_IRQ_ONCE:
                {
                    int const line = static_cast<int>(a1);
                    if (line < 0 or line >= KICKOS_MAX_IRQ)
                    {
                        return static_cast<uint64_t>(-KOS_EINVAL);
                    }
                    return bench_irq_once(line);
                }
                case KOS_BENCH_OP_IRQ_MASKED_ONCE:
                {
                    int const line = static_cast<int>(a1);
                    if (line < 0 or line >= KICKOS_MAX_IRQ)
                    {
                        return static_cast<uint64_t>(-KOS_EINVAL);
                    }
                    return bench_irq_masked_once(line, static_cast<uint32_t>(a2));
                }
                case KOS_BENCH_OP_PHASE_PRINT:
                {
                    bench_phase_print();
                    return 0;
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
