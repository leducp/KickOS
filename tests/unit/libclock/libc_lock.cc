// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// newlib's locks, each a mutex word and a depth in its own struct __lock, a named static one or
// one a FILE holds, taken through the real user/src/mutex.cc over a host compare-and-swap. The
// kernel here answers the two contended entries as the kernel does: a dead holder's word is
// stolen with -KOS_EOWNERDEAD, a try on a word another thread holds -KOS_ETIMEDOUT, a wait parks
// until an unlock hands the word over, a non-holder unlocking -KOS_EPERM. Each host thread is a
// KickOS thread of its own.

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <deque>
#include <iterator>
#include <map>
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
    std::map<uint32_t*, std::deque<uint32_t>> g_waiters;

    int g_locks = 0;
    int g_unlocks = 0;
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
    // OTHER_ID's holder unlocks as soon as a thread waits, handing the word to that waiter.
    std::atomic<bool> g_other_hands_over{false};
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

    uint32_t holder_of(uint32_t const* word)
    {
        return __atomic_load_n(word, __ATOMIC_ACQUIRE) & ~KOS_WORD_WAITERS;
    }

    struct Named
    {
        _LOCK_T lock;
        bool recursive;
    };

    // Two FILEs' locks, as newlib finds them: zero from the FILE's storage, never initialised.
    struct __lock g_file_a;
    struct __lock g_file_b;

    // Every static lock newlib names, with the entry point its own callers use, then the FILEs'.
    Named const g_all[] = {
        {&__lock___sfp_recursive_mutex, true},    {&__lock___atexit_recursive_mutex, true},
        {&__lock___at_quick_exit_mutex, false},   {&__lock___malloc_recursive_mutex, true},
        {&__lock___env_recursive_mutex, true},    {&__lock___tz_mutex, false},
        {&__lock___dd_hash_mutex, false},         {&__lock___arc4random_mutex, false},
        {&g_file_a, true},                        {&g_file_b, true},
    };

    Named const& sfp_lock()
    {
        return g_all[0];
    }

    Named const& malloc_lock()
    {
        return g_all[3];
    }

    Named const& tz_lock()
    {
        return g_all[5];
    }

    Named const& file_lock()
    {
        return g_all[8];
    }

    std::vector<Named> all_but_malloc()
    {
        std::vector<Named> out;
        for (Named const& n : g_all)
        {
            if (n.lock != malloc_lock().lock)
            {
                out.push_back(n);
            }
        }
        return out;
    }

    uint32_t* word_of(Named const& n)
    {
        return &n.lock->__word;
    }

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

    int try_acquire(Named const& n)
    {
        if (n.recursive)
        {
            return __retarget_lock_try_acquire_recursive(n.lock);
        }
        return __retarget_lock_try_acquire(n.lock);
    }

    struct LibcLock : ::testing::Test
    {
        void SetUp() override
        {
            g_lock_answer = 0;
            g_unlock_answer = 0;
            g_cancels_before_grant = 0;
            g_other_hands_over = false;
            g_lock_calls = 0;
            g_cas_calls = 0;
            for (Named const& n : g_all)
            {
                *n.lock = {};
            }
        }
        void TearDown() override
        {
            for (Named const& n : g_all)
            {
                EXPECT_EQ(holder_of(word_of(n)), 0u) << "an arm left a lock held";
            }
        }
    };

    using LibcLockDeathTest = LibcLock;

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
        std::deque<uint32_t>& waiters = g_waiters[word];
        uint32_t const me = self();
        while (true)
        {
            uint32_t const v = __atomic_load_n(word, __ATOMIC_ACQUIRE);
            uint32_t const owner = v & ~KOS_WORD_WAITERS;
            if (owner == 0 or owner == DEAD_ID)
            {
                uint32_t next = me;
                if (not waiters.empty())
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
            if (owner == OTHER_ID and g_other_hands_over.load()
                and __real_kickos_user_cas(word, v, me) == v)
            {
                bump(g_locks);
                return 0;
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
            waiters.push_back(me);
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
            std::deque<uint32_t>& waiters = g_waiters[word];
            if (holder_of(word) != self())
            {
                return -KOS_EPERM;
            }
            uint32_t next = 0;
            if (not waiters.empty())
            {
                next = waiters.front();
                waiters.pop_front();
                if (not waiters.empty())
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

    // True once `word` has a waiter parked on it.
    bool parked_on(uint32_t* word)
    {
        for (int i = 0; i < 2000; i++)
        {
            if ((__atomic_load_n(word, __ATOMIC_ACQUIRE) & KOS_WORD_WAITERS) != 0)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
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

TEST_F(LibcLock, every_newlib_lock_takes_a_word_of_its_own)
{
    for (Named const& n : g_all)
    {
        int const locks = g_locks;
        int const unlocks = g_unlocks;
        acquire(n);
        for (Named const& other : g_all)
        {
            if (other.lock == n.lock)
            {
                EXPECT_EQ(holder_of(word_of(other)), self());
            }
            else
            {
                EXPECT_EQ(holder_of(word_of(other)), 0u) << "two locks share a word";
            }
        }
        EXPECT_EQ(tally_of(g_locks), locks + 1);
        release(n);
        EXPECT_EQ(tally_of(g_unlocks), unlocks + 1);
    }
}

TEST_F(LibcLock, kos_libc_malloc_lock_names_mallocs_word)
{
    EXPECT_EQ(&kos_libc_malloc_lock()->word, word_of(malloc_lock()));
}

// A FILE's lock is made with the FILE's zeroed storage, and its release leaves that storage as
// it was made, so a FILE closed and its slot reused needs no lock made or retired.
TEST_F(LibcLock, a_file_lock_is_unheld_from_zero_storage_and_released_back_to_it)
{
    struct __lock fresh = {};
    Named const f = {&fresh, true};
    acquire(f);
    acquire(f);
    EXPECT_EQ(holder_of(&fresh.__word), self());
    EXPECT_EQ(fresh.__depth, 2u);
    release(f);
    release(f);
    EXPECT_EQ(fresh.__word, 0u);
    EXPECT_EQ(fresh.__depth, 0u);
}

TEST_F(LibcLock, a_lock_another_thread_holds_leaves_every_other_lock_free)
{
    acquire(file_lock());
    std::atomic<int> taken{0};
    std::thread other([&] {
        for (Named const& n : g_all)
        {
            if (n.lock != file_lock().lock)
            {
                acquire(n);
                release(n);
                taken = taken + 1;
            }
        }
    });
    other.join();
    EXPECT_EQ(taken.load(), static_cast<int>(std::size(g_all)) - 1);
    EXPECT_EQ(holder_of(word_of(file_lock())), self());
    release(file_lock());
}

// Each pair in both orders, newlib's opposite ones among them: a FILE then sfp and sfp then a
// FILE, and atexit's lock inside or outside another lock an exit handler takes.
TEST_F(LibcLock, any_two_locks_but_mallocs_nest_in_either_order)
{
    std::vector<Named> const others = all_but_malloc();
    for (Named const& a : others)
    {
        for (Named const& b : others)
        {
            if (a.lock == b.lock)
            {
                continue;
            }
            int const locks = g_locks;
            acquire(a);
            acquire(b);
            EXPECT_EQ(tally_of(g_locks), locks + 2) << "a nest took one word for two locks";
            EXPECT_EQ(holder_of(word_of(a)), self());
            EXPECT_EQ(holder_of(word_of(b)), self());
            release(b);
            EXPECT_EQ(holder_of(word_of(a)), self());
            release(a);
        }
    }
}

// A closer holding a FILE waits for the sfp another thread holds, and a third thread meanwhile
// takes another FILE's lock at once.
TEST_F(LibcLock, a_thread_waiting_for_a_lock_holds_up_no_other_lock)
{
    acquire(sfp_lock());
    std::atomic<bool> done{false};
    std::thread closer([&] {
        acquire(file_lock());
        acquire(sfp_lock());
        release(sfp_lock());
        release(file_lock());
        done = true;
    });
    EXPECT_TRUE(parked_on(word_of(sfp_lock()))) << "the closer never waited for sfp";
    std::atomic<int> tried{-1};
    std::thread third([&] {
        tried = try_acquire(g_all[9]);
        if (tried == 1)
        {
            release(g_all[9]);
        }
    });
    third.join();
    EXPECT_EQ(tried.load(), 1) << "another FILE's lock was held up by the waiter or by sfp";
    EXPECT_FALSE(done.load());
    release(sfp_lock());
    closer.join();
    EXPECT_TRUE(done.load());
}

// The witness at its unit: the stdio side of newlib completes while another thread holds malloc.
TEST_F(LibcLock, a_file_lock_is_taken_while_another_thread_holds_malloc)
{
    acquire(malloc_lock());
    std::atomic<bool> printed{false};
    std::thread other([&] {
        acquire(file_lock());
        acquire(sfp_lock());
        release(sfp_lock());
        release(file_lock());
        printed = true;
    });
    other.join();
    EXPECT_TRUE(printed.load());
    EXPECT_EQ(holder_of(word_of(malloc_lock())), self());
    release(malloc_lock());
}

TEST_F(LibcLock, malloc_inside_any_other_lock_takes_both_mutexes)
{
    for (Named const& n : all_but_malloc())
    {
        int const locks = g_locks;
        acquire(n);
        acquire(malloc_lock());
        EXPECT_EQ(tally_of(g_locks), locks + 2);
        EXPECT_EQ(holder_of(word_of(n)), self());
        EXPECT_EQ(holder_of(word_of(malloc_lock())), self());
        release(malloc_lock());
        EXPECT_EQ(holder_of(word_of(malloc_lock())), 0u);
        EXPECT_EQ(holder_of(word_of(n)), self());
        release(n);
    }
}

TEST_F(LibcLock, the_depth_counts_down_to_the_unlock)
{
    int const unlocks = g_unlocks;
    acquire(malloc_lock());
    acquire(malloc_lock());
    acquire(malloc_lock());
    release(malloc_lock());
    release(malloc_lock());
    EXPECT_EQ(holder_of(word_of(malloc_lock())), self());
    EXPECT_EQ(tally_of(g_unlocks), unlocks);
    release(malloc_lock());
    EXPECT_EQ(tally_of(g_unlocks), unlocks + 1);
}

TEST_F(LibcLock, another_thread_waits_for_the_owner)
{
    acquire(sfp_lock());
    std::atomic<bool> entered{false};
    std::thread other([&] {
        acquire(sfp_lock());
        entered = true;
        release(sfp_lock());
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_FALSE(entered.load()) << "a second thread recursed on the owner's depth";
    release(sfp_lock());
    other.join();
    EXPECT_TRUE(entered.load());
}

TEST_F(LibcLock, a_releaser_preempted_after_its_unlock_leaves_the_next_owner_whole)
{
    acquire(file_lock());
    std::atomic<bool> nested{false};
    std::thread next([&] {
        acquire(file_lock());
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        acquire(file_lock());
        nested = true;
        release(file_lock());
        release(file_lock());
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    g_preempt_after_unlock = true;
    release(file_lock());
    g_preempt_after_unlock = false;
    next.join();
    EXPECT_TRUE(nested.load());
}

// The re-ask enters the kernel with no compare-and-swap of its own, so a killed waiter ends at
// that entry and never takes a lock freed meanwhile with no entry at all.
TEST_F(LibcLock, a_cancelled_wait_asks_again_at_the_entry_and_takes_the_lock_once)
{
    *word_of(sfp_lock()) = OTHER_ID;
    g_cancels_before_grant = 1;
    g_other_hands_over = true;
    int const locks = g_locks;
    acquire(sfp_lock());
    g_other_hands_over = false;
    EXPECT_EQ(tally_of(g_lock_calls), 2) << "the cancelled wait did not ask exactly once more";
    EXPECT_EQ(tally_of(g_cas_calls), 1) << "the re-ask went through the compare-and-swap";
    EXPECT_EQ(tally_of(g_locks), locks + 1);
    EXPECT_EQ(holder_of(word_of(sfp_lock())), self());
    release(sfp_lock());
    EXPECT_EQ(holder_of(word_of(sfp_lock())), 0u) << "the take left a depth other than one";
}

TEST_F(LibcLock, a_try_takes_a_free_lock_and_nests_on_its_own)
{
    int const locks = g_locks;
    EXPECT_EQ(try_acquire(sfp_lock()), 1);
    EXPECT_EQ(holder_of(word_of(sfp_lock())), self());
    EXPECT_EQ(tally_of(g_locks), locks + 1);
    EXPECT_EQ(try_acquire(sfp_lock()), 1) << "the owner's try is a nest";
    EXPECT_EQ(tally_of(g_locks), locks + 1);
    EXPECT_EQ(try_acquire(tz_lock()), 1);
    EXPECT_EQ(tally_of(g_locks), locks + 2) << "a try of another lock nested on the first";
    release(tz_lock());
    release(sfp_lock());
    release(sfp_lock());
    EXPECT_EQ(holder_of(word_of(sfp_lock())), 0u);
}

TEST_F(LibcLock, a_try_on_a_lock_another_thread_holds_fails_and_holds_nothing)
{
    acquire(tz_lock());
    int const refused = g_tries_refused;
    std::atomic<int> tried{-1};
    std::thread other([&] { tried = try_acquire(tz_lock()); });
    other.join();
    EXPECT_EQ(tried.load(), 0);
    EXPECT_EQ(tally_of(g_tries_refused), refused + 1) << "the try never reached the kernel";
    EXPECT_EQ(holder_of(word_of(tz_lock())), self());
    release(tz_lock());
    std::thread after([&] {
        tried = try_acquire(tz_lock());
        if (tried == 1)
        {
            release(tz_lock());
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
                acquire(file_lock());
                acquire(malloc_lock());
                int const seen = count;
                std::this_thread::yield();
                count = seen + 1;
                release(malloc_lock());
                release(file_lock());
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
    *word_of(malloc_lock()) = OTHER_ID;
    g_lock_answer = -KOS_EINVAL;
    EXPECT_EXIT(acquire(malloc_lock()), ::testing::KilledBySignal(SIGILL), "");
    g_lock_answer = 0;
    *word_of(malloc_lock()) = 0;
}

// Every lock newlib names, a FILE's and a try's included: what a dead holder guarded is torn.
TEST_F(LibcLockDeathTest, a_dead_owner_of_any_libc_lock_ends_its_taker)
{
    auto const ended = ::testing::KilledBySignal(SIGILL);
    for (Named const& n : g_all)
    {
        *word_of(n) = DEAD_ID;
        EXPECT_EXIT(acquire(n), ended, "");
        EXPECT_EXIT((void)try_acquire(n), ended, "");
        *word_of(n) = 0;
    }
}

TEST_F(LibcLockDeathTest, any_other_lock_taken_inside_mallocs_traps)
{
    for (Named const& n : all_but_malloc())
    {
        EXPECT_EXIT(
            {
                acquire(malloc_lock());
                acquire(n);
            },
            ::testing::KilledBySignal(SIGILL), "");
        EXPECT_EXIT(
            {
                acquire(malloc_lock());
                (void)try_acquire(n);
            },
            ::testing::KilledBySignal(SIGILL), "");
    }
}

// newlib's locks are its named objects and its FILEs'.
TEST_F(LibcLockDeathTest, a_lock_to_make_or_retire_traps)
{
    _LOCK_T made = nullptr;
    EXPECT_EXIT(__retarget_lock_init(&made), ::testing::KilledBySignal(SIGILL), "");
    EXPECT_EXIT(__retarget_lock_init_recursive(&made), ::testing::KilledBySignal(SIGILL), "");
    EXPECT_EXIT(__retarget_lock_close(file_lock().lock), ::testing::KilledBySignal(SIGILL), "");
    EXPECT_EXIT(__retarget_lock_close_recursive(file_lock().lock),
                ::testing::KilledBySignal(SIGILL), "");
}

// The re-ask loop is sound only because the kernel ends a cancelled thread at its next entry.
// This kernel never grants, and stands for that end after a bound so the test cannot hang.
TEST_F(LibcLockDeathTest, a_wait_cancelled_for_good_ends_at_an_entry_and_never_traps)
{
    *word_of(sfp_lock()) = OTHER_ID;
    g_lock_answer = -KOS_ECANCELED;
    EXPECT_EXIT(acquire(sfp_lock()), ::testing::ExitedWithCode(ENDED_AT_ENTRY), "");
    g_lock_answer = 0;
    *word_of(sfp_lock()) = 0;
}

// The release enters only for a word with waiters.
TEST_F(LibcLockDeathTest, an_unlock_the_kernel_refuses_traps)
{
    EXPECT_EXIT(
        {
            acquire(malloc_lock());
            __atomic_store_n(word_of(malloc_lock()), self() | KOS_WORD_WAITERS, __ATOMIC_RELEASE);
            g_unlock_answer = -KOS_EPERM;
            release(malloc_lock());
        },
        ::testing::KilledBySignal(SIGILL), "");
}

TEST_F(LibcLockDeathTest, a_release_without_a_hold_traps)
{
    EXPECT_EXIT(release(file_lock()), ::testing::KilledBySignal(SIGILL), "");
}

TEST_F(LibcLockDeathTest, a_release_by_a_thread_that_does_not_hold_it_traps)
{
    EXPECT_EXIT(
        {
            acquire(file_lock());
            std::thread other([] { release(file_lock()); });
            other.join();
        },
        ::testing::KilledBySignal(SIGILL), "");
}
