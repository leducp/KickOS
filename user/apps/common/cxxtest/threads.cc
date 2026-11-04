// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// cxxtest's threaded checks: what the C++ runtime keeps once for every thread of a task, a
// function-local static, a once_flag, a shared_ptr's counts and the unwinder's frame registry,
// reached from two threads at once, what it keeps per thread, the caught exceptions and the
// uncaught count, and std::thread, std::mutex and std::condition_variable
// over the KickOS threads model. Everything two threads share is static: a thread's stack is
// its own on a region board.

#include "threads.h"

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/user_cas.h>

#include <bits/gthr.h>
#include <malloc.h>
#include <stdlib.h>

#include <atomic>
#include <cxxabi.h>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace
{
    using namespace std::chrono_literals;

    void bump(uint32_t* count)
    {
        uint32_t v = __atomic_load_n(count, __ATOMIC_RELAXED);
        while (true)
        {
            uint32_t const seen = kickos_user_cas(count, v, v + 1u);
            if (seen == v)
            {
                return;
            }
            v = seen;
        }
    }

    uint32_t read(uint32_t const* count)
    {
        return __atomic_load_n(count, __ATOMIC_ACQUIRE);
    }

    // Both threads throw their first exception together, which sorts the frame registry.
    uint32_t g_throw_arrived = 0;
    int g_throw_caught[2];

    void first_throw(int i)
    {
        bump(&g_throw_arrived);
        while (read(&g_throw_arrived) < 2)
        {
            std::this_thread::yield();
        }
        try
        {
            throw std::runtime_error("first");
        }
        catch (std::runtime_error const& e)
        {
            g_throw_caught[i] = e.what()[0] == 'f';
        }
    }

    // A static whose construction sleeps, so the second thread arrives while it is built.
    uint32_t g_slow_built = 0;
    int g_slow_seen[2];

    struct Slow
    {
        Slow()
        {
            bump(&g_slow_built);
            std::this_thread::sleep_for(5ms);
            value = 42;
        }
        int value = 0;
    };

    void slow_user(int i)
    {
        static Slow s;
        g_slow_seen[i] = s.value;
    }

    std::once_flag g_once;
    uint32_t g_once_runs = 0;
    int g_once_value = 0;
    int g_once_seen[3];

    void once_body()
    {
        bump(&g_once_runs);
        std::this_thread::sleep_for(5ms);
        g_once_value = 7;
    }

    void once_user(int i)
    {
        std::call_once(g_once, once_body);
        g_once_seen[i] = g_once_value;
    }

    std::once_flag g_throwing_once;
    int g_throwing_tries = 0;

    void throws_first()
    {
        g_throwing_tries++;
        if (g_throwing_tries == 1)
        {
            throw std::runtime_error("once");
        }
    }

    // Copied and dropped by two threads while a third, above them, takes or drops one copy at
    // each wake, so on one core it lands inside their count updates. The task holds four
    // threads: main, the worker and two more.
    constexpr int CHURN = 20000;
    std::shared_ptr<int> g_shared;
    std::atomic<bool> g_churn_done{false};

    void churn()
    {
        for (int i = 0; i < CHURN; i++)
        {
            std::shared_ptr<int> copy = g_shared;
            (void)copy;
        }
    }

    void interrupter(void*)
    {
        std::shared_ptr<int> held;
        while (not g_churn_done.load())
        {
            if (held)
            {
                held.reset();
            }
            else
            {
                held = g_shared;
            }
            kos_sleep_ns(100000u);
        }
        held.reset();
    }

    constexpr int ROUNDS = 200;
    std::mutex g_pp_mutex;
    std::condition_variable g_pp_cv;
    int g_turn = 0;
    int g_pp_count = 0;

    void player(int me)
    {
        for (int i = 0; i < ROUNDS; i++)
        {
            std::unique_lock<std::mutex> lock(g_pp_mutex);
            g_pp_cv.wait(lock, [me] { return g_turn == me; });
            g_pp_count++;
            g_turn = 1 - me;
            g_pp_cv.notify_one();
        }
    }

#if KICKOS_KERNEL_CORES == 1
    // At its creator's priority it runs at the creator's yield and yields back.
    std::atomic<int> g_step{0};

    void stepper()
    {
        g_step.store(1);
        kos_yield();
        g_step.store(2);
    }
#endif

    std::timed_mutex g_timed;
    bool g_timed_got = true;
    std::chrono::steady_clock::duration g_timed_waited{};

    void timed_taker()
    {
        auto const t0 = std::chrono::steady_clock::now();
        g_timed_got = g_timed.try_lock_for(10ms);
        g_timed_waited = std::chrono::steady_clock::now() - t0;
    }

    std::recursive_timed_mutex g_rtimed;
    bool g_rtimed_held_got = true;
    bool g_rtimed_free_got = false;
    std::chrono::steady_clock::duration g_rtimed_waited{};

    void rtimed_held_taker()
    {
        auto const t0 = std::chrono::steady_clock::now();
        g_rtimed_held_got = g_rtimed.try_lock_for(10ms);
        g_rtimed_waited = std::chrono::steady_clock::now() - t0;
    }

    void rtimed_free_taker()
    {
        g_rtimed_free_got = g_rtimed.try_lock_for(10ms);
        if (g_rtimed_free_got)
        {
            g_rtimed.unlock();
        }
    }

    // A recursive mutex held two deep across a condition wait: the wait frees it for another
    // thread, and the waiter gets both levels back.
    __gthread_recursive_mutex_t g_rwait_mutex = __GTHREAD_RECURSIVE_MUTEX_INIT;
    __gthread_cond_t g_rwait_cond = __GTHREAD_COND_INIT;
    bool g_rwait_flag = false;
    bool g_rwait_other_got = true;

    void rwait_signaller()
    {
        (void)__gthread_recursive_mutex_lock(&g_rwait_mutex);
        g_rwait_flag = true;
        (void)__gthread_cond_signal(&g_rwait_cond);
        (void)__gthread_recursive_mutex_unlock(&g_rwait_mutex);
    }

    void rwait_trier()
    {
        g_rwait_other_got = __gthread_recursive_mutex_trylock(&g_rwait_mutex) == 0;
        if (g_rwait_other_got)
        {
            (void)__gthread_recursive_mutex_unlock(&g_rwait_mutex);
        }
    }

    bool recursive_wait_keeps_depth()
    {
        g_rwait_flag = false;
        (void)__gthread_recursive_mutex_lock(&g_rwait_mutex);
        (void)__gthread_recursive_mutex_lock(&g_rwait_mutex);
        std::thread s(rwait_signaller);
        int rc = 0;
        while (rc == 0 and not g_rwait_flag)
        {
            rc = __gthread_cond_wait_recursive(&g_rwait_cond, &g_rwait_mutex);
        }
        (void)__gthread_recursive_mutex_unlock(&g_rwait_mutex);
        std::thread held(rwait_trier);
        held.join();
        bool const one_left = not g_rwait_other_got;
        (void)__gthread_recursive_mutex_unlock(&g_rwait_mutex);
        std::thread freed(rwait_trier);
        freed.join();
        s.join();
        return rc == 0 and g_rwait_flag and one_left and g_rwait_other_got;
    }

    void inherit_entry(void*)
    {
    }

    // Every key the task has left, then one freed and created again while a thread still holds
    // a value set under the deleted key.
    __gthread_key_t g_fill[KICKOS_THREAD_KEYS];
    __gthread_key_t g_old_key;
    __gthread_key_t g_new_key;
    int g_stale_value = 1;
    int g_fresh_value = 2;
    void* g_seen_under_new = &g_stale_value;
    int g_old_dtor_calls = 0;
    int g_new_dtor_calls = 0;
    void* g_new_dtor_arg = nullptr;
    kos::Semaphore g_key_set;
    kos::Semaphore g_key_swapped;

    void old_dtor(void*)
    {
        g_old_dtor_calls++;
    }

    void new_dtor(void* value)
    {
        g_new_dtor_calls++;
        g_new_dtor_arg = value;
    }

    void stale_holder()
    {
        (void)__gthread_setspecific(g_old_key, &g_stale_value);
        (void)g_key_set.post();
        (void)g_key_swapped.wait();
        g_seen_under_new = __gthread_getspecific(g_new_key);
    }

    void fresh_holder()
    {
        (void)__gthread_setspecific(g_new_key, &g_fresh_value);
    }

    // A joiner that did not create the thread it joins: a thread of its own, above the threads
    // it joins, so on one core it runs the moment the join lets it go.
    std::thread g_joinee;
    bool g_nc_joined = false;
    bool g_nc_created = false;
    bool g_nc_refused = false;
    kos::Semaphore g_nc_block;

    void returns_now()
    {
    }

    void exits_early()
    {
        kos_exit(0);
    }

    void blocks_on_semaphore()
    {
        (void)g_nc_block.wait();
    }

    // With the task at its thread budget, the create right after the join needs the slot of
    // the thread just joined.
    void nc_joiner(void*)
    {
        try
        {
            g_joinee.join();
            g_nc_joined = true;
            std::thread n(returns_now);
            n.join();
            g_nc_created = true;
        }
        catch (std::system_error const&)
        {
            g_nc_refused = true;
        }
    }

    bool joined_by_another(void (*body)(), bool cancel)
    {
        g_nc_joined = false;
        g_nc_created = false;
        g_nc_refused = false;
        g_joinee = std::thread(body);
        auto joiner = kos::thread::create(nc_joiner, nullptr, "ncjoin", 20);
        if (not joiner.valid())
        {
            return false;
        }
        if (cancel)
        {
            (void)kos_thread_cancel(g_joinee.native_handle(), KOS_CANCEL_SLAY, 0);
        }
        (void)joiner.join(KOS_TIMEOUT_NONE);
        return g_nc_joined and g_nc_created and not g_nc_refused;
    }

#if KICKOS_TLS
    // Zero-initialised and dynamically built only: no thread_local in this image has an initial
    // value, so its thread-local storage is .tbss alone, which a new thread must find zeroed.
    thread_local volatile uint32_t t_zeroed[4];
    uint32_t g_tl_built = 0;
    bool g_tl_fresh[2] = {false, false};

    struct TlBuilt
    {
        TlBuilt()
        {
            bump(&g_tl_built);
            value = 7;
        }
        int value = 0;
    };

    void tl_fresh(int i)
    {
        bool zero = true;
        for (uint32_t w = 0; w < 4u; w++)
        {
            if (t_zeroed[w] != 0u)
            {
                zero = false;
            }
            t_zeroed[w] = 0xC0DE0000u + w;
        }
        thread_local TlBuilt built;
        g_tl_fresh[i] = zero and built.value == 7;
    }
#endif

    // A key destructor that registers a thread_local destructor, as a thread_local first built
    // there does, after the thread's thread_local destructors ran.
    __gthread_key_t g_late_key;
    bool g_late_destroyed = false;
    int g_late_value = 3;

    void late_destroy(void*)
    {
        g_late_destroyed = true;
    }

    void late_key_dtor(void*)
    {
        (void)__cxxabiv1::__cxa_thread_atexit(late_destroy, nullptr, nullptr);
    }

    void late_setter()
    {
        (void)__gthread_setspecific(g_late_key, &g_late_value);
    }

    bool key_dtor_registration_runs()
    {
        g_late_destroyed = false;
        if (__gthread_key_create(&g_late_key, late_key_dtor) != 0)
        {
            return false;
        }
        std::thread t(late_setter);
        t.join();
        (void)__gthread_key_delete(g_late_key);
        return g_late_destroyed;
    }

    // A detached thread that starts and joins a thread, fills blocks the size of a thread's
    // record, then starts and detaches another before it returns: a record of its own freed
    // while it ran is one of those blocks by then, and its return writes into it.
    constexpr int SPRAY = 8;
    constexpr uint32_t SPRAY_FILL = 0xA5A5A5A5u;
    constexpr size_t SPRAY_WORDS = 5u * sizeof(void*) / sizeof(uint32_t);
    constexpr uint32_t GONE_BOUND_US = 2000000u;
    uint32_t* g_spray[SPRAY];
    kos::Semaphore g_loose_go;
    bool g_loose_child_gone = false;
    bool g_loose_ran = false;

    void loose_parent()
    {
        (void)g_loose_go.wait();
        std::thread a(returns_now);
        a.join();
        for (int i = 0; i < SPRAY; i++)
        {
            g_spray[i] = static_cast<uint32_t*>(malloc(SPRAY_WORDS * sizeof(uint32_t)));
            if (g_spray[i] != nullptr)
            {
                for (size_t w = 0; w < SPRAY_WORDS; w++)
                {
                    g_spray[i][w] = SPRAY_FILL;
                }
            }
        }
        std::thread b(returns_now);
        kos_thread_t const child = b.native_handle();
        b.detach();
        g_loose_child_gone = kos_thread_join(child, GONE_BOUND_US) == 0;
        g_loose_ran = true;
    }

    bool detached_parent_keeps_its_record()
    {
        g_loose_ran = false;
        g_loose_child_gone = false;
        std::thread t(loose_parent);
        kos_thread_t const parent = t.native_handle();
        t.detach();
        (void)g_loose_go.post();
        bool const parent_gone = kos_thread_join(parent, GONE_BOUND_US) == 0;
        bool intact = true;
        for (int i = 0; i < SPRAY; i++)
        {
            if (g_spray[i] == nullptr)
            {
                intact = false;
                continue;
            }
            for (size_t w = 0; w < SPRAY_WORDS; w++)
            {
                if (g_spray[i][w] != SPRAY_FILL)
                {
                    intact = false;
                }
            }
            free(g_spray[i]);
            g_spray[i] = nullptr;
        }
        return parent_gone and g_loose_child_gone and g_loose_ran and intact;
    }

    unsigned heap_in_use()
    {
        return static_cast<unsigned>(mallinfo().uordblks);
    }

    bool detached_leave_no_record()
    {
        unsigned const before = heap_in_use();
        bool gone = true;
        for (int i = 0; i < 3; i++)
        {
            std::thread t(returns_now);
            kos_thread_t const h = t.native_handle();
            t.detach();
            if (kos_thread_join(h, GONE_BOUND_US) != 0)
            {
                gone = false;
            }
        }
        return gone and heap_in_use() == before;
    }

    // Each thread holds a caught exception while the other throws, catches and rethrows its own,
    // and each counts only its own uncaught exception.
    constexpr uint32_t HOLD_BOUND_US = 2000000u;
    kos::Semaphore g_a_unwinding;
    kos::Semaphore g_b_counted;
    kos::Semaphore g_a_caught;
    kos::Semaphore g_b_caught;
    kos::Semaphore g_a_rethrown;
    kos::Semaphore g_b_rethrown;
    int g_a_uncaught = -1;
    int g_b_uncaught = -1;
    bool g_a_own = false;
    bool g_b_own = false;

    struct Unwinding
    {
        ~Unwinding()
        {
            g_a_uncaught = std::uncaught_exceptions();
            (void)g_a_unwinding.post();
            (void)g_b_counted.wait(HOLD_BOUND_US);
        }
    };

    bool rethrows_own(char want)
    {
        try
        {
            throw;
        }
        catch (std::runtime_error const& e)
        {
            return e.what()[0] == want;
        }
        catch (...)
        {
            return false;
        }
    }

    void holder_a()
    {
        try
        {
            Unwinding u;
            throw std::runtime_error("a");
        }
        catch (std::runtime_error const&)
        {
            (void)g_a_caught.post();
            (void)g_b_caught.wait(HOLD_BOUND_US);
            g_a_own = rethrows_own('a');
            (void)g_a_rethrown.post();
            (void)g_b_rethrown.wait(HOLD_BOUND_US);
        }
    }

    void holder_b()
    {
        (void)g_a_unwinding.wait(HOLD_BOUND_US);
        g_b_uncaught = std::uncaught_exceptions();
        (void)g_b_counted.post();
        (void)g_a_caught.wait(HOLD_BOUND_US);
        try
        {
            throw std::runtime_error("b");
        }
        catch (std::runtime_error const&)
        {
            (void)g_b_caught.post();
            (void)g_a_rethrown.wait(HOLD_BOUND_US);
            g_b_own = rethrows_own('b');
            (void)g_b_rethrown.post();
        }
    }

    void key_reuse(void (*report)(char const*, bool))
    {
        int n = 0;
        while (n < KICKOS_THREAD_KEYS and __gthread_key_create(&g_fill[n], nullptr) == 0)
        {
            n++;
        }
        bool reused = n > 0 and __gthread_key_delete(g_fill[n - 1]) == 0
                      and __gthread_key_create(&g_old_key, old_dtor) == 0;
        bool fresh = false;
        if (reused)
        {
            std::thread t(stale_holder);
            (void)g_key_set.wait();
            reused = __gthread_key_delete(g_old_key) == 0
                     and __gthread_key_create(&g_new_key, new_dtor) == 0
                     and g_new_key != g_old_key;
            (void)g_key_swapped.post();
            t.join();
            if (reused)
            {
                std::thread u(fresh_holder);
                u.join();
                fresh = g_seen_under_new == nullptr and g_old_dtor_calls == 0
                        and g_new_dtor_calls == 1 and g_new_dtor_arg == &g_fresh_value;
                (void)__gthread_key_delete(g_new_key);
            }
        }
        for (int i = 0; i + 1 < n; i++)
        {
            (void)__gthread_key_delete(g_fill[i]);
        }
        report("a deleted key's index serves a new key", reused);
        report("a value a deleted key left behind is never the new key's", fresh);
    }
}

void cxxtest_first_throws(void (*report)(char const*, bool))
{
    std::thread a(first_throw, 0);
    std::thread b(first_throw, 1);
    a.join();
    b.join();
    report("two threads' first throws at once are both caught",
           g_throw_caught[0] == 1 and g_throw_caught[1] == 1);
}

void cxxtest_threads(void (*report)(char const*, bool))
{
    try
    {
        {
            std::thread a(slow_user, 0);
            std::thread b(slow_user, 1);
            a.join();
            b.join();
            report(
                "a static two threads reach at once is built once",
                read(&g_slow_built) == 1 and g_slow_seen[0] == 42 and g_slow_seen[1] == 42);
        }
        {
            std::thread a(once_user, 0);
            std::thread b(once_user, 1);
            once_user(2);
            a.join();
            b.join();
            report("a contended call_once runs once and every caller sees its effect",
                   read(&g_once_runs) == 1 and g_once_seen[0] == 7 and g_once_seen[1] == 7
                       and g_once_seen[2] == 7);
        }
        {
            bool threw = false;
            try
            {
                std::call_once(g_throwing_once, throws_first);
            }
            catch (std::runtime_error const&)
            {
                threw = true;
            }
            std::call_once(g_throwing_once, throws_first);
            std::call_once(g_throwing_once, throws_first);
            report("a call_once whose function threw runs again, then never",
                   threw and g_throwing_tries == 2);
        }
        {
            g_shared = std::make_shared<int>(5);
            g_churn_done.store(false);
            auto irq = kos::thread::create(interrupter, nullptr, "interrupter", 20);
            std::thread a(churn);
            churn();
            a.join();
            g_churn_done.store(true);
            bool const spawned = irq.valid();
            if (spawned)
            {
                (void)irq.join(KOS_TIMEOUT_NONE);
            }
            report("a shared_ptr two threads copy and drop keeps its count",
                   spawned and g_shared.use_count() == 1);
            g_shared.reset();
        }
        {
            std::thread a(player, 0);
            std::thread b(player, 1);
            a.join();
            b.join();
            report("two std::threads take turns over a condition variable",
                   g_pp_count == 2 * ROUNDS);
        }
        {
            std::unique_lock<std::mutex> lock(g_pp_mutex);
            auto const t0 = std::chrono::steady_clock::now();
            std::cv_status const st = g_pp_cv.wait_for(lock, 20ms);
            auto const waited = std::chrono::steady_clock::now() - t0;
            report("a timed condition wait ends at its deadline",
                   st == std::cv_status::timeout and waited >= 20ms and waited < 2s);
        }
        {
            g_timed.lock();
            std::thread t(timed_taker);
            t.join();
            g_timed.unlock();
            report("a timed lock of a held mutex ends at its deadline",
                   not g_timed_got and g_timed_waited >= 10ms and g_timed_waited < 2s);
        }
        {
            g_rtimed.lock();
            g_rtimed.lock();
            std::thread held(rtimed_held_taker);
            held.join();
            g_rtimed.unlock();
            std::thread still(rtimed_held_taker);
            still.join();
            bool const one_left = not g_rtimed_held_got;
            g_rtimed.unlock();
            std::thread freed(rtimed_free_taker);
            freed.join();
            report("a recursive timed lock waits until every level is released",
                   one_left and g_rtimed_waited >= 10ms and g_rtimed_waited < 2s
                       and g_rtimed_free_got);
        }
        report("a recursive wait held two deep gives both levels back",
               recursive_wait_keeps_depth());
#if KICKOS_KERNEL_CORES == 1
        {
            g_step.store(0);
            std::thread t(stepper);
            kos_yield();
            int const seen = g_step.load();
            t.join();
            report("a std::thread runs at its creator's priority", seen == 1);
        }
#endif
        {
            kos_thread_params p = {};
            p.entry = inherit_entry;
            p.flags = KOS_THREAD_INHERIT_PRIO;
            kos_thread_t h = KOS_THREAD_NONE;
            int const rc = kos_thread_create(&p, &h);
            if (rc == 0)
            {
                (void)kos_thread_join(h, KOS_TIMEOUT_NONE);
            }
            report("a thread inheriting its priority ignores the one it names", rc == 0);
        }
        {
            std::thread a(holder_a);
            std::thread b(holder_b);
            a.join();
            b.join();
            report("each thread rethrows its own caught exception while another holds one",
                   g_a_own and g_b_own);
            report("each thread counts only its own uncaught exception",
                   g_a_uncaught == 1 and g_b_uncaught == 0);
        }
        key_reuse(report);
        report("a thread a non-creator joined has its slot free again",
               joined_by_another(returns_now, false));
        report("a join returns for a thread that ended by kos_exit",
               joined_by_another(exits_early, false));
        report("a join returns for a thread its creator cancelled",
               joined_by_another(blocks_on_semaphore, true));
#if KICKOS_TLS
        {
            std::thread a(tl_fresh, 0);
            a.join();
            std::thread b(tl_fresh, 1);
            b.join();
            report("a new thread's thread_locals start zeroed and are built for it",
                   g_tl_fresh[0] and g_tl_fresh[1] and read(&g_tl_built) == 2u);
        }
#endif
        report("a thread_local destructor a key destructor registers runs",
               key_dtor_registration_runs());
        report("a detached thread that starts and detaches threads keeps its own record",
               detached_parent_keeps_its_record());
        report("a detached thread that returned leaves no record behind",
               detached_leave_no_record());
        {
            int const mask = kos_task_cores(KOS_TASK_NONE);
            unsigned cores = 1u;
            if (mask != -KOS_ENOSYS)
            {
                cores = static_cast<unsigned>(__builtin_popcount(static_cast<uint32_t>(mask)));
            }
            report("hardware_concurrency counts the task's cores",
                   mask != 0 and std::thread::hardware_concurrency() == cores);
        }
    }
    catch (std::system_error const&)
    {
        report("a threaded check raised a system error", false);
    }
}
