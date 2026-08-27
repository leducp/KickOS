// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The thread_local witness: each thread reads back what IT wrote, the storage is at a DIFFERENT
// address per thread, and a .tdata initialiser arrives.

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys/atomic.h>

namespace
{
    constexpr int WORKERS = 2;

    // .tdata (a non-zero initialiser) and .tbss (no initialiser).
    //
    // VOLATILE ON THE SEEDED ONE OR THE CHECK IS VACUOUS: a thread_local that is only ever read
    // is constant-folded at -Os, .tdata comes out EMPTY, and comparing the read against the
    // initialiser then proves nothing about any template.
    thread_local volatile unsigned g_seeded = 0xA5A5A5A5u;
    thread_local unsigned g_written = 0;

    // `done` PUBLISHES the four fields above it: the worker fills them and stores it last,
    // main reads it first. Plain words gave the reader no ordering and let its poll loop keep
    // the flag in a register, so the whole struct was a data race.
    struct Report
    {
        unsigned addr;
        unsigned sp;
        unsigned seeded;
        unsigned read_back;
        kickos::Atomic<unsigned, kickos::Order::ACQUIRE | kickos::Order::RELEASE> done;
    };

    unsigned read_sp()
    {
        unsigned sp = 0;
#if defined(__arm__)
        __asm__ volatile("mov %0, sp" : "=r"(sp));
#elif defined(__aarch64__)
        // Through a 64-bit temporary: `mov w0, sp` is not an encoding, and the report field
        // is 32 bits, which every address on the boards that run this fits in.
        unsigned long long sp64 = 0;
        __asm__ volatile("mov %0, sp" : "=r"(sp64));
        sp = static_cast<unsigned>(sp64);
#elif defined(__riscv)
        __asm__ volatile("mv %0, sp" : "=r"(sp));
#elif defined(__XTENSA__)
        __asm__ volatile("mov %0, a1" : "=r"(sp));
#elif defined(__RX__)
        __asm__ volatile("mov.l r0, %0" : "=r"(sp));
#endif
        return sp;
    }

    Report g_report[WORKERS] = {};

    void worker(void* arg)
    {
        int const k = static_cast<int>(reinterpret_cast<uintptr_t>(arg));
        unsigned const mine = 0xC0DE0000u + static_cast<unsigned>(k);
        // Read BEFORE the write, so a template that never arrived is visible as a wrong seed
        // rather than as a zero nobody can attribute.
        unsigned const seeded = g_seeded;
        g_seeded = 0;
        g_written = mine;
        // A round trip through the scheduler: on an arch whose thread pointer is a register,
        // this is where a switch that forgot to restore it shows up.
        kos::sleep_ns(50000000ull);
        g_report[k].addr = static_cast<unsigned>(reinterpret_cast<uintptr_t>(&g_written));
        g_report[k].sp = read_sp();
        g_report[k].seeded = seeded;
        g_report[k].read_back = g_written;
        g_report[k].done = 1;
        while (true)
        {
            kos::sleep_ns(1000000000ull);
        }
    }
}

int main(int, char**)
{
    kos::print("[tlsprobe] start\n");

    unsigned const root_addr = static_cast<unsigned>(reinterpret_cast<uintptr_t>(&g_written));
    {
        char d[72];
        ksnprintf(d, sizeof(d), "[tlsprobe] root tp %x sp %x\n", root_addr, read_sp());
        kos::print(d);
    }
    g_written = 0x4F4F5400u;

    for (int k = 0; k < WORKERS; k++)
    {
        kos::thread::create(worker, reinterpret_cast<void*>(static_cast<uintptr_t>(k)),
                            "tlsw", 10);
    }

    for (int spin = 0; spin < 200; spin++)
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
    char b[80];
    for (int k = 0; k < WORKERS; k++)
    {
        unsigned const want = 0xC0DE0000u + static_cast<unsigned>(k);
        char const* verdict = "ok";
        if (g_report[k].done == 0)
        {
            verdict = "NEVER RAN";
            bad++;
        }
        else if (g_report[k].read_back != want)
        {
            verdict = "READ BACK WRONG";
            bad++;
        }
        else if (g_report[k].seeded != 0xA5A5A5A5u)
        {
            verdict = "TEMPLATE MISSING";
            bad++;
        }
        else if (g_report[k].addr == root_addr)
        {
            verdict = "SHARED WITH ROOT";
            bad++;
        }
        // THE BLOCK MUST BE THE ONE THIS THREAD IS STANDING ON: a thread pointer derived from SP
        // names the NEIGHBOUR's block when SP sits exactly at an exclusive stack top, which
        // checking the two against each other makes visible here rather than as a data abort in
        // whichever thread happens to be adjacent.
        else if (g_report[k].addr > g_report[k].sp)
        {
            verdict = "BLOCK ABOVE SP";
            bad++;
        }
#if defined(KICKOS_TLS_STRIDE)
        else if (g_report[k].sp - g_report[k].addr >= KICKOS_TLS_STRIDE)
        {
            verdict = "BLOCK MORE THAN ONE STRIDE BELOW SP";
            bad++;
        }
#endif
        ksnprintf(b, sizeof(b), "[tlsprobe] w%d tp %x sp %x seed %x read %x %s\n", k,
                  g_report[k].addr, g_report[k].sp, g_report[k].seeded,
                  g_report[k].read_back, verdict);
        kos::print(b);
    }
    if (WORKERS == 2 and g_report[0].addr == g_report[1].addr and g_report[0].done != 0)
    {
        kos::print("[tlsprobe] w0 and w1 SHARE ONE BLOCK\n");
        bad++;
    }
    if (g_written != 0x4F4F5400u)
    {
        kos::print("[tlsprobe] ROOT COPY CLOBBERED\n");
        bad++;
    }
    ksnprintf(b, sizeof(b), "[tlsprobe] root addr %x read %x\n", root_addr, g_written);
    kos::print(b);

    if (bad == 0)
    {
        kos::print("[tlsprobe] PASS\n");
    }
    else
    {
        kos::print("[tlsprobe] FAIL\n");
    }
    // Returning from main is a kos_shutdown(0), which is what ends the qemu run.
    return 0;
}
