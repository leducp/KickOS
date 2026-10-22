// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Compiles the kos_* C-facing headers it includes as C with the real flags. Rewriting it as C++,
// or including a C++-only header, leaves the C claim those headers make resting on
// check_public_headers.sh's standalone compile alone.

#include <stdbool.h>

#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

static uint64_t const BEAT_NS = 400000000ull; // 0.4 s between hits

static kos_sem_t g_ping = KOS_SEM_INIT(1); // ping serves first
static kos_sem_t g_pong = KOS_SEM_INIT(0);
static kos_sem_t g_idle = KOS_SEM_INIT(0);

static void say(char const* who, int n)
{
    char b[48];
    ksnprintf(b, sizeof(b), "  %s %d\n", who, n);
    kos_print(b);
}

static void ping(void* arg)
{
    int n = 0;
    (void)arg;
    while (true)
    {
        kos_sem_wait(&g_ping, KOS_TIMEOUT_NONE);
        kos_sleep_ns(BEAT_NS);
        say("ping", ++n);
        kos_sem_post(&g_pong);
    }
}

static void pong(void* arg)
{
    int n = 0;
    (void)arg;
    while (true)
    {
        kos_sem_wait(&g_pong, KOS_TIMEOUT_NONE);
        kos_sleep_ns(BEAT_NS);
        say("pong", ++n);
        kos_sem_post(&g_ping);
    }
}

static int spawn(void (*entry)(void*), char const* name)
{
    struct kos_thread_params p = {0};
    // A null out-pointer is refused -KOS_EINVAL, so the sink is not optional.
    kos_thread_t h = KOS_THREAD_NONE;

    p.entry = entry;
    p.name = name;
    p.prio = 10;
    return kos_thread_create(&p, &h);
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    kos_print("hello from KickOS userspace!\n");
    kos_print("two threads play ping-pong. Press Ctrl+C to stop.\n\n");

    spawn(ping, "ping");
    spawn(pong, "pong");

    // A daemon: main never returns.
    while (true)
    {
        kos_sem_wait(&g_idle, KOS_TIMEOUT_NONE);
    }
}
