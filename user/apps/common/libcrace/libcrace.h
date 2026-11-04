// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What libcrace's main and its peer share: the gates between a task's driver thread and its
// threads, the racing worker, and the helpers that park a victim on a core.

#ifndef KICKOS_USER_APPS_COMMON_LIBCRACE_LIBCRACE_H
#define KICKOS_USER_APPS_COMMON_LIBCRACE_LIBCRACE_H

#include <kickos/board_config.h>
#include <kickos/config/priorities.h>
#include <kickos/kos.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys.h>
#include <kickos/sys/user_cas.h>

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

namespace libcrace
{
    constexpr int WORKERS = 4;
    constexpr int ROUNDS = 300;
    constexpr int PAYLOAD = 40;
    constexpr int BLOCKS = 3;
    constexpr uint32_t QUANTUM_NS = LIBCRACE_QUANTUM_NS;
    constexpr uint8_t WORKER_PRIO = 10;
    constexpr uint32_t PRINT_WAIT_US = 3000000;
    constexpr uint8_t VICTIM_PRIO = KICKOS_PRIO_MAX;

    // The semaphores between a task's driver thread and the threads it spawns.
    struct Gates
    {
        kos_sem_t done;
        kos_sem_t starved;
        kos_sem_t held;
        kos_sem_t release;
        kos_sem_t entering;
        kos_sem_t printed;
        kos_sem_t go;
        kos_sem_t begun;
    };
    inline Gates g_static_gates = {};
    inline Gates* g_gates = &g_static_gates;

    inline Gates* gates()
    {
        return g_gates;
    }

    // The racers of each task, half in main's and half in the peer's. How many of this task's have
    // run their first block and ended.
    constexpr uint32_t TASK_RACERS = WORKERS / 2;
    inline uint32_t g_began = 0;
    inline uint32_t g_ended = 0;

    // Each racer holds, at these slots, the notification its task's racers wait on and the one the
    // other task's wait on. A task's last racer to run its first block signals the other task and
    // waits for it, so no racer of either task runs a second block before every racer of both has
    // run its first.
    constexpr kos_cap_t MEET_OWN = KOS_SPAWN_DELEGATED_CAP0;
    constexpr kos_cap_t MEET_OTHER = KOS_SPAWN_DELEGATED_CAP0 + 1;

    // Every gate and count at 0, before any thread of the run exists.
    inline void gates_seat()
    {
        *g_gates = Gates{};
        g_began = 0;
        g_ended = 0;
    }

    // One more on `count`, answering the new count. Inline: the smallest boards have no flash for
    // the call.
    __attribute__((always_inline)) inline uint32_t count_one(uint32_t* count)
    {
        while (true)
        {
            uint32_t const was = __atomic_load_n(count, __ATOMIC_ACQUIRE);
            if (kickos_user_cas(count, was, was + 1u) == was)
            {
                return was + 1u;
            }
        }
    }

    inline uint32_t count_of(uint32_t const* count)
    {
        return __atomic_load_n(count, __ATOMIC_ACQUIRE);
    }

    // The last racer of the task to run its first block releases the others, so every racer's
    // first block comes before any racer's second, whatever the driver's priority and the core
    // count. Where two tasks race, it first meets the other task's.
    inline void await_begun(uint32_t began)
    {
        if (began < TASK_RACERS)
        {
            kos_sem_wait(&g_gates->begun, KOS_TIMEOUT_NONE);
            return;
        }
        uint32_t bits = 0;
        (void)kos_notify(MEET_OTHER);
        if (kos_notify_bind(MEET_OWN) == 0)
        {
            (void)kos_notify_wait(MEET_OWN, 1u, KOS_TIMEOUT_NONE, &bits);
            (void)kos_notify_unbind(MEET_OWN);
        }
        for (uint32_t k = 1; k < TASK_RACERS; k++)
        {
            kos_sem_post(&g_gates->begun);
        }
    }

    // `entry` in the caller's task.
    inline kos::thread::Handle spawn_in(void (*entry)(void*), void* arg, char const* name,
                                        uint8_t prio, uint32_t core_mask,
                                        uint8_t policy = KOS_POLICY_FIFO, uint32_t quantum_ns = 0)
    {
        return kos::thread::create(entry, arg, name, prio, policy, quantum_ns, nullptr, 0,
                                   nullptr, 0, nullptr, 0, 0, nullptr, core_mask);
    }

    // A racer of this task, holding `own` at MEET_OWN and `other` at MEET_OTHER.
    inline kos::thread::Handle spawn_racer(void (*entry)(void*), int w, kos_cap_t own,
                                           kos_cap_t other)
    {
        void* const arg = reinterpret_cast<void*>(static_cast<uintptr_t>(w));
        kos_cap_grant const meet[2] = {{own, KOS_CAP_WAIT}, {other, KOS_CAP_SIGNAL}};
        return kos::thread::create(entry, arg, "racer", WORKER_PRIO, KOS_POLICY_RR, QUANTUM_NS,
                                   nullptr, 0, nullptr, 0, meet, 2);
    }

    inline char letter(int w, int i)
    {
        return static_cast<char>('a' + (w * 7 + i) % 26);
    }

    inline bool whole(char const* block, size_t len, char c)
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

    inline void malloc_victim(void*)
    {
        kos_sem_post(&gates()->entering);
        void* volatile block = malloc(32);
        free(block);
    }

#if KICKOS_FAULT_ISOLATION and KICKOS_HAVE_ASPACE
    // Never unlocks: a kill must find it at the re-wait, holding the malloc lock.
    inline void dead_malloc_holder(void*)
    {
#if defined(_NEWLIB_VERSION)
        __malloc_lock(_REENT);
#endif
        Gates* const g = gates();
        kos_sem_post(&g->held);
        while (true)
        {
            (void)kos_sem_wait(&g->release, KOS_TIMEOUT_NONE);
        }
    }
#endif

#if defined(_NEWLIB_VERSION)
    inline uint32_t g_release_go = 0;

    // Takes the malloc lock with no entry, then spins with none, so a kill finds it running.
    inline void running_holder(void*)
    {
        __malloc_lock(_REENT);
        kos_sem_post(&gates()->held);
        while (__atomic_load_n(&g_release_go, __ATOMIC_ACQUIRE) == 0)
        {
        }
        __malloc_unlock(_REENT);
        while (true)
        {
            kos_yield();
        }
    }

    inline void release_setter(void*)
    {
        (void)kos_sem_wait(&gates()->go, KOS_TIMEOUT_NONE);
        __atomic_store_n(&g_release_go, 1u, __ATOMIC_RELEASE);
    }

#endif

    // Pins the caller to the core it runs on. A one-core kernel has nothing to pin; any other refusal
    // refuses the arm, which would otherwise kill a victim that had not parked yet.
    inline bool pin_main(uint32_t* core_mask)
    {
        int const core = kos_core_current();
        int const rc = kos_thread_affinity(kos_thread_self(), 1u << core);
        *core_mask = 0;
        if (rc == -KOS_ENOSYS)
        {
            return true;
        }
        if (rc < 0)
        {
            return false;
        }
        *core_mask = 1u << core;
        return true;
    }

    inline void unpin_main(uint32_t core_mask)
    {
        if (core_mask != 0)
        {
            kos_thread_affinity(kos_thread_self(), 0);
        }
    }

    // The victim outranks main on main's core, so main, woken by its post, runs again only once
    // it has parked. Invalid when the spawn or the post fails.
    inline kos::thread::Handle spawn_parked(void (*entry)(void*), char const* name,
                                            uint32_t core_mask)
    {
        (void)kos_sem_init(&g_gates->entering, 0);
        auto victim = spawn_in(entry, nullptr, name, VICTIM_PRIO, core_mask);
        if (victim.valid() and kos_sem_wait(&g_gates->entering, KOS_TIMEOUT_NONE) != 0)
        {
            victim.cancel(KOS_CANCEL_KILL, 0);
            victim.join();
            return kos::thread::Handle();
        }
        return victim;
    }

    // A heap the workers' blocks exhaust answers NULL, which is not a torn block: the round
    // gives back what it took and asks again.
    inline bool take_blocks(char* (&blocks)[BLOCKS], size_t const (&sizes)[BLOCKS], char c)
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

    inline void worker(void* arg)
    {
        int const w = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
        Gates* const g = gates();
        int torn = 0;
        bool starved = false;
        // Set when this racer's first block came before any racer of its task ended and its last
        // block after every one of them began: their runs overlap.
        bool first_beside = false;
        bool last_beside = false;
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
                    kos_sem_post(&g->starved);
                }
                kos::sleep_ns(QUANTUM_NS);
            }
            if (i == 0)
            {
                // One write, so it reaches the wire whole where stdout is unbuffered.
                char began[24];
                ksnprintf(began, sizeof(began), "libcrace w%d began\n", w);
                kos::print(began);
                uint32_t const n = count_one(&g_began);
                first_beside = count_of(&g_ended) == 0;
                await_begun(n);
            }
            if (i == ROUNDS - 1)
            {
                last_beside = count_of(&g_began) == TASK_RACERS;
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
        (void)count_one(&g_ended);
        // One write: through stdio an unbuffered stdout lets the other task's bytes into it. Not
        // snprintf: the C library's string formatter would be a second printf engine per image.
        char report[56];
        ksnprintf(report, sizeof(report), "libcrace w%d done torn=%d raced=%d\n", w, torn,
                  static_cast<int>(first_beside and last_beside));
        kos::print(report);
        kos_sem_post(&g->done);
    }
}

#endif
