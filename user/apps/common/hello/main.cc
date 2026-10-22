// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KickOS "hello" (C++ flavor), against the ergonomic C++ API (kos::). The
// byte-for-byte identical program in the plain-C API lives in apps/hello_c.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>

namespace
{
    constexpr uint64_t BEAT_NS = 400000000ull; // 0.4 s between hits

    kos::Semaphore g_ping(1); // ping serves first
    kos::Semaphore g_pong;

    void say(char const* who, int n)
    {
        char b[48];
        ksnprintf(b, sizeof(b), "  %s %d\n", who, n);
        kos::print(b);
    }

    void ping(void*)
    {
        int n = 0;
        while (true)
        {
            g_ping.wait();
            kos::sleep_ns(BEAT_NS);
            say("ping", ++n);
            g_pong.post();
        }
    }
    void pong(void*)
    {
        int n = 0;
        while (true)
        {
            g_pong.wait();
            kos::sleep_ns(BEAT_NS);
            say("pong", ++n);
            g_ping.post();
        }
    }
}

int main(int, char**)
{
    kos::print("hello from KickOS userspace!\n");
    kos::print("two threads play ping-pong; press Ctrl+C to stop.\n\n");

    kos::thread::create(ping, nullptr, "ping", 10);
    kos::thread::create(pong, nullptr, "pong", 10);

    // A daemon: returning from main would exit, so main parks on a semaphore nobody posts
    // and the two players run until interrupted.
    static kos::Semaphore idle;
    while (true)
    {
        idle.wait();
    }
}
