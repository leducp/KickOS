// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Ergonomic C++ layer over the C syscall API (dual API, invariant #6). Header
// only; every call still funnels through the same syscall trap.

#ifndef KICKOS_KOS_H
#define KICKOS_KOS_H

#include <kickos/sys.h>

namespace kos
{

    inline long write(int fd, void const* buf, size_t len)
    {
        return kos_write(fd, buf, len);
    }
    inline long puts(char const* s)
    {
        return kos_puts(s);
    }
    inline void yield()
    {
        kos_yield();
    }
    inline void sleep_ns(uint64_t ns)
    {
        kos_sleep_ns(ns);
    }
    inline void irq_inject(int irq)
    {
        kos_irq_inject(irq);
    }
    inline void irq_attach(int irq, int sem_id)
    {
        kos_irq_attach(irq, sem_id);
    }
    inline uint64_t clock_now()
    {
        return kos_clock_now();
    }
#if defined(KICKOS_ENABLE_SELFTEST)
    inline void* guard_addr()
    {
        return kos_guard_addr();
    }
#endif
    [[noreturn]] inline void exit(int code)
    {
        kos_exit(code);
    }

    // Counting semaphore handle.
    class Semaphore
    {
    public:
        explicit Semaphore(int initial = 0)
            : id_(kos_sem_create(initial))
        {
        }
        void wait()
        {
            kos_sem_wait(id_);
        }
        void post()
        {
            kos_sem_post(id_);
        }
        int id() const
        {
            return id_;
        }

    private:
        int id_;
    };

}

namespace kos::thread
{

    // Start a thread (not a process: KickOS has one address space, isolation is
    // by MPU + privilege). Unprivileged by default. Returns a thread id, or -1.
    inline int spawn(void (*entry)(void*), void* arg, char const* name,
                     uint8_t prio, uint8_t policy = KOS_POLICY_FIFO,
                     uint32_t quantum_ns = 0, bool privileged = false)
    {
        kos_thread_params p{};
        p.entry = entry;
        p.arg = arg;
        p.name = name;
        p.prio = prio;
        p.policy = policy;
        p.quantum_ns = quantum_ns;
        p.privileged = static_cast<uint8_t>(privileged);
        return kos_thread_spawn(&p);
    }

}

#endif // KICKOS_KOS_H
