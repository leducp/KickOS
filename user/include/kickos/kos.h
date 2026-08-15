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
    // Debug console output (see kos_print): the developer escape hatch, not
    // stdout. Ordinary output = libc stdio over a userspace console driver.
    inline void print(char const* s)
    {
        kos_print(s);
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
    inline int irq_attach(int irq, kos_cap_t sem_cap)
    {
        return kos_irq_attach(irq, sem_cap);
    }
    inline uint64_t clock_now()
    {
        return kos_clock_now();
    }
    // Running core clock in Hz (0 on the host sim); read-only, see kos_cpu_clock_hz.
    inline uint32_t cpu_clock_hz()
    {
        return kos_cpu_clock_hz();
    }
    // Branch clock feeding the register block at `base` in Hz (0 if unknown / sim);
    // read-only, see kos_periph_clock_hz.
    inline uint32_t periph_clock_hz(uintptr_t base)
    {
        return kos_periph_clock_hz(base);
    }
    inline void clock_set_realtime(uint64_t unix_ns)
    {
        kos_clock_set_realtime(unix_ns);
    }
    // Borrow the kernel's diagnostic LED (see kos_kernel_diag_led_set): a shared
    // status pin, not an app-owned device; provisional until caps land.
    inline void kernel_diag_led(bool on)
    {
        kos_kernel_diag_led_set(on);
    }
    inline void kernel_diag_led_toggle()
    {
        kos_kernel_diag_led_toggle();
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

    // These wrappers carry both the handle and the code the create returned. `valid()` is
    // the ONLY correct success test: there is no negative handle to compare against, and a
    // live handle may look negative if cast to int.
    //
    // Owning counting semaphore: ctor creates, dtor closes its cap (last close frees
    // the object). Non-copyable, movable (a moved-from handle is emptied so the dtor
    // won't double-close).
    class Semaphore
    {
    public:
        explicit Semaphore(int initial = 0)
        {
            err_ = kos_sem_create(initial, &id_);
        }
        ~Semaphore()
        {
            if (id_ != KOS_CAP_NONE)
            {
                kos_sem_destroy(id_);
            }
        }

        Semaphore(Semaphore const&) = delete;
        Semaphore& operator=(Semaphore const&) = delete;

        Semaphore(Semaphore&& other) noexcept
            : id_(other.id_), err_(other.err_)
        {
            other.id_ = KOS_CAP_NONE;
        }
        Semaphore& operator=(Semaphore&& other) noexcept
        {
            if (this != &other)
            {
                if (id_ != KOS_CAP_NONE)
                {
                    kos_sem_destroy(id_);
                }
                id_ = other.id_;
                err_ = other.err_;
                other.id_ = KOS_CAP_NONE;
            }
            return *this;
        }

        // Return the syscall status (0, or a negative -KOS_E*). On a moved-from / failed /
        // closed handle these do NOT block or signal: they return -KOS_EBADF, so a caller
        // can tell a real wait/post from a no-op instead of proceeding as if synchronized.
        int wait()
        {
            return kos_sem_wait(id_);
        }
        int post()
        {
            return kos_sem_post(id_);
        }
        kos_cap_t id() const
        {
            return id_;
        }
        bool valid() const
        {
            return id_ != KOS_CAP_NONE;
        }
        int error() const
        {
            return err_;
        }

    private:
        kos_cap_t id_ = KOS_CAP_NONE;
        int err_ = -KOS_EBADF;
    };

    // Owning priority-inheritance mutex: ctor creates, dtor closes its cap (last
    // close frees the object). Non-copyable, movable (a moved-from handle is emptied
    // so the dtor won't double-close). lock/unlock return the raw syscall codes (see
    // kos_mutex_lock: 0, -KOS_EOWNERDEAD, -KOS_EBADF, -KOS_EDEADLK). ROBUST-MUTEX CAVEAT:
    // -KOS_EOWNERDEAD means the lock IS held (owner died), so do NOT treat every rc < 0 as
    // "not acquired" or you strand the mutex; special-case rc == -KOS_EOWNERDEAD as held.
    class Mutex
    {
    public:
        Mutex()
        {
            err_ = kos_mutex_create(&id_);
        }
        ~Mutex()
        {
            // Closing a mutex you still hold is refused (R2: kos_handle_close -> -KOS_EBUSY),
            // so destroying a locked kos::Mutex leaks its cap. Unlock it before letting it
            // die.
            if (id_ != KOS_CAP_NONE)
            {
                kos_handle_close(id_);
            }
        }

        Mutex(Mutex const&) = delete;
        Mutex& operator=(Mutex const&) = delete;

        Mutex(Mutex&& other) noexcept
            : id_(other.id_), err_(other.err_)
        {
            other.id_ = KOS_CAP_NONE;
        }
        Mutex& operator=(Mutex&& other) noexcept
        {
            if (this != &other)
            {
                if (id_ != KOS_CAP_NONE)
                {
                    kos_handle_close(id_);
                }
                id_ = other.id_;
                err_ = other.err_;
                other.id_ = KOS_CAP_NONE;
            }
            return *this;
        }

        int lock()
        {
            return kos_mutex_lock(id_);
        }
        int unlock()
        {
            return kos_mutex_unlock(id_);
        }
        kos_cap_t id() const
        {
            return id_;
        }
        bool valid() const
        {
            return id_ != KOS_CAP_NONE;
        }
        int error() const
        {
            return err_;
        }

    private:
        kos_cap_t id_ = KOS_CAP_NONE;
        int err_ = -KOS_EBADF;
    };

    // IRQ line capability (tier-1 userspace driver). Two ways in:
    //   root:   auto irq = kos::Irq::claim(line);        // needs KOS_AUTH_IRQ
    //   driver: auto irq = kos::Irq::adopt(cap_index);   // a cap delegated at spawn
    // then irq.wait(); ...; irq.ack();
    // OWNING and move-only: the destructor closes the cap, which drops the binding's
    // last reference and hands the line back if this was the only holder.
    class Irq
    {
    public:
        static Irq claim(int line, unsigned int flags = KOS_IRQ_EDGE)
        {
            kos_cap_t h = KOS_CAP_NONE;
            int const rc = kos_irq_claim(line, flags, &h);
            return Irq(h, rc);
        }
        // Wrap a cap the spawning parent already delegated into this thread's table.
        static Irq adopt(kos_cap_t irq_cap)
        {
            return Irq(irq_cap, 0);
        }
        Irq(Irq&& o)
            : h_(o.h_), err_(o.err_)
        {
            o.h_ = KOS_CAP_NONE;
        }
        Irq& operator=(Irq&& o)
        {
            if (this != &o)
            {
                close();
                h_ = o.h_;
                err_ = o.err_;
                o.h_ = KOS_CAP_NONE;
            }
            return *this;
        }
        Irq(Irq const&) = delete;
        Irq& operator=(Irq const&) = delete;
        ~Irq()
        {
            close();
        }
        int wait()
        {
            return kos_irq_wait(h_);
        }
        int ack()
        {
            return kos_irq_ack(h_);
        }
        int notify()
        {
            return kos_irq_notify(h_);
        }
        int discard()
        {
            return kos_irq_discard(h_);
        }
        kos_cap_t handle() const
        {
            return h_;
        }
        bool valid() const
        {
            return h_ != KOS_CAP_NONE;
        }
        // The code kos_irq_claim returned; 0 for an adopted cap.
        int error() const
        {
            return err_;
        }

    private:
        Irq(kos_cap_t h, int err)
            : h_(h), err_(err)
        {
        }
        void close()
        {
            if (h_ != KOS_CAP_NONE)
            {
                kos_handle_close(h_);
                h_ = KOS_CAP_NONE;
            }
        }
        kos_cap_t h_;
        int err_;
    };
}

// Define a caller-owned thread stack buffer for kos::thread::spawn's `stack` argument.
// Leave it off and the kernel demand-allocates a KICKOS_USER_STACK_SIZE default.
// Under MPU enforcement the stack is granted as ONE region. Power-of-two size and
// natural alignment is a conservative compile-time superset: PMSAv7/NAPOT require it,
// while the base+limit backends (PMSAv8, SYSMPU, RX) would accept any
// arch_mpu_min_region() multiple. The granule is a runtime seam value, so that looser
// rule is not expressible here and those parts pay a padding gap.
// Without enforcement the stack is never a region: a 16-byte ABI alignment suffices.
#if KICKOS_HAVE_MPU
#define KOS_STACK_DEFINE(name, size)                                              \
    static_assert(((size) & ((size) - 1)) == 0,                                   \
                  "KOS_STACK_DEFINE: size must be a power of two under MPU");      \
    alignas(size) unsigned char name[size]
#else
#define KOS_STACK_DEFINE(name, size) alignas(16) unsigned char name[size]
#endif

namespace kos::thread
{
    // What spawn hands back: the handle and the code kos_thread_spawn returned. `valid()` is
    // the ONLY correct success test: there is no negative handle to compare against, and a
    // live handle looks negative cast to int.
    //
    // NOT owning, unlike Semaphore: a thread handle names nothing to close, so there is no
    // destructor and copying is free. kill() is COOPERATIVE (see kos_thread_kill).
    class Handle
    {
    public:
        Handle() = default;
        Handle(kos_thread_t id, int err)
            : id_(id), err_(err)
        {
        }

        kos_thread_t id() const
        {
            return id_;
        }
        bool valid() const
        {
            return id_ != KOS_THREAD_NONE;
        }
        int error() const
        {
            return err_;
        }
        // Returns -KOS_EBADF on a failed spawn: KOS_THREAD_NONE names nothing to cancel.
        int kill() const
        {
            return kos_thread_kill(id_);
        }
        // FORCIBLE, where kill() is cooperative, and it WAITS (see kos_thread_slay): 0 means
        // gone, -KOS_ETIMEDOUT means condemned and irrevocably so with the capability sweep
        // still outstanding. Unbounded by default, because a caller that wanted "accepted"
        // rather than "gone" wanted kill().
        int slay(uint32_t timeout_us = KOS_TIMEOUT_NONE) const
        {
            return kos_thread_slay(id_, timeout_us);
        }
        // Wait for the thread to be gone (see kos_thread_join): 0 also for a thread that
        // had already exited, and -KOS_EBADF on a failed spawn. Unbounded by default, which
        // is what a caller wants when it has just kill()ed a cooperative target.
        int join(uint32_t timeout_us = KOS_TIMEOUT_NONE) const
        {
            return kos_thread_join(id_, timeout_us);
        }

    private:
        kos_thread_t id_ = KOS_THREAD_NONE;
        int err_ = -KOS_EBADF;
    };

    // Start a thread (not a process: KickOS has one address space, isolation is
    // by MPU + privilege). Unprivileged by default. `mem`/`mem_size` grant the
    // thread a domain data region (threads sharing one region share a domain).
    // Spawning does NOT preempt the caller, even for a higher-priority thread:
    // the new thread runs once the caller next blocks or yields.
    // `stack`/`stack_size` are optional: pass a caller-owned buffer to size a thread's
    // stack to its need, or leave them 0 to get the kernel default (KICKOS_USER_STACK_SIZE).
    // `mmio`/`mmio_size` grant a device register block (R|W|DEV); the caller needs
    // AUTH_MEMORY (privilege implies every authority). It is the new thread's ALONE, whatever
    // group it joins.
    // `task` names a group from kos_task_create for the thread to JOIN, coupling its fate and
    // its shared memory to its peers'; the default leaves it in a group of its own, which is
    // what every spawn meant before tasks existed.
    inline Handle spawn(void (*entry)(void*), void* arg, char const* name,
                        uint8_t prio, uint8_t policy = KOS_POLICY_FIFO,
                        uint32_t quantum_ns = 0, bool privileged = false,
                        void* mem = nullptr, uint32_t mem_size = 0,
                        void* stack = nullptr, uint32_t stack_size = 0,
                        void* mmio = nullptr, uint32_t mmio_size = 0,
                        kos_cap_grant const* caps = nullptr, uint8_t cap_count = 0,
                        uint8_t authority = 0, uint16_t const* cap_dest = nullptr,
                        kos_task_t task = KOS_TASK_NONE)
    {
        kos_thread_params p{};
        p.entry = entry;
        p.arg = arg;
        p.name = name;
        p.prio = prio;
        p.policy = policy;
        p.quantum_ns = quantum_ns;
        p.privileged = static_cast<uint8_t>(privileged);
        p.mem_base = mem;
        p.mem_size = mem_size;
        p.mmio_base = mmio;
        p.mmio_size = mmio_size;
        p.stack_base = stack;
        p.stack_size = stack_size;
        p.caps = caps;
        p.cap_count = cap_count;
        p.authority = authority;
        p.cap_dest = cap_dest;
        p.task = task;
        kos_thread_t h = KOS_THREAD_NONE;
        int const rc = kos_thread_spawn(&p, &h);
        return Handle(h, rc);
    }

    // Delegate a fixed cap list to the child (B1 default: cap i -> child index i+1, and a
    // grant may name its own index instead).
    inline Handle spawn_caps(void (*entry)(void*), void* arg, char const* name, uint8_t prio,
                             kos_cap_grant const* caps, uint8_t cap_count,
                             uint8_t policy = KOS_POLICY_FIFO, uint32_t quantum_ns = 0,
                             bool privileged = false, void* mem = nullptr, uint32_t mem_size = 0,
                             uint8_t authority = 0, uint16_t const* cap_dest = nullptr,
                             kos_task_t task = KOS_TASK_NONE)
    {
        return spawn(entry, arg, name, prio, policy, quantum_ns, privileged, mem, mem_size,
                     nullptr, 0, nullptr, 0, caps, cap_count, authority, cap_dest, task);
    }
}

namespace kos
{
    // Allocate a domain data region from the user-RAM pool (see kos_ram_alloc).
    inline void* ram_alloc(size_t size)
    {
        return kos_ram_alloc(size);
    }
}

#endif // KICKOS_KOS_H
