// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// One source, two images: main (libcmix) and its peer (libcmix_peer, LIBCMIX_PEER), each linking
// its own newlib profile. Each runs the same libc arms in its own task, printf, malloc, a FILE
// lock, a spawned thread's own libc state and exit, and names the profile its printf answers to,
// in main and in that thread: nano prints nothing for a float conversion its image did not ask
// for, full prints it. main runs the peer once, reads the status
// its exit() ended it with, then exits itself. main's exit() runs its atexit handler, the
// peer's its static object's destructor.

#include <kickos/apps/peer_run.h>
#include <kickos/kos.h>
#include <kickos/sys.h>

#include <errno.h>
#include <newlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace
{
#ifdef LIBCMIX_PEER
    char const ROLE[] = "peer";
#else
    char const ROLE[] = "main";
#endif
    constexpr int PEER_CODE = 5;

    int g_failed = 0;
    char g_stdout_buffer[96];

    void verdict(bool ok, char const* arm)
    {
        if (ok)
        {
            printf("libcmix %s: ok %s\n", ROLE, arm);
        }
        else
        {
            printf("libcmix %s: FAIL %s\n", ROLE, arm);
            g_failed = 1;
        }
    }

    void printf_arm()
    {
        char text[16];
        int const n = snprintf(text, sizeof(text), "%d:%s", 42, "ok");
        verdict(n == 5 and strcmp(text, "42:ok") == 0, "printf");
        char real[16];
        (void)snprintf(real, sizeof(real), "%.1f", 1.5);
        char const* seen = "full";
        if (real[0] == '\0')
        {
            seen = "nano";
        }
#if defined(_NANO_FORMATTED_IO)
        char const* built = "nano";
#else
        char const* built = "full";
#endif
        printf("libcmix %s: profile %s\n", ROLE, seen);
        verdict(strcmp(seen, built) == 0, "profile");
    }

    void malloc_arm()
    {
        constexpr int BLOCKS = 8;
        unsigned char* block[BLOCKS] = {};
        bool ok = true;
        for (int i = 0; i < BLOCKS; i++)
        {
            size_t const size = 8u + 8u * static_cast<size_t>(i);
            block[i] = static_cast<unsigned char*>(malloc(size));
            if (block[i] == nullptr)
            {
                printf("libcmix %s: malloc(%u) refused\n", ROLE, static_cast<unsigned>(size));
                ok = false;
                continue;
            }
            if ((reinterpret_cast<uintptr_t>(block[i]) & 7u) != 0)
            {
                printf("libcmix %s: malloc(%u) misaligned\n", ROLE, static_cast<unsigned>(size));
                ok = false;
            }
            memset(block[i], 0x40 + i, size);
        }
        for (int i = 0; i < BLOCKS; i++)
        {
            if (block[i] == nullptr)
            {
                continue;
            }
            size_t const size = 8u + 8u * static_cast<size_t>(i);
            for (size_t k = 0; k < size; k++)
            {
                if (block[i][k] != static_cast<unsigned char>(0x40 + i))
                {
                    ok = false;
                }
            }
        }
        unsigned char* grown = static_cast<unsigned char*>(realloc(block[0], 128));
        if (grown == nullptr or grown[0] != 0x40 or grown[7] != 0x40)
        {
            ok = false;
        }
        else
        {
            block[0] = grown;
        }
        for (int i = 0; i < BLOCKS; i++)
        {
            free(block[i]);
        }
        verdict(ok, "malloc");
    }

    int g_try_held = 0;
    int g_try_free = -1;

    void try_held(void*)
    {
        g_try_held = ftrylockfile(stdout);
        if (g_try_held == 0)
        {
            funlockfile(stdout);
        }
    }

    void try_free(void*)
    {
        g_try_free = ftrylockfile(stdout);
        if (g_try_free == 0)
        {
            funlockfile(stdout);
        }
    }

    // stdout's lock held here refuses another thread's try, and once released admits it.
    void file_lock_arm()
    {
        flockfile(stdout);
        auto holder = kos::thread::create(try_held, nullptr, "tryheld", 10);
        bool const ran_held = holder.valid() and holder.join() == 0;
        funlockfile(stdout);
        auto other = kos::thread::create(try_free, nullptr, "tryfree", 10);
        bool const ran_free = other.valid() and other.join() == 0;
        verdict(ran_held and ran_free and g_try_held != 0 and g_try_free == 0, "file_lock");
    }

    bool g_thread_ok = false;

    // From a spawned thread's own libc state: its formatter answers as this image's profile, its
    // heap block is whole, and the errno it sets is its own.
    void thread_libc(void*)
    {
        char real[16];
        (void)snprintf(real, sizeof(real), "%.1f", 1.5);
        char const* seen = "full";
        if (real[0] == '\0')
        {
            seen = "nano";
        }
        printf("libcmix %s: thread profile %s\n", ROLE, seen);
        bool ok = true;
        unsigned char* const block = static_cast<unsigned char*>(malloc(48));
        if (block == nullptr)
        {
            ok = false;
        }
        else
        {
            memset(block, 0x5A, 48);
            for (size_t k = 0; k < 48; k++)
            {
                if (block[k] != 0x5A)
                {
                    ok = false;
                }
            }
            free(block);
        }
        errno = EDOM;
        g_thread_ok = ok and errno == EDOM;
    }

    void thread_arm()
    {
        errno = 0;
        auto t = kos::thread::create(thread_libc, nullptr, "libcthr", 10);
        bool const ran = t.valid() and t.join() == 0;
        verdict(ran and g_thread_ok and errno == 0, "thread");
    }

#ifdef LIBCMIX_PEER
    // An atexit call anywhere in the peer would link the libc's exit registration on its own,
    // and the destructor would run even where the libc drops static destructors.
    struct StaticWitness
    {
        ~StaticWitness()
        {
            printf("libcmix %s: static destructor\n", ROLE);
        }
    };

    StaticWitness g_static_witness;
#else
    void at_exit()
    {
        printf("libcmix %s: atexit\n", ROLE);
    }
#endif

#ifndef LIBCMIX_PEER
    // The peer's own arms, then the status its exit() ended its task with.
    void peer_arm()
    {
        kos_program_t prog = KOS_CAP_NONE;
        kos_task_t task = KOS_TASK_NONE;
        char const* const argv[] = {"peer", nullptr};
        kos_task_params params = {};
        params.argv = argv;
        int status = -1000;
        if (kos_program_find("libcmix_peer", &prog) == 0
            and kickos::apps::create_peer(prog, params, &task) == 0)
        {
            while (kickos::apps::wait_dead(task, &status, 1000u) == -KOS_ETIMEDOUT)
            {
            }
            (void)kos_task_slay(task, 0);
        }
        if (prog != KOS_CAP_NONE)
        {
            (void)kos_handle_close(prog);
        }
        printf("libcmix main: peer status %d\n", status);
        verdict(status == PEER_CODE, "peer_exit");
    }
#endif
}

int main(int, char**)
{
    // Line buffered from storage of its own, so no arm's heap is the stream's.
    setvbuf(stdout, g_stdout_buffer, _IOLBF, sizeof(g_stdout_buffer));
    printf("libcmix %s: start\n", ROLE);
    printf_arm();
    malloc_arm();
    file_lock_arm();
    thread_arm();
#ifndef LIBCMIX_PEER
    peer_arm();
    if (atexit(at_exit) != 0)
    {
        g_failed = 1;
    }
#endif
    char const* end = "done";
    if (g_failed != 0)
    {
        end = "failed";
    }
    printf("libcmix %s: %s\n", ROLE, end);
#ifdef LIBCMIX_PEER
    exit(PEER_CODE + g_failed);
#else
    exit(g_failed);
#endif
}
