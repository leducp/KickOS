// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// newlib's locks over the two mutex words of the address space, the libc lock's and malloc's,
// taken through the real user/src/mutex.cc over a host compare-and-swap. The kernel here answers
// the two contended entries as the kernel does: a dead holder's word is stolen with
// -KOS_EOWNERDEAD, a try on a word another thread holds -KOS_ETIMEDOUT, a wait parks until an
// unlock hands the word over, a non-holder unlocking -KOS_EPERM. Each host thread is a KickOS
// thread of its own.

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include <unistd.h>

#include <kickos/arch/arch.h>
#include <kickos/sys.h>
#include <kickos/sys/user_cas.h>

#include <sys/lock.h>

extern "C" uint32_t __real_kickos_user_cas(uint32_t* word, uint32_t expected, uint32_t desired);

namespace
{
    // An id no live thread carries: a holder that died.
    constexpr uint32_t DEAD_ID = 0x7FFFFFF0u;
    // An id a live thread elsewhere carries, which never releases.
    constexpr uint32_t OTHER_ID = 0x7FFFFFF1u;

    // The tallies, each written under g_tally.
    std::mutex g_tally;
    // The fake kernel's own exclusion, and the waiters it parks per word, in arrival order.
    std::mutex g_kernel;
    std::condition_variable g_handed;
    std::deque<uint32_t> g_waiters_libc;
    std::deque<uint32_t> g_waiters_malloc;

    int g_locks = 0;
    int g_unlocks = 0;
    int g_other_words = 0;
    int g_tries_refused = 0;
    int g_cas_calls = 0;
    // The kernel's answer to the next contended lock, when it is a refusal.
    std::atomic<int> g_lock_answer{0};
    std::atomic<int> g_unlock_answer{0};
    // Cancelled waits the kernel answers before it grants, and every lock entry it is asked.
    std::atomic<int> g_cancels_before_grant{0};
    int g_lock_calls = 0;
    // A thread whose cancelled wait asks again this often is taken as ended at its entry.
    constexpr int CANCEL_ENTRIES_BOUND = 1000;
    constexpr int ENDED_AT_ENTRY = 42;
    // The unlocking thread is preempted on its way out, so a waiter runs before it returns.
    std::atomic<bool> g_preempt_after_unlock{false};
    uint32_t g_next_thread = 1;
    thread_local uint32_t t_self = 0;

    void bump(int& tally)
    {
        std::lock_guard<std::mutex> const hold(g_tally);
        tally = tally + 1;
    }

    int tally_of(int const& tally)
    {
        std::lock_guard<std::mutex> const hold(g_tally);
        return tally;
    }

    uint32_t self()
    {
        if (t_self == 0)
        {
            std::lock_guard<std::mutex> const hold(g_tally);
            t_self = g_next_thread;
            g_next_thread = g_next_thread + 1;
        }
        return t_self;
    }

    uint32_t* libc_word()
    {
        return &kos_libc_lock(KOS_LIBC_LOCK_STDIO)->word;
    }

    uint32_t* malloc_word()
    {
        return &kos_libc_lock(KOS_LIBC_LOCK_MALLOC)->word;
    }

    uint32_t holder_of(uint32_t* word)
    {
        return __atomic_load_n(word, __ATOMIC_ACQUIRE) & ~KOS_WORD_WAITERS;
    }

    std::deque<uint32_t>* waiters_of(uint32_t* word)
    {
        if (word == libc_word())
        {
            return &g_waiters_libc;
        }
        if (word == malloc_word())
        {
            return &g_waiters_malloc;
        }
        bump(g_other_words);
        return nullptr;
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
            g_other_words = 0;
            g_cancels_before_grant = 0;
            g_lock_calls = 0;
            g_cas_calls = 0;
            *libc_word() = 0;
            *malloc_word() = 0;
        }
        void TearDown() override
        {
            EXPECT_EQ(holder_of(libc_word()), 0u) << "an arm left the libc lock held";
            EXPECT_EQ(holder_of(malloc_word()), 0u) << "an arm left the malloc lock held";
            EXPECT_EQ(tally_of(g_other_words), 0) << "a lock reached a word other than the two";
        }
    };

    using LibcLockDeathTest = LibcLock;

    Named const& malloc_lock()
    {
        return g_static[3];
    }

    // The kernel's contended lock: a dead holder's word is stolen with -KOS_EOWNERDEAD, a held
    // one parks the caller until an unlock hands the word over, a try answers at once.
    int kernel_lock(uint32_t* word, uint32_t timeout_us)
    {
        bump(g_lock_calls);
        if (g_cancels_before_grant.load() > 0)
        {
            g_cancels_before_grant.store(g_cancels_before_grant.load() - 1);
            return -KOS_ECANCELED;
        }
        int const answer = g_lock_answer.load();
        if (answer == -KOS_ECANCELED and tally_of(g_lock_calls) >= CANCEL_ENTRIES_BOUND)
        {
            _exit(ENDED_AT_ENTRY);
        }
        if (answer != 0)
        {
            return answer;
        }
        std::unique_lock<std::mutex> hold(g_kernel);
        std::deque<uint32_t>* const waiters = waiters_of(word);
        if (waiters == nullptr)
        {
            return -KOS_EINVAL;
        }
        uint32_t const me = self();
        while (true)
        {
            uint32_t const v = __atomic_load_n(word, __ATOMIC_ACQUIRE);
            uint32_t const owner = v & ~KOS_WORD_WAITERS;
            if (owner == 0 or owner == DEAD_ID)
            {
                uint32_t next = me;
                if (not waiters->empty())
                {
                    next = next | KOS_WORD_WAITERS;
                }
                if (__real_kickos_user_cas(word, v, next) == v)
                {
                    bump(g_locks);
                    if (owner == DEAD_ID)
                    {
                        return -KOS_EOWNERDEAD;
                    }
                    return 0;
                }
                continue;
            }
            if (owner == me)
            {
                return -KOS_EDEADLK;
            }
            if (timeout_us == 0)
            {
                bump(g_tries_refused);
                return -KOS_ETIMEDOUT;
            }
            EXPECT_EQ(timeout_us, KOS_TIMEOUT_NONE) << "a libc lock waited with a deadline";
            if (__real_kickos_user_cas(word, v, v | KOS_WORD_WAITERS) != v)
            {
                continue;
            }
            waiters->push_back(me);
            g_handed.wait(hold, [&] { return holder_of(word) == me; });
            bump(g_locks);
            return 0;
        }
    }

    // The kernel's unlock of a word with waiters: the first one is handed the word.
    int kernel_unlock(uint32_t* word)
    {
        if (g_unlock_answer.load() != 0)
        {
            return g_unlock_answer.load();
        }
        {
            std::lock_guard<std::mutex> const hold(g_kernel);
            std::deque<uint32_t>* const waiters = waiters_of(word);
            if (waiters == nullptr)
            {
                return -KOS_EINVAL;
            }
            if (holder_of(word) != self())
            {
                return -KOS_EPERM;
            }
            uint32_t next = 0;
            if (not waiters->empty())
            {
                next = waiters->front();
                waiters->pop_front();
                if (not waiters->empty())
                {
                    next = next | KOS_WORD_WAITERS;
                }
            }
            __atomic_store_n(word, next, __ATOMIC_RELEASE);
            bump(g_unlocks);
        }
        g_handed.notify_all();
        if (g_preempt_after_unlock.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return 0;
    }
}

extern "C" uint32_t kickos_self_lock_id(void)
{
    return self();
}

// The sim's own compare-and-swap, linked in, counting the takes and releases the fast path
// completes alone (-Wl,--wrap in CMakeLists.txt).
extern "C" uint32_t __wrap_kickos_user_cas(uint32_t* word, uint32_t expected, uint32_t desired)
{
    bump(g_cas_calls);
    (void)waiters_of(word);
    uint32_t const held = __real_kickos_user_cas(word, expected, desired);
    if (held == expected and expected == 0)
    {
        bump(g_locks);
    }
    if (held == expected and desired == 0)
    {
        bump(g_unlocks);
    }
    return held;
}

extern "C" uintptr_t arch_syscall(uintptr_t nr, uintptr_t a0, uintptr_t a1, uintptr_t, uintptr_t)
{
    uint32_t* const word = reinterpret_cast<uint32_t*>(a0);
    if (nr == KOS_SYS_MUTEX_LOCK)
    {
        return static_cast<uintptr_t>(kernel_lock(word, static_cast<uint32_t>(a1)));
    }
    if (nr == KOS_SYS_MUTEX_UNLOCK)
    {
        return static_cast<uintptr_t>(kernel_unlock(word));
    }
    ADD_FAILURE() << "the libc locks made syscall " << nr;
    return static_cast<uintptr_t>(-KOS_ENOSYS);
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
        EXPECT_EQ(holder_of(libc_word()), self());
        EXPECT_EQ(holder_of(malloc_word()), 0u);
        EXPECT_EQ(tally_of(g_locks), locks + 1);
        release(n);
        EXPECT_EQ(tally_of(g_unlocks), unlocks + 1);
    }
    __retarget_lock_close_recursive(file_lock);
}

TEST_F(LibcLock, malloc_takes_a_mutex_of_its_own)
{
    acquire(malloc_lock());
    EXPECT_EQ(holder_of(malloc_word()), self());
    EXPECT_EQ(holder_of(libc_word()), 0u);
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
    EXPECT_EQ(holder_of(malloc_word()), self());
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
    EXPECT_EQ(tally_of(g_locks), locks + 1);
    for (Named const& n : g_static)
    {
        if (n.lock != malloc_lock().lock)
        {
            EXPECT_EQ(tally_of(g_unlocks), unlocks);
            release(n);
        }
    }
    EXPECT_EQ(tally_of(g_unlocks), unlocks + 1);
}

TEST_F(LibcLock, malloc_inside_the_libc_lock_takes_both_mutexes)
{
    int const locks = g_locks;
    acquire(g_static[0]);
    acquire(malloc_lock());
    EXPECT_EQ(tally_of(g_locks), locks + 2);
    EXPECT_EQ(holder_of(libc_word()), self());
    EXPECT_EQ(holder_of(malloc_word()), self());
    release(malloc_lock());
    EXPECT_EQ(holder_of(malloc_word()), 0u);
    EXPECT_EQ(holder_of(libc_word()), self());
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
    EXPECT_EQ(holder_of(malloc_word()), self());
    EXPECT_EQ(tally_of(g_unlocks), unlocks);
    release(malloc_lock());
    EXPECT_EQ(tally_of(g_unlocks), unlocks + 1);
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
    *libc_word() = DEAD_ID;
    int const locks = g_locks;
    acquire(g_static[0]);
    acquire(g_static[1]);
    EXPECT_EQ(holder_of(libc_word()), self());
    EXPECT_EQ(tally_of(g_locks), locks + 1);
    release(g_static[1]);
    release(g_static[0]);
}

// The re-ask enters the kernel with no compare-and-swap of its own, so a killed waiter ends at
// that entry and never takes a lock freed meanwhile with no entry at all.
TEST_F(LibcLock, a_cancelled_wait_asks_again_at_the_entry_and_takes_the_lock_once)
{
    *libc_word() = DEAD_ID;
    g_cancels_before_grant = 1;
    int const locks = g_locks;
    acquire(g_static[0]);
    EXPECT_EQ(tally_of(g_lock_calls), 2) << "the cancelled wait did not ask exactly once more";
    EXPECT_EQ(tally_of(g_cas_calls), 1) << "the re-ask went through the compare-and-swap";
    EXPECT_EQ(tally_of(g_locks), locks + 1);
    EXPECT_EQ(holder_of(libc_word()), self());
    release(g_static[0]);
    EXPECT_EQ(holder_of(libc_word()), 0u) << "the take left a depth other than one";
}

TEST_F(LibcLock, a_try_takes_a_free_lock)
{
    int const locks = g_locks;
    EXPECT_EQ(__retarget_lock_try_acquire_recursive(&__lock___sfp_recursive_mutex), 1);
    EXPECT_EQ(holder_of(libc_word()), self());
    EXPECT_EQ(tally_of(g_locks), locks + 1);
    EXPECT_EQ(__retarget_lock_try_acquire(&__lock___tz_mutex), 1) << "the owner's try is a nest";
    EXPECT_EQ(tally_of(g_locks), locks + 1);
    release(g_static[5]);
    release(g_static[0]);
    EXPECT_EQ(holder_of(libc_word()), 0u);
}

TEST_F(LibcLock, a_try_on_a_lock_another_thread_holds_fails_and_holds_nothing)
{
    acquire(g_static[1]);
    int const refused = g_tries_refused;
    std::atomic<int> tried{-1};
    std::thread other([&] { tried = __retarget_lock_try_acquire(&__lock___tz_mutex); });
    other.join();
    EXPECT_EQ(tried.load(), 0);
    EXPECT_EQ(tally_of(g_tries_refused), refused + 1) << "the try never reached the kernel";
    EXPECT_EQ(holder_of(libc_word()), self());
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

TEST_F(LibcLockDeathTest, a_kernel_refusal_traps)
{
    *malloc_word() = OTHER_ID;
    g_lock_answer = -KOS_EINVAL;
    EXPECT_EXIT(acquire(g_static[3]), ::testing::KilledBySignal(SIGILL), "");
    g_lock_answer = 0;
    *malloc_word() = 0;
}

TEST_F(LibcLockDeathTest, a_dead_owner_of_the_malloc_lock_traps)
{
    *malloc_word() = DEAD_ID;
    EXPECT_EXIT(acquire(malloc_lock()), ::testing::KilledBySignal(SIGILL), "");
    *malloc_word() = 0;
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
    *libc_word() = OTHER_ID;
    g_lock_answer = -KOS_ECANCELED;
    EXPECT_EXIT(acquire(g_static[0]), ::testing::ExitedWithCode(ENDED_AT_ENTRY), "");
    g_lock_answer = 0;
    *libc_word() = 0;
}

// The release enters only for a word with waiters.
TEST_F(LibcLockDeathTest, an_unlock_the_kernel_refuses_traps)
{
    EXPECT_EXIT(
        {
            acquire(g_static[3]);
            __atomic_store_n(malloc_word(), self() | KOS_WORD_WAITERS, __ATOMIC_RELEASE);
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
