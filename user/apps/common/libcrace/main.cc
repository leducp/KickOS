// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Four threads, two in main's task and two in a task of their own, print through stdio and
// allocate from the heap at once, each preempted every few tens of microseconds. Every line a
// worker prints is one known text, and every block it allocates holds its pattern until freed.
// On a region board the two tasks share one heap and one stdout; on a translating one each task
// has its own, shared by its two threads. Main's task's workers start on a heap main has
// exhausted, and a round the heap cannot serve waits for it. Before they start: a thread killed
// while it waits for the malloc lock ends alone, and so does one that holds the libc lock and
// hands it over; a malloc lock taken from a holder that died holding it ends the taker's task;
// and a printf completes while another thread of main's task holds the malloc lock.

#include <kickos/config/priorities.h>
#include <kickos/kos.h>
#include <kickos/sys.h>

#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_NEWLIB_VERSION)
// newlib ships this header with no C++ linkage of its own.
extern "C"
{
#include <envlock.h>
}
#endif

namespace
{
    constexpr int WORKERS = 4;
    constexpr int ROUNDS = 300;
    constexpr int PAYLOAD = 40;
    constexpr int BLOCKS = 3;
    constexpr uint32_t QUANTUM_NS = 20000;
    constexpr uint8_t WORKER_PRIO = 10;
    constexpr kos_cap_t DONE = kos::child_cap(0);
    constexpr kos_cap_t STARVED = kos::child_cap(1);
    constexpr kos_cap_t HELD = kos::child_cap(0);
    constexpr kos_cap_t RELEASE = kos::child_cap(1);
    constexpr kos_cap_t ENTERING = kos::child_cap(0);
    constexpr kos_cap_t PRINTED = kos::child_cap(0);
    constexpr uint32_t PRINT_WAIT_US = 3000000;
    constexpr uint8_t VICTIM_PRIO = KICKOS_PRIO_MAX;

    char letter(int w, int i)
    {
        return static_cast<char>('a' + (w * 7 + i) % 26);
    }

    bool whole(char const* block, size_t len, char c)
    {
        for (size_t k = 0; k < len; k++)
        {
            if (block[k] != c)
            {
                return false;
            }
        }
        return true;
    }

    // The sim's host libc has no lock to hold.
    void malloc_holder(void*)
    {
#if defined(_NEWLIB_VERSION)
        __malloc_lock(_REENT);
#endif
        kos_sem_post(HELD);
        kos_sem_wait(RELEASE, KOS_TIMEOUT_NONE);
#if defined(_NEWLIB_VERSION)
        __malloc_unlock(_REENT);
#endif
    }

    void malloc_victim(void*)
    {
        kos_sem_post(ENTERING);
        void* volatile block = malloc(32);
        free(block);
    }

    // The env lock is the libc lock.
    void libc_holder_victim(void*)
    {
#if defined(_NEWLIB_VERSION)
        __env_lock(_REENT);
#endif
        kos_sem_post(ENTERING);
        void* volatile block = malloc(32);
        free(block);
#if defined(_NEWLIB_VERSION)
        __env_unlock(_REENT);
#endif
    }

#if KICKOS_FAULT_ISOLATION and KICKOS_HAVE_ASPACE
    void anchor(void*)
    {
        while (true)
        {
            kos::sleep_ns(1000000000ull);
        }
    }
#endif

    void printer(void*)
    {
        printf("libcrace: printf beside the malloc lock %d\n", 1);
        kos_sem_post(PRINTED);
    }

    // Holds the malloc lock in a thread of main's task and returns with `release` to end it.
    bool hold_malloc(kos::thread::Handle* holder, kos_cap_t* release)
    {
        kos_cap_t held = KOS_CAP_NONE;
        if (kos_sem_create(0, &held) != 0)
        {
            return false;
        }
        if (kos_sem_create(0, release) != 0)
        {
            kos_handle_close(held);
            return false;
        }
        kos_cap_grant const caps[] = {{held, KOS_CAP_SIGNAL}, {*release, KOS_CAP_WAIT}};
        *holder = kos::thread::create_caps(malloc_holder, nullptr, "mholder", WORKER_PRIO, caps, 2);
        bool holding = holder->valid();
        if (holding and kos_sem_wait(held, KOS_TIMEOUT_NONE) != 0)
        {
            holding = false;
            kos_sem_post(*release);
            holder->join();
        }
        kos_handle_close(held);
        if (not holding)
        {
            kos_handle_close(*release);
        }
        return holding;
    }

    void end_hold(kos::thread::Handle const& holder, kos_cap_t release)
    {
        kos_sem_post(release);
        holder.join();
        kos_handle_close(release);
    }

    // Pins main to the core it runs on. A one-core kernel has nothing to pin; any other refusal
    // refuses the arm, which would otherwise kill a victim that had not parked yet.
    bool pin_main(uint32_t* core_mask)
    {
        int const core = kos_core_current();
        int const rc = kos_thread_set_affinity(kos_thread_self(), 1u << core);
        *core_mask = 0;
        if (rc == -KOS_ENOSYS)
        {
            return true;
        }
        if (rc != 0)
        {
            return false;
        }
        *core_mask = 1u << core;
        return true;
    }

    void unpin_main(uint32_t core_mask)
    {
        if (core_mask != 0)
        {
            kos_thread_set_affinity(kos_thread_self(), 0);
        }
    }

    // The victim outranks main on main's core, so main, woken by its post, runs again only once
    // it has parked. Invalid when the spawn or the post fails.
    kos::thread::Handle spawn_parked(void (*entry)(void*), char const* name, kos_task_t task,
                                     uint32_t core_mask, kos_cap_t entering)
    {
        kos_cap_grant const caps[] = {{entering, KOS_CAP_SIGNAL}};
        auto victim = kos::thread::create_caps(entry, nullptr, name, VICTIM_PRIO, caps, 1,
                                               KOS_POLICY_FIFO, 0, nullptr, 0, 0, nullptr, task,
                                               nullptr, 0, core_mask);
        if (victim.valid() and kos_sem_wait(entering, KOS_TIMEOUT_NONE) != 0)
        {
            victim.kill();
            victim.join();
            return kos::thread::Handle();
        }
        return victim;
    }

    // `entry` parks inside malloc, behind a holder of the malloc lock, and is killed there.
    bool kill_inside_malloc(void (*entry)(void*), char const* name)
    {
        kos::thread::Handle holder;
        kos_cap_t release = KOS_CAP_NONE;
        uint32_t core_mask = 0;
        kos_cap_t entering = KOS_CAP_NONE;
        if (not pin_main(&core_mask))
        {
            return false;
        }
        bool ended = false;
        if (kos_sem_create(0, &entering) == 0 and hold_malloc(&holder, &release))
        {
            auto victim = spawn_parked(entry, name, KOS_TASK_NONE, core_mask, entering);
            ended = victim.valid() and victim.kill() == 0 and victim.join() == 0;
            end_hold(holder, release);
        }
        kos_handle_close(entering);
        unpin_main(core_mask);
        return ended;
    }

    // The killed holder's death reaches the next take of the libc lock, which holds it; the host
    // libc of the sim takes no lock of ours.
    bool libc_lock_handed_over()
    {
        int expected = 0;
#if defined(_NEWLIB_VERSION)
        expected = -KOS_EOWNERDEAD;
#endif
        if (kos_mutex_lock(KOS_CAP_LIBC_LOCK, 0) != expected)
        {
            return false;
        }
        return kos_mutex_unlock(KOS_CAP_LIBC_LOCK) == 0;
    }

#if KICKOS_FAULT_ISOLATION and KICKOS_HAVE_ASPACE
    // In a task of its own, whose entry only anchors it: a holder of the malloc lock is killed,
    // with a taker parked behind it or, `waiting` false, one that comes only after the death, so
    // the taker takes the lock from a dead owner.
    bool dead_malloc_holder_ends_the_takers_task(bool waiting, bool* ended)
    {
        *ended = false;
        uint32_t core_mask = 0;
        if (not pin_main(&core_mask))
        {
            return false;
        }
        kos_task_t task = KOS_TASK_NONE;
        kos_cap_t held = KOS_CAP_NONE;
        kos_cap_t release = KOS_CAP_NONE;
        kos_cap_t entering = KOS_CAP_NONE;
        bool ran = kos_task_create(nullptr, 0, 0, &task) == 0
                   and kos_task_sched_grant(task, VICTIM_PRIO, 0) == 0
                   and kos_sem_create(0, &held) == 0 and kos_sem_create(0, &release) == 0
                   and kos_sem_create(0, &entering) == 0;
        kos::thread::Handle base;
        kos::thread::Handle holder;
        if (ran)
        {
            base = kos::thread::create_caps(anchor, nullptr, "manchor", WORKER_PRIO, nullptr, 0,
                                            KOS_POLICY_FIFO, 0, nullptr, 0, 0, nullptr, task);
            kos_cap_grant const caps[] = {{held, KOS_CAP_SIGNAL}, {release, KOS_CAP_WAIT}};
            holder = kos::thread::create_caps(malloc_holder, nullptr, "mholder", WORKER_PRIO,
                                              caps, 2, KOS_POLICY_FIFO, 0, nullptr, 0, 0,
                                              nullptr, task);
            ran = base.valid() and holder.valid() and kos_sem_wait(held, KOS_TIMEOUT_NONE) == 0;
        }
        if (ran and waiting)
        {
            auto taker = spawn_parked(malloc_victim, "mwaiter", task, core_mask, entering);
            ran = taker.valid() and holder.kill() == 0 and holder.join() == 0
                  and taker.join() == 0;
        }
        else if (ran)
        {
            ran = holder.kill() == 0 and holder.join() == 0;
            auto taker = spawn_parked(malloc_victim, "mlater", task, core_mask, entering);
            ran = ran and taker.valid() and taker.join() == 0;
        }
        if (ran)
        {
            int status = 0;
            *ended = kos_task_exit_status(task, &status) == 0 and status == KOS_EXIT_FAULT;
        }
        if (task != KOS_TASK_NONE)
        {
            kos_task_kill(task);
        }
        if (base.valid())
        {
            base.join();
        }
        if (holder.valid())
        {
            holder.join();
        }
        kos_handle_close(held);
        kos_handle_close(release);
        kos_handle_close(entering);
        unpin_main(core_mask);
        return ran;
    }
#endif

    // The printf allocates nothing: main's first printf gave stdout its buffer.
    bool print_beside_malloc()
    {
        kos::thread::Handle holder;
        kos_cap_t release = KOS_CAP_NONE;
        kos_cap_t printed = KOS_CAP_NONE;
        if (kos_sem_create(0, &printed) != 0)
        {
            return false;
        }
        if (not hold_malloc(&holder, &release))
        {
            kos_handle_close(printed);
            return false;
        }
        kos_cap_grant const caps[] = {{printed, KOS_CAP_SIGNAL}};
        auto print = kos::thread::create_caps(printer, nullptr, "mprint", WORKER_PRIO, caps, 1);
        bool const started = print.valid();
        if (started and kos_sem_wait(printed, PRINT_WAIT_US) == 0)
        {
            kos::print("libcrace: a printf completed while malloc was held\n");
        }
        else
        {
            kos::print("libcrace: a printf waited behind the malloc lock\n");
        }
        end_hold(holder, release);
        if (started)
        {
            print.join();
        }
        kos_handle_close(printed);
        return started;
    }

    struct Ballast
    {
        Ballast* next;
    };

    // The sim's host heap has no end to reach.
    Ballast* exhaust_heap()
    {
        Ballast* head = nullptr;
#if defined(_NEWLIB_VERSION)
        size_t size = 4096;
        while (size >= sizeof(Ballast))
        {
            auto* const b = static_cast<Ballast*>(malloc(size));
            if (b == nullptr)
            {
                size /= 2;
                continue;
            }
            b->next = head;
            head = b;
        }
#endif
        return head;
    }

    void release_heap(Ballast* head)
    {
        while (head != nullptr)
        {
            Ballast* const next = head->next;
            free(head);
            head = next;
        }
    }

    // A heap the workers' blocks exhaust answers NULL, which is not a torn block: the round
    // gives back what it took and asks again.
    bool take_blocks(char* (&blocks)[BLOCKS], size_t const (&sizes)[BLOCKS], char c)
    {
        for (int b = 0; b < BLOCKS; b++)
        {
            blocks[b] = static_cast<char*>(malloc(sizes[b]));
            if (blocks[b] == nullptr)
            {
                for (int k = 0; k < b; k++)
                {
                    free(blocks[k]);
                    blocks[k] = nullptr;
                }
                return false;
            }
            memset(blocks[b], c, sizes[b]);
        }
        return true;
    }

    void worker(void* arg)
    {
        int const w = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
        int torn = 0;
        bool starved = false;
        for (int i = 0; i < ROUNDS; i++)
        {
            char const c = letter(w, i);
            size_t const sizes[BLOCKS] = {16u + static_cast<size_t>((i * 13) % 96), 64u, 24u};
            char* blocks[BLOCKS] = {};
            while (not take_blocks(blocks, sizes, c))
            {
                if (not starved)
                {
                    starved = true;
                    kos_sem_post(STARVED);
                }
                kos::sleep_ns(QUANTUM_NS);
            }
            char payload[PAYLOAD + 1];
            memset(payload, c, PAYLOAD);
            payload[PAYLOAD] = '\0';
            printf("libcrace w%d i%d %s\n", w, i, payload);
            for (int b = 0; b < BLOCKS; b++)
            {
                if (not whole(blocks[b], sizes[b], c))
                {
                    torn++;
                }
                free(blocks[b]);
            }
        }
        printf("libcrace w%d done torn=%d\n", w, torn);
        kos_sem_post(DONE);
        // The other task's entry returning would end its sibling with it.
        while (true)
        {
            kos::sleep_ns(1000000000ull);
        }
    }
}

int main(int, char**)
{
    printf("libcrace: %d workers x %d rounds\n", WORKERS, ROUNDS);
    if (not kill_inside_malloc(malloc_victim, "mvictim"))
    {
        kos::print("libcrace: the malloc kill arm was refused\n");
        return 1;
    }
    kos::print("libcrace: a thread killed waiting for malloc ended alone\n");
    if (not kill_inside_malloc(libc_holder_victim, "mlibc") or not libc_lock_handed_over())
    {
        kos::print("libcrace: a thread killed holding the libc lock kept it\n");
        return 1;
    }
    kos::print("libcrace: a thread killed holding the libc lock handed it over\n");
#if KICKOS_FAULT_ISOLATION and KICKOS_HAVE_ASPACE
    bool dead_ended = false;
    if (not dead_malloc_holder_ends_the_takers_task(true, &dead_ended))
    {
        kos::print("libcrace: the dead malloc holder arm was refused\n");
        return 1;
    }
    if (dead_ended)
    {
        kos::print("libcrace: a malloc lock taken from a dead holder ended its task\n");
    }
    else
    {
        kos::print("libcrace: a malloc lock taken from a dead holder did not end its task\n");
    }
    if (not dead_malloc_holder_ends_the_takers_task(false, &dead_ended))
    {
        kos::print("libcrace: the unwatched dead malloc holder arm was refused\n");
        return 1;
    }
    if (dead_ended)
    {
        kos::print("libcrace: a malloc lock its holder died in unwatched ended the next taker's "
                   "task\n");
    }
    else
    {
        kos::print("libcrace: a malloc lock its holder died in unwatched let the next taker "
                   "run\n");
    }
#else
    kos::print("libcrace: no task-private heap with a contained fault, so the dead malloc holder "
               "arms did not run\n");
#endif
    if (not print_beside_malloc())
    {
        kos::print("libcrace: the printf beside malloc arm was refused\n");
        return 1;
    }
    kos_cap_t done = KOS_CAP_NONE;
    kos_cap_t starved = KOS_CAP_NONE;
    kos_task_t other = KOS_TASK_NONE;
    if (kos_sem_create(0, &done) != 0 or kos_sem_create(0, &starved) != 0
        or kos_task_create(nullptr, 0, 0, &other) != 0)
    {
        kos::print("libcrace: setup refused\n");
        return 1;
    }
    kos_cap_grant const caps[] = {{done, KOS_CAP_SIGNAL}, {starved, KOS_CAP_SIGNAL}};
    Ballast* const ballast = exhaust_heap();
    int started = 0;
    for (int w = 0; w < WORKERS; w++)
    {
        kos_task_t task = KOS_TASK_NONE;
        if (w >= WORKERS / 2)
        {
            task = other;
        }
        auto h = kos::thread::create_caps(worker, reinterpret_cast<void*>(static_cast<uintptr_t>(w)),
                                          "racer", WORKER_PRIO, caps, 2, KOS_POLICY_RR, QUANTUM_NS,
                                          nullptr, 0, 0, nullptr, task);
        if (h.valid())
        {
            started++;
        }
    }
    if (ballast != nullptr and started == WORKERS)
    {
        kos_sem_wait(starved, KOS_TIMEOUT_NONE);
    }
    release_heap(ballast);
    for (int w = 0; w < started; w++)
    {
        kos_sem_wait(done, KOS_TIMEOUT_NONE);
    }
    if (started != WORKERS)
    {
        kos::print("libcrace: spawn refused\n");
        return 1;
    }
    kos::print("libcrace: all workers done\n");
    return 0;
}
