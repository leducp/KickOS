// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Four threads, two in main's task and two in a task of their own, print through stdio and
// allocate from the heap at once, each preempted every few tens of microseconds. Every line a
// worker prints is one known text, and every block it allocates holds its pattern until freed.
// On a region board the two tasks share one heap and one stdout; on a translating one each task
// has its own, shared by its two threads.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
    constexpr int WORKERS = 4;
    constexpr int ROUNDS = 300;
    constexpr int PAYLOAD = 40;
    constexpr uint32_t QUANTUM_NS = 20000;
    constexpr uint8_t WORKER_PRIO = 10;
    constexpr kos_cap_t DONE = KOS_SPAWN_DELEGATED_CAP0;

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

    void worker(void* arg)
    {
        int const w = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
        int torn = 0;
        for (int i = 0; i < ROUNDS; i++)
        {
            char const c = letter(w, i);
            size_t const sizes[] = {16u + static_cast<size_t>((i * 13) % 96), 64u, 24u};
            char* blocks[3] = {};
            for (int b = 0; b < 3; b++)
            {
                blocks[b] = static_cast<char*>(malloc(sizes[b]));
                if (blocks[b] != nullptr)
                {
                    memset(blocks[b], c, sizes[b]);
                }
            }
            char payload[PAYLOAD + 1];
            memset(payload, c, PAYLOAD);
            payload[PAYLOAD] = '\0';
            printf("libcrace w%d i%d %s\n", w, i, payload);
            for (int b = 0; b < 3; b++)
            {
                if (blocks[b] == nullptr or not whole(blocks[b], sizes[b], c))
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
    kos_cap_t done = KOS_CAP_NONE;
    kos_task_t other = KOS_TASK_NONE;
    if (kos_sem_create(0, &done) != 0 or kos_task_create(nullptr, 0, 0, &other) != 0)
    {
        kos::print("libcrace: setup refused\n");
        return 1;
    }
    kos_cap_grant const caps[] = {{done, KOS_CAP_SIGNAL}};
    int started = 0;
    for (int w = 0; w < WORKERS; w++)
    {
        kos_task_t task = KOS_TASK_NONE;
        if (w >= WORKERS / 2)
        {
            task = other;
        }
        auto h = kos::thread::create_caps(worker, reinterpret_cast<void*>(static_cast<uintptr_t>(w)),
                                          "racer", WORKER_PRIO, caps, 1, KOS_POLICY_RR, QUANTUM_NS,
                                          nullptr, 0, 0, nullptr, task);
        if (h.valid())
        {
            started++;
        }
    }
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
