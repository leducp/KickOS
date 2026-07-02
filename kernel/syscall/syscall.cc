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

        // --- Semaphore registry ----------------------------------------------------
        Semaphore g_sems[KICKOS_MAX_SEMAPHORES];
        int g_sem_count = 0;

        int sem_create(int initial)
        {
            IrqLock lock;
            if (g_sem_count >= KICKOS_MAX_SEMAPHORES) return -1;
            int id = g_sem_count++;
            sem_init(&g_sems[id], initial);
            return id;
        }

        bool sem_valid(int id) { return id >= 0 && id < g_sem_count; }

        // Privileged in-kernel IRQ handler bound by KOS_SYS_irq_attach: posts a
        // semaphore from ISR context, driving the interrupt-exit switch (trigger #4).
        void irq_sem_post(void* arg)
        {
            int id = static_cast<int>(reinterpret_cast<intptr_t>(arg));
            if (sem_valid(id)) sem_post(&g_sems[id]);
        }

        // --- Thread pool (static allocation; kernel-provided stacks) ---------------
        constexpr int kPoolSize = 16;
        constexpr size_t kUserStack = 64 * 1024;

        Thread g_pool[kPoolSize];
        alignas(16) unsigned char g_pool_stacks[kPoolSize][kUserStack];
        int g_pool_next = 0;

        int thread_spawn(kos_thread_params const* p)
        {
            IrqLock lock;
            if (p == nullptr) return -1;
            // Validate the user-supplied priority: it indexes g_ready[] and drives
            // a 1u<<prio shift, so an out-of-range value is an OOB write / UB.
            // Priority 0 is reserved for the idle thread.
            if (p->prio < KICKOS_PRIO_MIN || p->prio > KICKOS_PRIO_MAX) return -1;
            if (g_pool_next >= kPoolSize) return -1;
            int i = g_pool_next++;

            ThreadAttr attr;
            attr.name = "user";
            if (p->name != nullptr) attr.name = p->name;
            attr.prio = p->prio;
            attr.policy = Policy::FIFO;
            if (p->policy == KOS_POLICY_RR) attr.policy = Policy::RR;
            attr.quantum_ns = p->quantum_ns;
            attr.privileged = (p->privileged != 0);

            thread_create(&g_pool[i], p->entry, p->arg,
                          g_pool_stacks[i], kUserStack, attr);
            return i;
        }

    }
}

using namespace kickos;

extern "C" uintptr_t syscall_dispatch(uintptr_t nr,
                                      uintptr_t a0, uintptr_t a1,
                                      uintptr_t a2, uintptr_t a3)
{
    (void)a3;
    switch (nr)
    {
        case KOS_SYS_write:
        {
            int fd = static_cast<int>(a0);
            char const* buf = reinterpret_cast<char const*>(a1);
            size_t len = static_cast<size_t>(a2);
            if (fd == 1 || fd == 2)
            {
                arch_console_write(buf, len);
                return len;
            }
            return static_cast<uintptr_t>(-1);
        }
        case KOS_SYS_yield:
            sched::yield();
            return 0;
        case KOS_SYS_sleep_ns:
            ktime_sleep_ns(kos_u64_join(static_cast<uint32_t>(a0),
                                        static_cast<uint32_t>(a1)));
            return 0;
        case KOS_SYS_sem_create:
            return static_cast<uintptr_t>(sem_create(static_cast<int>(a0)));
        case KOS_SYS_sem_wait:
        {
            int id = static_cast<int>(a0);
            if (!sem_valid(id)) return static_cast<uintptr_t>(-1);
            sem_wait(&g_sems[id]);
            return 0;
        }
        case KOS_SYS_sem_post:
        {
            int id = static_cast<int>(a0);
            if (!sem_valid(id)) return static_cast<uintptr_t>(-1);
            sem_post(&g_sems[id]);
            return 0;
        }
        case KOS_SYS_thread_spawn:
            return static_cast<uintptr_t>(
                thread_spawn(reinterpret_cast<kos_thread_params const*>(a0)));
        case KOS_SYS_exit:
            sched::exit_current(); // noreturn
            return 0;
        case KOS_SYS_irq_inject:
            arch_irq_inject(static_cast<int>(a0));
            return 0;
        case KOS_SYS_guard_addr:
            return arch_mpu_probe_addr();
        case KOS_SYS_irq_attach:
            irq_attach(static_cast<int>(a0), irq_sem_post,
                       reinterpret_cast<void*>(static_cast<intptr_t>(a1)));
            return 0;
        case KOS_SYS_clock_now:
        {
            if (a0 == 0) return static_cast<uintptr_t>(-1);
            *reinterpret_cast<uint64_t*>(a0) = arch_clock_now();
            return 0;
        }
        default:
            // Unknown syscall from userspace is a caller error, not a kernel
            // invariant violation: fault the caller (-1), never panic the kernel.
            return static_cast<uintptr_t>(-1);
    }
}
