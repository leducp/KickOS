// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The per-thread errno witness: each thread reads back the value ITS OWN libc call left, it
// reads it back AFTER a scheduler round trip that crossed a peer's write, and the threads
// resolve errno at different addresses.
//
// NOTHING HERE ASSIGNS errno. An assignment would pass with the mechanism ripped out, the write
// and the read landing on the same wrong word. Every value below is written by newlib's own
// _strtol_r: an overflowing literal gives ERANGE, an out-of-range base gives EINVAL.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>

#include <errno.h>
#include <stdlib.h>

namespace
{
    constexpr int WORKERS = 2;

    struct Report
    {
        unsigned addr;     // where libc resolves this thread's errno
        int provoked;      // errno straight after the call that set it
        int after_trip;    // errno after the peer has provoked its own
        unsigned trips;    // scheduler round trips waited out
        // Volatile so root's poll survives a build that can see through kos::sleep_ns. The
        // fields above are written before it and read after it.
        volatile unsigned done;
    };

    Report g_report[WORKERS] = {};
    volatile unsigned g_provoked[WORKERS] = {};

    // ERANGE for worker 0, EINVAL for worker 1, both out of newlib.
    int provoke(int k)
    {
        char* end = nullptr;
        if (k == 0)
        {
            (void)strtol("99999999999999999999999999", &end, 10);
        }
        else
        {
            (void)strtol("10", &end, 99);
        }
        return errno;
    }

    int want_for(int k)
    {
        if (k == 0)
        {
            return ERANGE;
        }
        return EINVAL;
    }

    unsigned errno_addr()
    {
        return static_cast<unsigned>(reinterpret_cast<uintptr_t>(&errno));
    }

    void worker(void* arg)
    {
        int const k = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
        g_report[k].addr = errno_addr();
        g_report[k].provoked = provoke(k);
        g_provoked[k] = 1;
        // WAIT FOR THE PEER'S WRITE, not just for time to pass: the read below has to happen
        // after another thread has put a different value in libc's state word, or an
        // implementation that seats the pointer once at thread entry would pass.
        unsigned trips = 0;
        while (true)
        {
            kos::sleep_ns(20000000ull);
            trips++;
            unsigned ready = 0;
            for (int i = 0; i < WORKERS; i++)
            {
                ready += g_provoked[i];
            }
            if (ready == WORKERS or trips > 200)
            {
                break;
            }
        }
        g_report[k].after_trip = errno;
        g_report[k].trips = trips;
        g_report[k].done = 1;
        while (true)
        {
            kos::sleep_ns(1000000000ull);
        }
    }
}

int main(int, char**)
{
    kos::print("[errnoprobe] start\n");

    unsigned const root_addr = errno_addr();
    int const root_provoked = provoke(0); // root takes ERANGE, like worker 0

    for (int k = 0; k < WORKERS; k++)
    {
        kos::thread::spawn(worker, reinterpret_cast<void*>(static_cast<uintptr_t>(k)),
                           "errw", 10);
    }

    for (int spin = 0; spin < 300; spin++)
    {
        int ready = 0;
        for (int k = 0; k < WORKERS; k++)
        {
            ready += static_cast<int>(g_report[k].done);
        }
        if (ready == WORKERS)
        {
            break;
        }
        kos::sleep_ns(20000000ull);
    }

    int bad = 0;
    char b[96];
    for (int k = 0; k < WORKERS; k++)
    {
        int const want = want_for(k);
        char const* verdict = "ok";
        if (g_report[k].done == 0)
        {
            verdict = "NEVER RAN";
            bad++;
        }
        else if (g_report[k].provoked != want)
        {
            verdict = "LIBC SET THE WRONG VALUE";
            bad++;
        }
        else if (g_report[k].after_trip != want)
        {
            verdict = "LOST ITS OWN ERRNO ACROSS A SWITCH";
            bad++;
        }
        else if (g_report[k].addr == root_addr)
        {
            verdict = "SHARES ROOT'S STATE";
            bad++;
        }
        ksnprintf(b, sizeof(b), "[errnoprobe] w%d at %x provoked %d after %d trips %u %s\n",
                  k, g_report[k].addr, g_report[k].provoked, g_report[k].after_trip,
                  g_report[k].trips, verdict);
        kos::print(b);
    }

    if (g_report[0].addr == g_report[1].addr)
    {
        kos::print("[errnoprobe] w0 and w1 SHARE ONE STATE\n");
        bad++;
    }
    if (g_report[0].after_trip == g_report[1].after_trip)
    {
        kos::print("[errnoprobe] BOTH WORKERS READ THE SAME ERRNO\n");
        bad++;
    }
    // Root provoked ERANGE before either worker ran and worker 1 then provoked EINVAL, so a
    // shared state hands root the peer's value.
    int const root_now = errno;
    if (root_provoked != ERANGE or root_now != ERANGE)
    {
        bad++;
    }
    ksnprintf(b, sizeof(b), "[errnoprobe] root at %x provoked %d now %d (want %d)\n",
              root_addr, root_provoked, root_now, ERANGE);
    kos::print(b);

    if (bad == 0)
    {
        kos::print("[errnoprobe] PASS\n");
    }
    else
    {
        kos::print("[errnoprobe] FAIL\n");
    }
    // Returning from main is a kos_shutdown(0), which is what ends the qemu run.
    return 0;
}
