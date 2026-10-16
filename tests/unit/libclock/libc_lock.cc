// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// newlib's locks over the address space's two kernel mutexes, the libc lock's and malloc's. The
// kernel here is a host mutex per lock behind kos_mutex_lock/unlock that answers as the kernel
// does: a holder relocking is -KOS_EDEADLK, a try on a mutex another thread holds -KOS_ETIMEDOUT,
// a non-holder unlocking -KOS_EPERM. Each host thread is a KickOS thread of its own.

#include <gtest/gtest.h>

#include <atomic>
#include <csignal>
#include <mutex>
#include <thread>
#include <vector>

#include <unistd.h>

#include <kickos/sys.h>

#include <sys/lock.h>

namespace
{
    struct KernelMutex
    {
        std::mutex m;
        std::atomic<kos_thread_t> holder{KOS_THREAD_NONE};
    };
    KernelMutex g_libc;
    KernelMutex g_malloc;
    std::atomic<int> g_locks{0};
    std::atomic<int> g_unlocks{0};
    std::atomic<int> g_other_caps{0};
    std::atomic<int> g_tries_refused{0};
    // The kernel's answer to the next lock it grants, or -KOS_EBADF for no lock at the handle.
    std::atomic<int> g_lock_answer{0};
    std::atomic<int> g_unlock_answer{0};
    // Cancelled waits the kernel answers before it grants, and every lock call it is asked.
    std::atomic<int> g_cancels_before_grant{0};
    std::atomic<int> g_lock_calls{0};
    // A thread whose cancelled wait asks again this often is taken as ended at its entry.
    constexpr int CANCEL_ENTRIES_BOUND = 1000;
    constexpr int ENDED_AT_ENTRY = 42;
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
            g_cancels_before_grant = 0;
            g_lock_calls = 0;
        }
        void TearDown() override
        {
            EXPECT_EQ(g_libc.holder.load(), KOS_THREAD_NONE) << "an arm left the libc lock held";
            EXPECT_EQ(g_malloc.holder.load(), KOS_THREAD_NONE)
                << "an arm left the malloc lock held";
            EXPECT_EQ(g_other_caps.load(), 0) << "a lock reached a mutex other than the two";
        }
    };

    using LibcLockDeathTest = LibcLock;
}

extern "C" kos_thread_t kos_thread_self(void)
{
    return self();
}

namespace
{
    KernelMutex* kernel_mutex(kos_cap_t mtx)
    {
        if (mtx == KOS_CAP_LIBC_LOCK)
        {
            return &g_libc;
        }
        if (mtx == KOS_CAP_MALLOC_LOCK)
        {
            return &g_malloc;
        }
        std::lock_guard<std::mutex> const hold(g_tally);
        g_other_caps.store(g_other_caps.load() + 1);
        return nullptr;
    }

    Named const& malloc_lock()
    {
        return g_static[3];
    }
}

extern "C" int kos_mutex_lock(kos_cap_t mtx, uint32_t timeout_us)
{
    KernelMutex* const k = kernel_mutex(mtx);
    int const answer = g_lock_answer.load();
    g_lock_calls.store(g_lock_calls.load() + 1);
    if (k == nullptr or answer == -KOS_EBADF)
    {
        return -KOS_EBADF;
    }
    if (g_cancels_before_grant.load() > 0)
    {
        g_cancels_before_grant.store(g_cancels_before_grant.load() - 1);
        return -KOS_ECANCELED;
    }
    if (answer == -KOS_ECANCELED)
    {
        if (g_lock_calls.load() >= CANCEL_ENTRIES_BOUND)
        {
            _exit(ENDED_AT_ENTRY);
        }
        return -KOS_ECANCELED;
    }
    if (k->holder.load() == self())
    {
        return -KOS_EDEADLK;
    }
    if (timeout_us == 0)
    {
        if (not k->m.try_lock())
        {
            g_tries_refused.store(g_tries_refused.load() + 1);
            return -KOS_ETIMEDOUT;
        }
    }
    else
    {
        EXPECT_EQ(timeout_us, KOS_TIMEOUT_NONE) << "a libc lock waited with a deadline";
        k->m.lock();
    }
    k->holder = self();
    g_locks.store(g_locks.load() + 1);
    return answer;
}

extern "C" int kos_mutex_unlock(kos_cap_t mtx)
{
    KernelMutex* const k = kernel_mutex(mtx);
    if (k == nullptr)
    {
        return -KOS_EBADF;
    }
    if (g_unlock_answer.load() != 0)
    {
        return g_unlock_answer.load();
    }
    if (k->holder.load() != self())
    {
        return -KOS_EPERM;
    }
    k->holder = KOS_THREAD_NONE;
    g_unlocks.store(g_unlocks.load() + 1);
    k->m.unlock();
    if (g_preempt_after_unlock.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return 0;
}

TEST_F(LibcLock, every_newlib_lock_but_mallocs_is_the_libc_mutex)
{
    _LOCK_T file_lock = nullptr;
    __retarget_lock_init_recursive(&file_lock);
    std::vector<Named> all;
    for (Named const& n : g_static)
    {
        if (n.lock != malloc_lock().lock)
        {
            all.push_back(n);
        }
    }
    all.push_back({file_lock, true});
    for (Named const& n : all)
    {
        int const locks = g_locks;
        int const unlocks = g_unlocks;
        acquire(n);
        EXPECT_EQ(g_libc.holder.load(), self());
        EXPECT_EQ(g_malloc.holder.load(), KOS_THREAD_NONE);
        EXPECT_EQ(g_locks.load(), locks + 1);
        release(n);
        EXPECT_EQ(g_unlocks.load(), unlocks + 1);
    }
    __retarget_lock_close_recursive(file_lock);
}

TEST_F(LibcLock, malloc_takes_a_mutex_of_its_own)
{
    acquire(malloc_lock());
    EXPECT_EQ(g_malloc.holder.load(), self());
    EXPECT_EQ(g_libc.holder.load(), KOS_THREAD_NONE);
    release(malloc_lock());
}

// The witness at its unit: the stdio side of newlib completes while another thread holds malloc.
TEST_F(LibcLock, a_file_lock_is_taken_while_another_thread_holds_malloc)
{
    acquire(malloc_lock());
    std::atomic<bool> printed{false};
    std::thread other([&] {
        _LOCK_T file_lock = nullptr;
        __retarget_lock_init_recursive(&file_lock);
        __retarget_lock_acquire_recursive(file_lock);
        __retarget_lock_acquire_recursive(&__lock___sfp_recursive_mutex);
        __retarget_lock_release_recursive(&__lock___sfp_recursive_mutex);
        __retarget_lock_release_recursive(file_lock);
        printed = true;
    });
    other.join();
    EXPECT_TRUE(printed.load());
    EXPECT_EQ(g_malloc.holder.load(), self());
    release(malloc_lock());
}

TEST_F(LibcLock, nesting_two_libc_locks_is_recursion_on_one)
{
    int const locks = g_locks;
    int const unlocks = g_unlocks;
    for (Named const& n : g_static)
    {
        if (n.lock != malloc_lock().lock)
        {
            acquire(n);
        }
    }
    EXPECT_EQ(g_locks.load(), locks + 1);
    for (Named const& n : g_static)
    {
        if (n.lock != malloc_lock().lock)
        {
            EXPECT_EQ(g_unlocks.load(), unlocks);
            release(n);
        }
    }
    EXPECT_EQ(g_unlocks.load(), unlocks + 1);
}

TEST_F(LibcLock, malloc_inside_the_libc_lock_takes_both_mutexes)
{
    int const locks = g_locks;
    acquire(g_static[0]);
    acquire(malloc_lock());
    EXPECT_EQ(g_locks.load(), locks + 2);
    EXPECT_EQ(g_libc.holder.load(), self());
    EXPECT_EQ(g_malloc.holder.load(), self());
    release(malloc_lock());
    EXPECT_EQ(g_malloc.holder.load(), KOS_THREAD_NONE);
    EXPECT_EQ(g_libc.holder.load(), self());
    release(g_static[0]);
}

TEST_F(LibcLock, the_depth_counts_down_to_the_unlock)
{
    int const unlocks = g_unlocks;
    acquire(malloc_lock());
    acquire(malloc_lock());
    acquire(malloc_lock());
    release(malloc_lock());
    release(malloc_lock());
    EXPECT_EQ(g_malloc.holder.load(), self());
    EXPECT_EQ(g_unlocks.load(), unlocks);
    release(malloc_lock());
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

TEST_F(LibcLock, a_dead_owner_of_the_libc_lock_hands_it_over_held)
{
    g_lock_answer = -KOS_EOWNERDEAD;
    int const locks = g_locks;
    acquire(g_static[0]);
    acquire(g_static[1]);
    EXPECT_EQ(g_libc.holder.load(), self());
    EXPECT_EQ(g_locks.load(), locks + 1);
    release(g_static[1]);
    release(g_static[0]);
}

TEST_F(LibcLock, a_cancelled_wait_asks_again_and_takes_the_lock_once)
{
    g_cancels_before_grant = 1;
    int const locks = g_locks;
    acquire(g_static[0]);
    EXPECT_EQ(g_lock_calls.load(), 2) << "the cancelled wait did not ask exactly once more";
    EXPECT_EQ(g_locks.load(), locks + 1);
    EXPECT_EQ(g_libc.holder.load(), self());
    release(g_static[0]);
    EXPECT_EQ(g_libc.holder.load(), KOS_THREAD_NONE) << "the take left a depth other than one";
}

TEST_F(LibcLock, a_try_takes_a_free_lock)
{
    int const locks = g_locks;
    EXPECT_EQ(__retarget_lock_try_acquire_recursive(&__lock___sfp_recursive_mutex), 1);
    EXPECT_EQ(g_libc.holder.load(), self());
    EXPECT_EQ(g_locks.load(), locks + 1);
    EXPECT_EQ(__retarget_lock_try_acquire(&__lock___tz_mutex), 1) << "the owner's try is a nest";
    EXPECT_EQ(g_locks.load(), locks + 1);
    release(g_static[5]);
    release(g_static[0]);
    EXPECT_EQ(g_libc.holder.load(), KOS_THREAD_NONE);
}

TEST_F(LibcLock, a_try_on_a_lock_another_thread_holds_fails_and_holds_nothing)
{
    acquire(g_static[1]);
    int const refused = g_tries_refused;
    std::atomic<int> tried{-1};
    std::thread other([&] { tried = __retarget_lock_try_acquire(&__lock___tz_mutex); });
    other.join();
    EXPECT_EQ(tried.load(), 0);
    EXPECT_EQ(g_tries_refused.load(), refused + 1) << "the try never reached the kernel";
    EXPECT_EQ(g_libc.holder.load(), self());
    release(g_static[1]);
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
                acquire(g_static[0]);
                acquire(malloc_lock());
                int const seen = count;
                std::this_thread::yield();
                count = seen + 1;
                release(malloc_lock());
                release(g_static[0]);
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

TEST_F(LibcLockDeathTest, a_dead_owner_of_the_malloc_lock_traps)
{
    g_lock_answer = -KOS_EOWNERDEAD;
    EXPECT_EXIT(acquire(malloc_lock()), ::testing::KilledBySignal(SIGILL), "");
    g_lock_answer = 0;
}

TEST_F(LibcLockDeathTest, the_libc_lock_taken_inside_mallocs_traps)
{
    EXPECT_EXIT(
        {
            acquire(malloc_lock());
            acquire(g_static[0]);
        },
        ::testing::KilledBySignal(SIGILL), "");
    EXPECT_EXIT(
        {
            acquire(malloc_lock());
            (void)__retarget_lock_try_acquire_recursive(&__lock___sfp_recursive_mutex);
        },
        ::testing::KilledBySignal(SIGILL), "");
}

// The re-ask loop is sound only because the kernel ends a cancelled thread at its next entry.
// This kernel never grants, and stands for that end after a bound so the test cannot hang.
TEST_F(LibcLockDeathTest, a_wait_cancelled_for_good_ends_at_an_entry_and_never_traps)
{
    g_lock_answer = -KOS_ECANCELED;
    EXPECT_EXIT(acquire(g_static[0]), ::testing::ExitedWithCode(ENDED_AT_ENTRY), "");
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
