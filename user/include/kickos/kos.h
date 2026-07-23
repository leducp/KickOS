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
    // stdout. Ordinary output = libc stdio over a userspace console driver, Later.
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
    inline int irq_attach(int irq, int sem_id)
    {
        return kos_irq_attach(irq, sem_id);
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
    inline void clock_set_realtime(uint64_t unix_ns)
    {
        kos_clock_set_realtime(unix_ns);
    }
    // Borrow the kernel's diagnostic LED (see kos_kernel_diag_led_set): a shared
    // status pin, not an app-owned device -- provisional until caps land.
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

    // Owning counting semaphore: ctor creates, dtor closes its cap (last close frees
    // the object). Non-copyable, movable (a moved-from handle is emptied so the dtor
    // won't double-close).
    class Semaphore
    {
    public:
        explicit Semaphore(int initial = 0)
            : id_(kos_sem_create(initial))
        {
        }
        ~Semaphore()
        {
            if (id_ >= 0)
            {
                kos_sem_destroy(id_);
            }
        }

        Semaphore(Semaphore const&) = delete;
        Semaphore& operator=(Semaphore const&) = delete;

        Semaphore(Semaphore&& other) noexcept
            : id_(other.id_)
        {
            other.id_ = -1;
        }
        Semaphore& operator=(Semaphore&& other) noexcept
        {
            if (this != &other)
            {
                if (id_ >= 0)
                {
                    kos_sem_destroy(id_);
                }
                id_ = other.id_;
                other.id_ = -1;
            }
            return *this;
        }

        // Return the syscall status (0, or a negative -KOS_E*), surfaced now instead of
        // swallowed: on a moved-from / failed / closed handle these do NOT block or signal
        // -- they return -KOS_EBADF (so a caller can tell a real wait/post from a no-op),
        // where before the thread silently proceeded as if synchronized.
        int wait()
        {
            return kos_sem_wait(id_);
        }
        int post()
        {
            return kos_sem_post(id_);
        }
        int id() const
        {
            return id_;
        }

    private:
        int id_;
    };

    // Owning priority-inheritance mutex: ctor creates, dtor closes its cap (last
    // close frees the object). Non-copyable, movable (a moved-from handle is emptied
    // so the dtor won't double-close). lock/unlock return the raw syscall codes (see
    // kos_mutex_lock: 0, -KOS_EOWNERDEAD, -KOS_EBADF, -KOS_EDEADLK). ROBUST-MUTEX CAVEAT:
    // -KOS_EOWNERDEAD means the lock IS held (owner died) -- do NOT treat every rc < 0 as
    // "not acquired" or you strand the mutex; special-case rc == -KOS_EOWNERDEAD as held.
    class Mutex
    {
    public:
        Mutex()
            : id_(kos_mutex_create())
        {
        }
        ~Mutex()
        {
            // Closing a mutex you still hold is refused (R2: kos_handle_close -> -KOS_EBUSY),
            // so destroying a locked kos::Mutex leaks its cap -- the correct fail-safe
            // (unlock before scope exit). Unlock a mutex before letting it die.
            if (id_ >= 0)
            {
                kos_handle_close(id_);
            }
        }

        Mutex(Mutex const&) = delete;
        Mutex& operator=(Mutex const&) = delete;

        Mutex(Mutex&& other) noexcept
            : id_(other.id_)
        {
            other.id_ = -1;
        }
        Mutex& operator=(Mutex&& other) noexcept
        {
            if (this != &other)
            {
                if (id_ >= 0)
                {
                    kos_handle_close(id_);
                }
                id_ = other.id_;
                other.id_ = -1;
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
        int id() const
        {
            return id_;
        }

    private:
        int id_;
    };

    // IRQ-as-event handle (tier-1 userspace driver):
    //   auto irq = kos::Irq::request(line); irq.wait(); ...; irq.ack();
    // Non-owning handle wrapper; IRQ handles have no release path yet (out of
    // M0.4 scope, which freelisted only semaphores).
    class Irq
    {
    public:
        static Irq request(int line)
        {
            return Irq(kos_irq_register(line));
        }
        int wait()
        {
            return kos_irq_wait(h_);
        }
        int ack()
        {
            return kos_irq_ack(h_);
        }
        int handle() const
        {
            return h_;
        }

    private:
        explicit Irq(int h)
            : h_(h)
        {
        }
        int h_;
    };
}

// Define a caller-owned thread stack buffer for kos::thread::spawn's `stack`
// argument -- the OPTIONAL way to own a thread's stack (leave it off and the kernel
// demand-allocates a KICKOS_USER_STACK_SIZE default). Under MPU enforcement the
// stack is granted as ONE region, so the size must be a power of two and the buffer
// naturally aligned to it (a valid PMSA/NAPOT base). Without enforcement the stack is
// never a region: a 16-byte ABI alignment suffices and avoids wasting a large gap.
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
    // Start a thread (not a process: KickOS has one address space, isolation is
    // by MPU + privilege). Unprivileged by default. `mem`/`mem_size` grant the
    // thread a domain data region (threads sharing one region share a domain).
    // Spawning does NOT preempt the caller, even for a higher-priority thread:
    // the new thread runs once the caller next blocks or yields. Returns an opaque
    // handle (index+generation, not the telemetry thread id), or a negative -KOS_E* code.
    // `stack`/`stack_size` are optional: pass a caller-owned buffer to size a thread's
    // stack to its need, or leave them 0 to get the kernel default (KICKOS_USER_STACK_SIZE).
    // `mmio`/`mmio_size` grant a device register block (R|W|DEV); PRIVILEGED caller only.
    inline int spawn(void (*entry)(void*), void* arg, char const* name,
                     uint8_t prio, uint8_t policy = KOS_POLICY_FIFO,
                     uint32_t quantum_ns = 0, bool privileged = false,
                     void* mem = nullptr, uint32_t mem_size = 0,
                     void* stack = nullptr, uint32_t stack_size = 0,
                     void* mmio = nullptr, uint32_t mmio_size = 0,
                     kos_cap_grant const* caps = nullptr, uint8_t cap_count = 0)
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
        return kos_thread_spawn(&p);
    }

    // Delegate a fixed cap list to the child (B1: cap i -> child index i+1). The
    // common cross-thread-sem shape: a child that must wait/post sems the parent owns.
    // Covers the extra args the caps-using workers need (privileged rr/stress workers;
    // a shared mem grant for the domain test).
    inline int spawn_caps(void (*entry)(void*), void* arg, char const* name, uint8_t prio,
                          kos_cap_grant const* caps, uint8_t cap_count,
                          uint8_t policy = KOS_POLICY_FIFO, uint32_t quantum_ns = 0,
                          bool privileged = false, void* mem = nullptr, uint32_t mem_size = 0)
    {
        return spawn(entry, arg, name, prio, policy, quantum_ns, privileged, mem, mem_size,
                     nullptr, 0, nullptr, 0, caps, cap_count);
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
