// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The tree's only .c, so the only BUILD that compiles the C-facing headers as C with the
// real flags. Rewriting it as C++, or including a C++-only header, leaves the C claim
// those headers make resting on check_c_headers.sh's standalone compile alone.

#include <stdbool.h>

#include <kickos/sys.h>
#include <kickos/libc/fmt.h>

static uint64_t const BEAT_NS = 400000000ull; // 0.4 s between hits

static kos_cap_t g_ping = KOS_CAP_NONE; // MAIN's cap
static kos_cap_t g_pong = KOS_CAP_NONE;

// Both sems are delegated on every spawn; a fresh child table makes handle == index.
static kos_cap_t const CH_PING = 1;
static kos_cap_t const CH_PONG = 2;

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
        kos_sem_wait(CH_PING);
        kos_sleep_ns(BEAT_NS);
        say("ping", ++n);
        kos_sem_post(CH_PONG);
    }
}

static void pong(void* arg)
{
    int n = 0;
    (void)arg;
    while (true)
    {
        kos_sem_wait(CH_PONG);
        kos_sleep_ns(BEAT_NS);
        say("pong", ++n);
        kos_sem_post(CH_PING);
    }
}

static int spawn(void (*entry)(void*), char const* name)
{
    struct kos_cap_grant caps[] = {{g_ping, KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER},
                                   {g_pong, KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER}};
    struct kos_thread_params p = {0};
    // A null out-pointer is refused -KOS_EINVAL, so the sink is not optional.
    kos_thread_t h = KOS_THREAD_NONE;

    p.entry = entry;
    p.name = name;
    p.prio = 10;
    p.caps = caps;
    p.cap_count = 2;
    return kos_thread_create(&p, &h);
}

int main(int argc, char** argv)
{
    kos_cap_t idle = KOS_CAP_NONE;

    (void)argc;
    (void)argv;

    kos_print("hello from KickOS userspace!\n");
    kos_print("two threads play ping-pong. Press Ctrl+C to stop.\n\n");

    (void)kos_sem_create(1, &g_ping); // ping serves first
    (void)kos_sem_create(0, &g_pong);

    spawn(ping, "ping");
    spawn(pong, "pong");

    // A daemon: main never returns.
    (void)kos_sem_create(0, &idle);
    while (true)
    {
        kos_sem_wait(idle);
    }
}
