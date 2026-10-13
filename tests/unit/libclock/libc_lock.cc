// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// newlib's locks over one kernel mutex. The kernel here is a host mutex behind
// kos_mutex_lock/unlock that answers as the kernel does: a holder relocking is -KOS_EDEADLK, a
// try on a mutex another thread holds -KOS_ETIMEDOUT, a non-holder unlocking -KOS_EPERM. Each
// host thread is a KickOS thread of its own.

#include <gtest/gtest.h>

#include <atomic>
#include <csignal>
#include <mutex>
#include <thread>
#include <vector>

#include <kickos/sys.h>

#include <sys/lock.h>

namespace
{
    std::mutex g_kernel;
    std::atomic<kos_thread_t> g_holder{KOS_THREAD_NONE};
    std::atomic<int> g_locks{0};
    std::atomic<int> g_unlocks{0};
    std::atomic<int> g_other_caps{0};
    std::atomic<int> g_tries_refused{0};
    // The kernel's answer to the next lock it grants, or -KOS_EBADF for no lock at the index.
    std::atomic<int> g_lock_answer{0};
    std::atomic<int> g_unlock_answer{0};
    // The unlocking thread is preempted on its way out, so a waiter runs before it returns.
    std::atomic<bool> g_preempt_after_unlock{false};
    // Serialises the tallies a non-holder writes: the next thread handle and a stray cap.
    std::mutex g_tally;
    kos_thread_t g_next_thread = 1;
    thread_local kos_thread_t t_self = KOS_THREAD_NONE;

    kos_thread_t self()
    {
        if (t_self == KOS_THREAD_NONE)
        {
            std::lock_guard<std::mutex> const hold(g_tally);
            t_self = g_next_thread;
            g_next_thread = g_next_thread + 1;
        }
        return t_self;
    }

    struct Named
    {
        _LOCK_T lock;
        bool recursive;
    };

    // Every static lock newlib names, with the entry point its own callers use.
    Named const g_static[] = {
        {&__lock___sfp_recursive_mutex, true},    {&__lock___atexit_recursive_mutex, true},
        {&__lock___at_quick_exit_mutex, false},   {&__lock___malloc_recursive_mutex, true},
        {&__lock___env_recursive_mutex, true},    {&__lock___tz_mutex, false},
        {&__lock___dd_hash_mutex, false},         {&__lock___arc4random_mutex, false},
    };

    void acquire(Named const& n)
    {
        if (n.recursive)
        {
            __retarget_lock_acquire_recursive(n.lock);
            return;
        }
        __retarget_lock_acquire(n.lock);
    }

    void release(Named const& n)
    {
        if (n.recursive)
        {
            __retarget_lock_release_recursive(n.lock);
            return;
        }
        __retarget_lock_release(n.lock);
    }

    struct LibcLock : ::testing::Test
    {
        void SetUp() override
        {
            g_lock_answer = 0;
            g_unlock_answer = 0;
            g_other_caps = 0;
        }
        void TearDown() override
        {
            EXPECT_EQ(g_holder.load(), KOS_THREAD_NONE) << "an arm left the kernel mutex held";
            EXPECT_EQ(g_other_caps.load(), 0) << "a lock reached a mutex other than the libc one";
        }
    };

    using LibcLockDeathTest = LibcLock;
}

extern "C" kos_thread_t kos_thread_self(void)
{
    return self();
}

extern "C" int kos_mutex_lock(kos_cap_t mtx, uint32_t timeout_us)
{
    if (mtx != KOS_CAP_LIBC_LOCK)
    {
        std::lock_guard<std::mutex> const hold(g_tally);
        g_other_caps.store(g_other_caps.load() + 1);
    }
    int const answer = g_lock_answer.load();
    if (answer == -KOS_EBADF)
    {
        return answer;
    }
    if (g_holder.load() == self())
    {
        return -KOS_EDEADLK;
    }
    if (timeout_us == 0)
    {
        if (not g_kernel.try_lock())
        {
            g_tries_refused.store(g_tries_refused.load() + 1);
            return -KOS_ETIMEDOUT;
        }
    }
    else
    {
        EXPECT_EQ(timeout_us, KOS_TIMEOUT_NONE) << "a libc lock waited with a deadline";
        g_kernel.lock();
    }
    g_holder = self();
    g_locks.store(g_locks.load() + 1);
    return answer;
}

extern "C" int kos_mutex_unlock(kos_cap_t mtx)
{
    if (mtx != KOS_CAP_LIBC_LOCK)
    {
        std::lock_guard<std::mutex> const hold(g_tally);
        g_other_caps.store(g_other_caps.load() + 1);
    }
    if (g_unlock_answer.load() != 0)
    {
        return g_unlock_answer.load();
    }
    if (g_holder.load() != self())
    {
        return -KOS_EPERM;
    }
    g_holder = KOS_THREAD_NONE;
    g_unlocks.store(g_unlocks.load() + 1);
    g_kernel.unlock();
    if (g_preempt_after_unlock.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 0;
}

TEST_F(LibcLock, every_newlib_lock_is_the_one_kernel_mutex)
{
    _LOCK_T file_lock = nullptr;
    __retarget_lock_init_recursive(&file_lock);
    std::vector<Named> all(std::begin(g_static), std::end(g_static));
    all.push_back({file_lock, true});
    for (Named const& n : all)
    {
        int const locks = g_locks;
        int const unlocks = g_unlocks;
        acquire(n);
        EXPECT_EQ(g_holder.load(), self());
        EXPECT_EQ(g_locks.load(), locks + 1);
        release(n);
        EXPECT_EQ(g_unlocks.load(), unlocks + 1);
    }
    __retarget_lock_close_recursive(file_lock);
}

TEST_F(LibcLock, nesting_two_locks_is_recursion_on_one)
{
    int const locks = g_locks;
    int const unlocks = g_unlocks;
    for (Named const& n : g_static)
    {
        acquire(n);
    }
    EXPECT_EQ(g_locks.load(), locks + 1);
    for (Named const& n : g_static)
    {
        EXPECT_EQ(g_unlocks.load(), unlocks);
        release(n);
    }
    EXPECT_EQ(g_unlocks.load(), unlocks + 1);
}

TEST_F(LibcLock, the_depth_counts_down_to_the_unlock)
{
    Named const& malloc_lock = g_static[3];
    int const unlocks = g_unlocks;
    acquire(malloc_lock);
    acquire(malloc_lock);
    acquire(malloc_lock);
    release(malloc_lock);
    release(malloc_lock);
    EXPECT_EQ(g_holder.load(), self());
    EXPECT_EQ(g_unlocks.load(), unlocks);
    release(malloc_lock);
    EXPECT_EQ(g_unlocks.load(), unlocks + 1);
}

TEST_F(LibcLock, another_thread_waits_for_the_owner)
{
    Named const& sfp = g_static[0];
    acquire(sfp);
    std::atomic<bool> entered{false};
    std::thread other([&] {
        acquire(sfp);
        entered = true;
        release(sfp);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(entered.load()) << "a second thread recursed on the owner's depth";
    release(sfp);
    other.join();
    EXPECT_TRUE(entered.load());
}

TEST_F(LibcLock, a_releaser_preempted_after_its_unlock_leaves_the_next_owner_whole)
{
    Named const& sfp = g_static[0];
    acquire(sfp);
    std::atomic<bool> nested{false};
    std::thread next([&] {
        acquire(sfp);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        acquire(sfp);
        nested = true;
        release(sfp);
        release(sfp);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    g_preempt_after_unlock = true;
    release(sfp);
    g_preempt_after_unlock = false;
    next.join();
    EXPECT_TRUE(nested.load());
}

TEST_F(LibcLock, a_dead_owner_hands_the_lock_over_held)
{
    g_lock_answer = -KOS_EOWNERDEAD;
    int const locks = g_locks;
    acquire(g_static[3]);
    acquire(g_static[0]);
    EXPECT_EQ(g_holder.load(), self());
    EXPECT_EQ(g_locks.load(), locks + 1);
    release(g_static[0]);
    release(g_static[3]);
}

TEST_F(LibcLock, a_try_takes_a_free_lock)
{
    int const locks = g_locks;
    EXPECT_EQ(__retarget_lock_try_acquire_recursive(&__lock___sfp_recursive_mutex), 1);
    EXPECT_EQ(g_holder.load(), self());
    EXPECT_EQ(g_locks.load(), locks + 1);
    EXPECT_EQ(__retarget_lock_try_acquire(&__lock___tz_mutex), 1) << "the owner's try is a nest";
    EXPECT_EQ(g_locks.load(), locks + 1);
    release(g_static[5]);
    release(g_static[0]);
    EXPECT_EQ(g_holder.load(), KOS_THREAD_NONE);
}

TEST_F(LibcLock, a_try_on_a_lock_another_thread_holds_fails_and_holds_nothing)
{
    acquire(g_static[3]);
    int const refused = g_tries_refused;
    std::atomic<int> tried{-1};
    std::thread other([&] { tried = __retarget_lock_try_acquire(&__lock___tz_mutex); });
    other.join();
    EXPECT_EQ(tried.load(), 0);
    EXPECT_EQ(g_tries_refused.load(), refused + 1) << "the try never reached the kernel";
    EXPECT_EQ(g_holder.load(), self());
    release(g_static[3]);
    std::thread after([&] {
        tried = __retarget_lock_try_acquire_recursive(&__lock___sfp_recursive_mutex);
        if (tried == 1)
        {
            release(g_static[0]);
        }
    });
    after.join();
    EXPECT_EQ(tried.load(), 1) << "a refused try left the lock looking held";
}

TEST_F(LibcLock, contending_threads_keep_a_shared_count_whole)
{
    constexpr int THREADS = 4;
    constexpr int ROUNDS = 5000;
    int count = 0;
    std::vector<std::thread> workers;
    for (int t = 0; t < THREADS; t++)
    {
        workers.emplace_back([&] {
            for (int i = 0; i < ROUNDS; i++)
            {
                acquire(g_static[3]);
                acquire(g_static[0]);
                int const seen = count;
                std::this_thread::yield();
                count = seen + 1;
                release(g_static[0]);
                release(g_static[3]);
            }
        });
    }
    for (std::thread& w : workers)
    {
        w.join();
    }
    EXPECT_EQ(count, THREADS * ROUNDS);
}

TEST_F(LibcLockDeathTest, a_missing_lock_traps)
{
    g_lock_answer = -KOS_EBADF;
    EXPECT_EXIT(acquire(g_static[3]), ::testing::KilledBySignal(SIGILL), "");
    g_lock_answer = 0;
}

TEST_F(LibcLockDeathTest, an_unlock_the_kernel_refuses_traps)
{
    EXPECT_EXIT(
        {
            acquire(g_static[3]);
            g_unlock_answer = -KOS_EPERM;
            release(g_static[3]);
        },
        ::testing::KilledBySignal(SIGILL), "");
}

TEST_F(LibcLockDeathTest, a_release_without_a_hold_traps)
{
    EXPECT_EXIT(release(g_static[3]), ::testing::KilledBySignal(SIGILL), "");
}

TEST_F(LibcLockDeathTest, a_release_by_a_thread_that_does_not_hold_it_traps)
{
    EXPECT_EXIT(
        {
            acquire(g_static[3]);
            std::thread other([] { release(g_static[3]); });
            other.join();
        },
        ::testing::KilledBySignal(SIGILL), "");
}
