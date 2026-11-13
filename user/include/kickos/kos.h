// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Ergonomic C++ layer over the C syscall API (dual API, invariant #6). Header
// only; every call still funnels through the same syscall trap.

#ifndef KICKOS_KOS_H
#define KICKOS_KOS_H

#include <kickos/sys.h>

#include <chrono>
#include <ratio>
#include <type_traits>

// Time crosses as std::chrono, defining no clock: a duration converts to kos_duration_t rounding
// up and saturating, a steady_clock point to kos_monotonic_t and a system_clock point to
// kos_realtime_t, and no other mix has an overload. A duration's or a point's max() is the same
// no limit as the overload that takes no time. A freestanding TU's <chrono> declares no clock, so
// it gets the duration forms alone.
namespace kos::detail
{
    // `d` in nanoseconds, rounded toward plus infinity and held inside the int64_t range; a NaN
    // count is 0.
    template <class Rep, class Period>
    constexpr int64_t ns_ceil(std::chrono::duration<Rep, Period> d)
    {
        using R = std::ratio_divide<Period, std::nano>;
        if constexpr (std::is_floating_point_v<Rep>)
        {
            double const c = static_cast<double>(d.count());
            if (not(c == c))
            {
                return 0;
            }
            // Bounded before scaling: an overflowing product is no constant expression.
            double const bound = 9223372036854775808.0 * static_cast<double>(R::den)
                                 / static_cast<double>(R::num);
            if (c >= bound)
            {
                return INT64_MAX;
            }
            if (c <= -bound)
            {
                return INT64_MIN;
            }
            double const v = c * static_cast<double>(R::num) / static_cast<double>(R::den);
            if (v >= 9223372036854775807.0)
            {
                return INT64_MAX;
            }
            if (v <= -9223372036854775808.0)
            {
                return INT64_MIN;
            }
            int64_t n = static_cast<int64_t>(v);
            if (static_cast<double>(n) < v)
            {
                n = n + 1;
            }
            return n;
        }
        else
        {
            static_assert(R::den <= INT64_MAX / R::num, "a period no int64_t product can scale");
            using U = std::common_type_t<std::make_unsigned_t<Rep>, uint64_t>;
            U const num = static_cast<U>(R::num);
            U const den = static_cast<U>(R::den);
            bool negative = false;
            U mag = static_cast<U>(d.count());
            if constexpr (std::is_signed_v<Rep>)
            {
                if (d.count() < 0)
                {
                    negative = true;
                    mag = U{0} - mag;
                }
            }
            U limit = static_cast<U>(INT64_MAX);
            if (negative)
            {
                limit = limit + 1;
            }
            U const q = mag / den;
            U ns = limit;
            if (q <= limit / num)
            {
                // (mag % den) * num < den * num, which the static_assert keeps inside int64_t.
                U const rest = (mag % den) * num;
                ns = q * num + rest / den;
                // A negative count rounds its magnitude down.
                if (not negative and rest % den != 0)
                {
                    ns = ns + 1;
                }
                if (ns > limit)
                {
                    ns = limit;
                }
            }
            if (negative)
            {
                if (ns == limit)
                {
                    return INT64_MIN;
                }
                return -static_cast<int64_t>(ns);
            }
            return static_cast<int64_t>(ns);
        }
    }
}

namespace kos
{
    template <class Rep, class Period>
    constexpr kos_duration_t to_kos(std::chrono::duration<Rep, Period> d)
    {
        if (d == d.max())
        {
            return KOS_FOREVER;
        }
        return kos_duration_t{detail::ns_ceil(d)};
    }
#if __STDC_HOSTED__
#if defined(__GLIBCXX__) and not defined(_GLIBCXX_USE_CLOCK_MONOTONIC)
    static_assert(false, "kickos/kos.h: this libstdc++ reads steady_clock from the wall clock, "
                         "not the KickOS monotonic clock; build with a toolchain of the KickOS "
                         "threads model (1.2.0 or later)");
#endif
    template <class Duration>
    constexpr kos_monotonic_t to_kos(std::chrono::time_point<std::chrono::steady_clock, Duration> t)
    {
        if (t == t.max())
        {
            return KOS_NEVER;
        }
        return kos_monotonic_t{detail::ns_ceil(t.time_since_epoch())};
    }
    template <class Duration>
    constexpr kos_realtime_t to_kos(std::chrono::time_point<std::chrono::system_clock, Duration> t)
    {
        if (t == t.max())
        {
            return kos_realtime_t{KOS_NEVER.ns};
        }
        return kos_realtime_t{detail::ns_ceil(t.time_since_epoch())};
    }
#endif
    constexpr std::chrono::nanoseconds to_chrono(kos_duration_t d)
    {
        return std::chrono::nanoseconds{d.ns};
    }
#if __STDC_HOSTED__
    constexpr std::chrono::steady_clock::time_point to_chrono(kos_monotonic_t t)
    {
        return std::chrono::steady_clock::time_point{
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::nanoseconds{t.ns})};
    }
    constexpr std::chrono::system_clock::time_point to_chrono(kos_realtime_t t)
    {
        return std::chrono::system_clock::time_point{
            std::chrono::duration_cast<std::chrono::system_clock::duration>(
                std::chrono::nanoseconds{t.ns})};
    }

    // The monotonic clock, read straight from kos_monotonic_now: the steady_clock epoch.
    inline std::chrono::steady_clock::time_point now()
    {
        return to_chrono(kos_monotonic_now());
    }
#endif
    // `d` from now, saturating.
    template <class Rep, class Period>
    kos_monotonic_t after(std::chrono::duration<Rep, Period> d)
    {
        return kos_after(kos_monotonic_now(), to_kos(d));
    }

    // Delegated capability i of a freshly spawned thread, where a default grant list seats it.
    constexpr kos_cap_t child_cap(int i)
    {
        return static_cast<kos_cap_t>(KOS_SPAWN_DELEGATED_CAP0 + i);
    }

    // kos_print: the whole line to stdout, as libc's write(1) sends it.
    inline void print(char const* s)
    {
        kos_print(s);
    }
    inline void yield()
    {
        kos_yield();
    }
    template <class Rep, class Period>
    void sleep_for(std::chrono::duration<Rep, Period> d)
    {
        kos_sleep_for(to_kos(d));
    }
#if __STDC_HOSTED__
    template <class Duration>
    void sleep_until(std::chrono::time_point<std::chrono::steady_clock, Duration> t)
    {
        kos_sleep_until(to_kos(t));
    }
#endif
    // Running core clock in Hz (0 where the backend has none); read-only, see kos_cpu_clock_hz.
    inline uint64_t cpu_clock_hz()
    {
        return kos_cpu_clock_hz();
    }
    // Branch clock feeding the register block at `base` in Hz (0 if unknown / sim);
    // read-only, see kos_periph_clock_hz.
    inline uint32_t periph_clock_hz(uintptr_t base)
    {
        return kos_periph_clock_hz(base);
    }
#if __STDC_HOSTED__
    // Set the realtime clock (see kos_realtime_set).
    template <class Duration>
    int realtime_set(std::chrono::time_point<std::chrono::system_clock, Duration> t)
    {
        return kos_realtime_set(to_kos(t));
    }
#endif
    // Borrow the kernel's diagnostic LED (see kos_kernel_diag_led_set): a shared
    // status pin, not an app-owned device; provisional until caps land.
    inline void kernel_diag_led(bool on)
    {
        kos_kernel_diag_led_set(on);
    }
    [[noreturn]] inline void exit(int code)
    {
        kos_exit(code);
    }

    // Counting semaphore over a kos_sem_t word it holds: no kernel object, so a static or a member
    // costs only the word, and nothing is freed. Not copyable or movable, since the word's address
    // is the semaphore. wait and post return the raw codes (see kos_sem_wait). A semaphore more
    // than one thread touches lives in memory every one of them can write, never on a thread's
    // stack: a region board gives each stack to its own thread.
    class Semaphore
    {
    public:
        // An initial count above KOS_SEM_COUNT_MAX does not compile in a constant initialisation
        // and traps at run time.
        constexpr explicit Semaphore(uint32_t initial = 0) : sem_{initial}
        {
            if (initial > static_cast<uint32_t>(KOS_SEM_COUNT_MAX))
            {
                __builtin_trap();
            }
        }

        Semaphore(Semaphore const&) = delete;
        Semaphore& operator=(Semaphore const&) = delete;

        int wait()
        {
            return kos_sem_wait(&sem_, KOS_NEVER);
        }
        // Takes only a unit already banked.
        int try_wait()
        {
            return kos_sem_wait(&sem_, KOS_POLL);
        }
        template <class Rep, class Period>
        int wait_for(std::chrono::duration<Rep, Period> d)
        {
            return kos_sem_wait(&sem_, after(d));
        }
#if __STDC_HOSTED__
        template <class Duration>
        int wait_until(std::chrono::time_point<std::chrono::steady_clock, Duration> t)
        {
            return kos_sem_wait(&sem_, to_kos(t));
        }
#endif
        int post()
        {
            return kos_sem_post(&sem_);
        }
        kos_sem_t* native()
        {
            return &sem_;
        }

    private:
        kos_sem_t sem_;
    };

    // Priority-inheritance mutex over a kos_mutex_t word it holds: no kernel object, so a
    // static or a member costs only the word, and nothing is freed. Not copyable or movable,
    // since the word's address is the mutex. lock, the timed locks and unlock return the raw codes
    // (see kos_mutex_lock). ROBUST-MUTEX CAVEAT: -KOS_EOWNERDEAD means the lock IS held (owner
    // died), so do NOT treat every rc < 0 as "not acquired" or you strand the mutex;
    // special-case rc == -KOS_EOWNERDEAD as held. Its storage must outlive every holder and
    // waiter: the kernel writes the word at the next hand-over.
    class Mutex
    {
    public:
        constexpr Mutex() = default;

        Mutex(Mutex const&) = delete;
        Mutex& operator=(Mutex const&) = delete;

        int lock()
        {
            return kos_mutex_lock(&word_, KOS_NEVER);
        }
        // True with the mutex held, a dead holder's stolen one included (-KOS_EOWNERDEAD); a
        // caller that must see that death uses try_lock_for with a zero duration.
        bool try_lock()
        {
            int const rc = kos_mutex_lock(&word_, KOS_POLL);
            return rc == 0 or rc == -KOS_EOWNERDEAD;
        }
        template <class Rep, class Period>
        int try_lock_for(std::chrono::duration<Rep, Period> d)
        {
            return kos_mutex_lock(&word_, after(d));
        }
#if __STDC_HOSTED__
        template <class Duration>
        int try_lock_until(std::chrono::time_point<std::chrono::steady_clock, Duration> t)
        {
            return kos_mutex_lock(&word_, to_kos(t));
        }
        template <class Duration>
        int try_lock_until(std::chrono::time_point<std::chrono::system_clock, Duration> t)
        {
            return kos_mutex_lock_realtime(&word_, to_kos(t));
        }
#endif
        int unlock()
        {
            return kos_mutex_unlock(&word_);
        }

    private:
        kos_mutex_t word_ = KOS_MUTEX_INIT;
    };

    // The capability wrappers below carry both the handle and the code the create returned.
    // `valid()` is the ONLY correct success test: there is no negative handle to compare
    // against, and a live handle may look negative if cast to int.
    //
    // IRQ line capability (tier-1 userspace driver). Two ways in:
    //   root:   auto irq = kos::Irq::claim(line);        // needs KOS_AUTH_IRQ
    //   driver: auto irq = kos::Irq::adopt(cap_index);   // a cap delegated at spawn
    // then irq.watch(n); n.bind(); kos::notify_wait(mask, ...); ...; irq.ack();
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
        // Attach this line to a notification, as one signaller among others. The ISR raises
        // that capability's badge bit there.
        int watch(kos_cap_t notify_cap)
        {
            return kos_watch(KOS_WATCH_IRQ, KOS_IRQ_FIRED, h_, notify_cap, 0, nullptr);
        }
        int ack()
        {
            return kos_irq_ack(h_);
        }
        int discard()
        {
            return kos_irq_discard(h_);
        }
        // Raise the line as its device firing would; needs KOS_CAP_SIGNAL (see kos_irq_raise).
        int raise()
        {
            return kos_irq_raise(h_);
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

    // A notification object. Two ways in, as with Irq:
    //   auto n = kos::Notification::create();
    //   auto n = kos::Notification::adopt(cap_index);   // a cap delegated at spawn
    // OWNING and move-only: the destructor closes the capability, which drops one reference.
    // A BINDING is not a capability and is not closed here; it ends at kos_notify_unbind or
    // at the thread's death.
    class Notification
    {
    public:
        static Notification create()
        {
            kos_cap_t h = KOS_CAP_NONE;
            int const rc = kos_notify_create(&h);
            return Notification(h, rc);
        }
        static Notification adopt(kos_cap_t notify_cap)
        {
            return Notification(notify_cap, 0);
        }
        Notification(Notification&& o)
            : h_(o.h_), err_(o.err_)
        {
            o.h_ = KOS_CAP_NONE;
        }
        Notification& operator=(Notification&& o)
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
        Notification(Notification const&) = delete;
        Notification& operator=(Notification const&) = delete;
        ~Notification()
        {
            close();
        }
        // Mint a badged second name for the same object. This capability must be unbadged.
        static Notification badge(kos_cap_t source_cap, uint32_t bit)
        {
            kos_cap_t h = KOS_CAP_NONE;
            int const rc = kos_notify_badge(source_cap, bit, &h);
            return Notification(h, rc);
        }
        int signal()
        {
            return kos_notify(h_);
        }
        int bind()
        {
            return kos_notify_bind(h_);
        }
        int unbind()
        {
            return kos_notify_unbind(h_);
        }
        kos_cap_t handle() const
        {
            return h_;
        }
        bool valid() const
        {
            return h_ != KOS_CAP_NONE;
        }
        // The code kos_notify_create returned; 0 for an adopted cap.
        int error() const
        {
            return err_;
        }

    private:
        Notification(kos_cap_t h, int err)
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

    // Wait on the notification the caller is bound to, which names no object (see
    // kos_notify_wait).
    inline int notify_wait(uint32_t mask, uint32_t* out_bits = nullptr)
    {
        return kos_notify_wait(mask, KOS_NEVER, out_bits);
    }
    template <class Rep, class Period>
    int notify_wait_for(uint32_t mask, std::chrono::duration<Rep, Period> d,
                        uint32_t* out_bits = nullptr)
    {
        return kos_notify_wait(mask, after(d), out_bits);
    }
#if __STDC_HOSTED__
    template <class Duration>
    int notify_wait_until(uint32_t mask,
                          std::chrono::time_point<std::chrono::steady_clock, Duration> t,
                          uint32_t* out_bits = nullptr)
    {
        return kos_notify_wait(mask, to_kos(t), out_bits);
    }
#endif
}

namespace kos::thread
{
    // What create hands back: the handle and the code kos_thread_create returned. `valid()` is
    // the ONLY correct success test: there is no negative handle to compare against, and a
    // live handle looks negative cast to int.
    //
    // NOT owning, unlike Semaphore: a thread handle names nothing to close, so there is no
    // destructor and copying is free.
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
        // Raise the cancel and return (see kos_thread_cancel); join() waits for the end.
        // Returns -KOS_EBADF on a failed create: KOS_THREAD_NONE names nothing to cancel.
        int cancel(enum kos_cancel kind) const
        {
            return kos_thread_cancel(id_, kind);
        }
        // Wait for the thread to be gone (see kos_thread_join): 0 also for a thread that
        // had already exited, and -KOS_EBADF on a failed create.
        int join() const
        {
            return kos_thread_join(id_, KOS_NEVER);
        }
        template <class Rep, class Period>
        int join_for(std::chrono::duration<Rep, Period> d) const
        {
            return kos_thread_join(id_, after(d));
        }
#if __STDC_HOSTED__
        template <class Duration>
        int join_until(std::chrono::time_point<std::chrono::steady_clock, Duration> t) const
        {
            return kos_thread_join(id_, to_kos(t));
        }
#endif

    private:
        kos_thread_t id_ = KOS_THREAD_NONE;
        int err_ = -KOS_EBADF;
    };

    // Create an unprivileged thread. One that outranks the caller runs before this returns; one
    // that does not runs once the caller blocks or yields, or at once on another core running
    // below it.
    // stack/stack_size optionally supplies a caller-owned stack, otherwise
    // KICKOS_USER_STACK_SIZE is used.
    // windows/window_count is this thread's window list (kos_window); a device window
    // requires AUTH_MEMORY.
    // The thread joins the caller's task: a fault terminates the whole task.
    inline Handle create(void (*entry)(void*), void* arg, char const* name,
                        uint8_t prio, uint8_t policy = KOS_POLICY_FIFO,
                        std::chrono::nanoseconds quantum = std::chrono::nanoseconds{0},
                        void* stack = nullptr, uint32_t stack_size = 0,
                        kos_window const* windows = nullptr, uint16_t window_count = 0,
                        kos_cap_grant const* caps = nullptr, uint8_t cap_count = 0,
                        uint32_t authority = 0, uint16_t const* cap_dest = nullptr,
                        uint32_t core_mask = 0)
    {
        kos_thread_params p{};
        p.entry = entry;
        p.arg = arg;
        p.name = name;
        p.prio = prio;
        p.policy = policy;
        p.quantum = to_kos(quantum);
        p.windows = windows;
        p.window_count = window_count;
        p.stack_base = stack;
        p.stack_size = stack_size;
        p.caps = caps;
        p.cap_count = cap_count;
        p.authority = authority;
        p.cap_dest = cap_dest;
        p.core_mask = core_mask;
        kos_thread_t h = KOS_THREAD_NONE;
        int const rc = kos_thread_create(&p, &h);
        return Handle(h, rc);
    }

    // Placement, spelled two ways over one kernel call, each answering the mask the thread now
    // runs on (see kos_thread_affinity). A mask of zero asks for the task's DEFAULT set, so
    // unpin restores what a thread naming no core is given at spawn without userspace ever
    // having to learn that set. All ones would be a wider request than the default: it names
    // the isolated cores too.
    inline int pin(kos_thread_t thread, uint32_t core)
    {
        if (core >= 31u)
        {
            return -KOS_EINVAL; // bit 31 is the query, and 1u << core is undefined past it
        }
        return kos_thread_affinity(thread, 1u << core);
    }
    inline int unpin(kos_thread_t thread)
    {
        return kos_thread_affinity(thread, 0);
    }
    // The core the caller ran on when the kernel read it; 0 on a one-core kernel.
    inline int core()
    {
        return kos_core_current();
    }
    // The mask `thread` may run on, or a negative -KOS_E* (see kos_thread_affinity).
    inline int affinity(kos_thread_t thread)
    {
        return kos_thread_affinity(thread, KOS_AFFINITY_QUERY);
    }

    // Delegate a fixed cap list to the child (B1 default: cap i -> child index KOS_SPAWN_DELEGATED_CAP0 + i, and a
    // grant may name its own index instead).
    inline Handle create_caps(void (*entry)(void*), void* arg, char const* name, uint8_t prio,
                             kos_cap_grant const* caps, uint8_t cap_count,
                             uint8_t policy = KOS_POLICY_FIFO,
                             std::chrono::nanoseconds quantum = std::chrono::nanoseconds{0},
                             uint32_t authority = 0, uint16_t const* cap_dest = nullptr,
                             void* stack = nullptr,
                             uint32_t stack_size = 0, uint32_t core_mask = 0)
    {
        return create(entry, arg, name, prio, policy, quantum, stack,
                     stack_size, nullptr, 0, caps, cap_count, authority, cap_dest, core_mask);
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

#if defined(KICKOS_HAVE_ASPACE) && KICKOS_HAVE_ASPACE
namespace kos
{
    // An owned capability handle and the code its mint returned: move-only, closed by the
    // destructor. A moved-from object names nothing and answers -KOS_EBADF from error().
    class OwnedCap
    {
    public:
        OwnedCap(OwnedCap&& o)
            : h_(o.h_), err_(o.err_)
        {
            o.h_ = KOS_CAP_NONE;
            o.err_ = -KOS_EBADF;
        }
        OwnedCap& operator=(OwnedCap&& o)
        {
            if (this != &o)
            {
                close();
                h_ = o.h_;
                err_ = o.err_;
                o.h_ = KOS_CAP_NONE;
                o.err_ = -KOS_EBADF;
            }
            return *this;
        }
        OwnedCap(OwnedCap const&) = delete;
        OwnedCap& operator=(OwnedCap const&) = delete;
        ~OwnedCap()
        {
            close();
        }
        kos_cap_t handle() const
        {
            return h_;
        }
        bool valid() const
        {
            return h_ != KOS_CAP_NONE;
        }
        // The code the mint returned; 0 for an adopted cap.
        int error() const
        {
            return err_;
        }

    protected:
        OwnedCap(kos_cap_t h, int err)
            : h_(h), err_(err)
        {
        }

    private:
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

    // An address-space capability. Two ways in, as with Irq:
    //   auto space = kos::Space::self();          // the calling task's own
    //   auto space = kos::Space::adopt(cap_index); // a cap delegated at spawn
    // It holds no reference on the space.
    class Space : public OwnedCap
    {
    public:
        static Space self()
        {
            kos_cap_t h = KOS_CAP_NONE;
            int const rc = kos_aspace_self(&h);
            return Space(h, rc);
        }
        static Space adopt(kos_cap_t space_cap)
        {
            return Space(space_cap, 0);
        }

    private:
        Space(kos_cap_t h, int err)
            : OwnedCap(h, err)
        {
        }
    };

    // A frame-run capability (see kos_frame_create):
    //   auto run = kos::Frame::create(bytes);
    //   uintptr_t va = 0; run.map(space.handle(), &va);   // 0: the kernel chooses
    // The frames go back to the pool at the run's last holder, a mapping being one.
    class Frame : public OwnedCap
    {
    public:
        static Frame create(size_t bytes)
        {
            kos_cap_t h = KOS_CAP_NONE;
            int const rc = kos_frame_create(bytes, &h);
            return Frame(h, rc);
        }
        static Frame adopt(kos_cap_t frame_cap)
        {
            return Frame(frame_cap, 0);
        }
        int map(kos_cap_t space, uintptr_t* va, uint32_t flags = 0)
        {
            return kos_frame_map(handle(), space, va, flags);
        }
        int unmap(kos_cap_t space, uintptr_t va)
        {
            return kos_frame_unmap(handle(), space, va);
        }

    private:
        Frame(kos_cap_t h, int err)
            : OwnedCap(h, err)
        {
        }
    };
}
#endif

#endif
