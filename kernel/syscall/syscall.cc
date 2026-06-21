// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The arch-independent syscall table + dispatch. The arch entry (sim
// trampoline / ARM SVC handler) reads the number + args and calls
// syscall_dispatch(); the result is delivered back to the caller frame.
// Kernel objects addressable from userspace (semaphores, threads) live in
// static pools referenced by small integer handles — no pointers cross the
// boundary.

#include <kickos/arch/arch.h>
#include <kickos/config.h>
#include <kickos/instance.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/time.h>
#include <kickos/kernel.h>
#include <kickos/irq.h>
#include <kickos/irqlock.h>

#include <kickos/sys/abi.h>

namespace kickos
{
    namespace
    {
        // --- Semaphore registry (instance-scoped freelist) -------------------------
        // A handle packs the slot index (low bits) + a generation (high bits). On
        // destroy the slot's generation is bumped, so a handle to a since-recycled
        // slot fails to resolve -- the fail-loud fix for id reuse (ABA). The handle
        // is opaque to userspace; sem_resolve() is the single validate-and-resolve
        // chokepoint the M2 capability model (12b) later swaps for a handle table.
        constexpr int kSemIndexBits = 8;
        static_assert(KICKOS_MAX_SEMAPHORES <= (1 << kSemIndexBits),
                      "sem handle index field too small for KICKOS_MAX_SEMAPHORES");

        int sem_make_handle(int index, uint16_t gen)
        {
            return static_cast<int>((static_cast<uint32_t>(gen) << kSemIndexBits) |
                                    static_cast<uint32_t>(index));
        }

        Semaphore* sem_resolve(int handle)
        {
            if (handle < 0)
            {
                return nullptr;
            }
            int index = handle & ((1 << kSemIndexBits) - 1);
            uint16_t gen = static_cast<uint16_t>(static_cast<uint32_t>(handle) >> kSemIndexBits);
            Kernel& k = kernel();
            if (index >= KICKOS_MAX_SEMAPHORES or not k.sem_used[index] or k.sem_gen[index] != gen)
            {
                return nullptr;
            }
            return &k.sems[index];
        }

        int sem_create(int initial)
        {
            IrqLock lock;
            Kernel& k = kernel();
            for (int i = 0; i < KICKOS_MAX_SEMAPHORES; i++)
            {
                if (not k.sem_used[i])
                {
                    k.sem_used[i] = true;
                    sem_init(&k.sems[i], initial);
                    return sem_make_handle(i, k.sem_gen[i]);
                }
            }
            return -1;
        }

        int sem_destroy(int handle)
        {
            IrqLock lock;
            Semaphore* s = sem_resolve(handle);
            if (s == nullptr)
            {
                return -1;
            }
            // Quiescent-only: refuse while waiters are parked (waking them with an
            // error needs the wait_result channel timed wait adds -- Later).
            if (not s->waiters.empty())
            {
                return -1;
            }
            int index = handle & ((1 << kSemIndexBits) - 1);
            kernel().sem_gen[index]++; // invalidate outstanding handles to this slot
            kernel().sem_used[index] = false;
            return 0;
        }

        // Privileged in-kernel IRQ handler bound by KOS_SYS_irq_attach: posts a
        // semaphore from ISR context, driving the interrupt-exit switch (trigger #4).
        void irq_sem_post(void* arg)
        {
            int handle = static_cast<int>(reinterpret_cast<intptr_t>(arg));
            Semaphore* s = sem_resolve(handle);
            if (s != nullptr)
            {
                sem_post(s);
            }
        }

        // --- Thread pool (instance-scoped; static allocation, kernel stacks) -------
        // Bump-allocated: EXITED slots are not reclaimed (a system spawns at most
        // KICKOS_MAX_THREADS ever). Freelisting this (like the sem pool, 8m) waits
        // for thread teardown -- and must revisit the "dead ctx keeps raised>0 is
        // harmless because slots never recycle" assumption before reuse.
        int thread_spawn(kos_thread_params const* p)
        {
            IrqLock lock;
            if (p == nullptr)
            {
                return -1;
            }
            // Validate the user-supplied priority: it indexes the ready lists and
            // drives a 1u<<prio bitmap shift, so an out-of-range value is an OOB write / UB.
            // Priority 0 is reserved for the idle thread.
            if (p->prio < KICKOS_PRIO_MIN or p->prio > KICKOS_PRIO_MAX)
            {
                return -1;
            }
            // No privilege escalation: only a privileged thread may spawn one (a
            // privileged thread is granted the whole arena). The granted domain
            // region's geometry is validated arch-side in arch_mpu_apply.
            if (p->privileged != 0 and not sched::current()->privileged)
            {
                return -1;
            }
            Kernel& k = kernel();
            if (k.thread_next >= KICKOS_MAX_THREADS)
            {
                return -1;
            }
            int i = k.thread_next++;

            ThreadAttr attr;
            attr.name = "user";
            if (p->name != nullptr)
            {
                attr.name = p->name;
            }
            attr.prio = p->prio;
            attr.policy = Policy::FIFO;
            if (p->policy == KOS_POLICY_RR)
            {
                attr.policy = Policy::RR;
            }
            attr.quantum_ns = p->quantum_ns;
            attr.privileged = (p->privileged != 0);
            attr.mem_base = p->mem_base;
            attr.mem_size = p->mem_size;

            thread_create(&k.thread_pool[i], p->entry, p->arg,
                          k.thread_stacks[i], KICKOS_USER_STACK_SIZE, attr);
            return i;
        }

    }
}

using namespace kickos;

extern "C" uintptr_t syscall_dispatch(uintptr_t nr,
                                      uintptr_t a0, uintptr_t a1,
                                      uintptr_t a2, uintptr_t a3)
{
    (void)a2; // unused by the current syscalls (all take <= 2 args or an out-ptr)
    (void)a3;
    switch (nr)
    {
        case KOS_SYS_kconsole_write:
        {
            // Explicit (buf, len): the kernel must never strlen a user pointer.
            // Bound-checking buf against the caller's regions is M2 (item 12).
            char const* buf = reinterpret_cast<char const*>(a0);
            size_t len = static_cast<size_t>(a1);
            arch_console_write(buf, len);
            return len;
        }
        case KOS_SYS_yield:
        {
            sched::yield();
            return 0;
        }
        case KOS_SYS_sleep_ns:
        {
            ktime_sleep_ns(kos_u64_join(static_cast<uint32_t>(a0),
                                        static_cast<uint32_t>(a1)));
            return 0;
        }
        case KOS_SYS_sem_create:
        {
            return static_cast<uintptr_t>(sem_create(static_cast<int>(a0)));
        }
        case KOS_SYS_sem_destroy:
        {
            return static_cast<uintptr_t>(sem_destroy(static_cast<int>(a0)));
        }
        case KOS_SYS_sem_wait:
        {
            // Resolve and use under one lock (sem_wait/sem_post nest their own):
            // otherwise a concurrent sem_destroy could free the slot between
            // resolve and use, defeating the quiescent-only guarantee.
            IrqLock lock;
            Semaphore* s = sem_resolve(static_cast<int>(a0));
            if (s == nullptr)
            {
                return static_cast<uintptr_t>(-1);
            }
            sem_wait(s);
            return 0;
        }
        case KOS_SYS_sem_post:
        {
            IrqLock lock;
            Semaphore* s = sem_resolve(static_cast<int>(a0));
            if (s == nullptr)
            {
                return static_cast<uintptr_t>(-1);
            }
            sem_post(s);
            return 0;
        }
        case KOS_SYS_thread_spawn:
        {
            return static_cast<uintptr_t>(
                thread_spawn(reinterpret_cast<kos_thread_params const*>(a0)));
        }
        case KOS_SYS_exit:
        {
            sched::exit_current(static_cast<int>(a0)); // noreturn
            return 0;
        }
        case KOS_SYS_irq_inject:
        {
            arch_irq_inject(static_cast<int>(a0));
            return 0;
        }
#if defined(KICKOS_ENABLE_SELFTEST)
        case KOS_SYS_guard_addr:
        {
            return arch_mpu_probe_addr();
        }
#endif
        case KOS_SYS_irq_attach:
        {
            // Tier-2 installs a privileged in-kernel handler: privileged-only, so
            // an unprivileged thread cannot bind (or steal) a line's dispatch.
            if (not sched::current()->privileged)
            {
                return static_cast<uintptr_t>(-1);
            }
            int irq = static_cast<int>(a0);
            int sem_handle = static_cast<int>(a1);
            if (irq < 0 or irq >= KICKOS_MAX_IRQ or sem_resolve(sem_handle) == nullptr)
            {
                return static_cast<uintptr_t>(-1);
            }
            // Store the handle (not a pointer): irq_sem_post re-resolves each fire,
            // so a since-destroyed sem fails safe instead of poking a stale slot.
            irq_attach(irq, irq_sem_post,
                       reinterpret_cast<void*>(static_cast<intptr_t>(sem_handle)));
            return 0;
        }
        case KOS_SYS_clock_now:
        {
            if (a0 == 0)
            {
                return static_cast<uintptr_t>(-1);
            }
            *reinterpret_cast<uint64_t*>(a0) = arch_clock_now();
            return 0;
        }
        case KOS_SYS_ram_alloc:
        {
            // Privileged-only: domains are carved by the privileged setup path,
            // not by arbitrary user threads (avoids a DoS on the shared pool and
            // matches static-allocation-first). IrqLock: arch_ram_alloc does an
            // unguarded read-modify-write of the bump pointer.
            IrqLock lock;
            if (not sched::current()->privileged)
            {
                return static_cast<uintptr_t>(-1);
            }
            return reinterpret_cast<uintptr_t>(
                arch_ram_alloc(static_cast<size_t>(a0)));
        }
        case KOS_SYS_irq_register:
        {
            return static_cast<uintptr_t>(irq_register(static_cast<int>(a0)));
        }
        case KOS_SYS_irq_wait:
        {
            return static_cast<uintptr_t>(irq_wait(static_cast<int>(a0)));
        }
        case KOS_SYS_irq_ack:
        {
            return static_cast<uintptr_t>(irq_ack(static_cast<int>(a0)));
        }
        default:
        {
            // Unknown syscall from userspace is a caller error, not a kernel
            // invariant violation: fault the caller (-1), never panic the kernel.
            return static_cast<uintptr_t>(-1);
        }
    }
}
