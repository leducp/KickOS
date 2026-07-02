// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// KickOS M0 self-test app (unprivileged userspace, C++). The CI gate: exercises
// every M0 verification bullet across the syscall boundary and ends with a
// deliberate MPU fault. Not a friendly demo -- see apps/hello for that.
//   - user SVC roundtrip returns correct results
//   - two-thread FIFO ordering
//   - a higher-priority thread preempts on ready (sem post, thread ctx)
//   - a semaphore posted from an IRQ handler switches, tick disabled (IRQ ctx)
//   - RR round-robins equal-priority threads
//   - sleep ordering via the tickless timer queue
//   - two equal-priority threads block on one semaphore (wait-queue regression)
//   - a privileged guard access survives a syscall; an unprivileged wild write
//     is caught and reported via mprotect

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/libc/fmt.h>
#include <kickos/libc/string.h>

namespace
{

    int g_done = -1; // completion counter
    int g_go = -1;   // preempt hand-off
    int g_irq = -1;  // IRQ-posted semaphore

    void line(char const* s)
    {
        kos_write(1, s, strlen(s));
    }

    // --- FIFO ordering ---------------------------------------------------------
    void fifo_worker(void* arg)
    {
        char b[64];
        ksnprintf(b, sizeof(b), "[fifo] %s running\n", static_cast<char const*>(arg));
        line(b);
        kos_sem_post(g_done);
    }

    // --- Priority preemption on ready (thread-ctx post) ------------------------
    void preempt_high(void*)
    {
        kos_sem_wait(g_go);
        line("[preempt] high preempts\n");
        kos_sem_post(g_done);
    }
    void preempt_low(void*)
    {
        line("[preempt] low before\n");
        kos_sem_post(g_go); // wakes higher-prio 'high' -> preempts now
        line("[preempt] low after\n");
        kos_sem_post(g_done);
    }

    // --- IRQ-context post ------------------------------------------------------
    void irq_waiter(void*)
    {
        kos_sem_wait(g_irq);
        line("[irq] woken by IRQ handler\n");
        kos_sem_post(g_done);
    }
    void irq_injector(void*)
    {
        line("[irq] injecting\n");
        kos_irq_inject(5); // ISR posts g_irq -> switch on IRQ exit
        line("[irq] injector resumes\n");
        kos_sem_post(g_done);
    }

    // --- Round-robin -----------------------------------------------------------
    void rr_worker(void* arg)
    {
        char const* name = static_cast<char const*>(arg);
        for (int i = 1; i <= 3; i++)
        {
            char b[48];
            ksnprintf(b, sizeof(b), "[rr] %s %d\n", name, i);
            line(b);
            // Burn ~2 ms of wall-clock, longer than the 1 ms slice, so the timer
            // preempts mid-iteration and rotates to the equal-priority peer.
            uint64_t start = kos_clock_now();
            while (kos_clock_now() - start < 2000000ull)
            {
            }
        }
        kos_sem_post(g_done);
    }

    // --- Sleep ordering (tickless timer) ---------------------------------------
    void sleeper(void* arg)
    {
        unsigned ms = static_cast<unsigned>(reinterpret_cast<uintptr_t>(arg));
        kos_sleep_ns(static_cast<uint64_t>(ms) * 1000000ull);
        char b[48];
        ksnprintf(b, sizeof(b), "[sleep] %ums woke\n", ms);
        line(b);
        kos_sem_post(g_done);
    }

    // --- Two equal-priority threads blocking on one semaphore ------------------
    // Regression: the blocker must detach from the ready list before parking on
    // the wait queue (shared qnext/qprev links), and a wait queue must hold >1
    // waiter. Without the fix the second equal-priority thread is orphaned and
    // this stage hangs.
    int g_multi = -1;
    void multi_worker(void* arg)
    {
        kos_sem_wait(g_multi);
        char b[48];
        ksnprintf(b, sizeof(b), "[multi] %s woke\n", static_cast<char const*>(arg));
        line(b);
        kos_sem_post(g_done);
    }

    // --- MPU wild write: an unprivileged thread must not reach address 0 -------
    void wild_writer(void*)
    {
        line("[mpu] unprivileged write to NULL (expect fault)\n");
        volatile int* p = nullptr;
        // Launder the pointer so the compiler cannot fold the null store into UB.
        asm volatile("" : "+r"(p));
        *p = 0xdead; // faults: address 0 is unmapped / unowned
        line("[mpu] ERROR: null write did not fault\n");
        kos_sem_post(g_done);
    }

    void wait_done(int count)
    {
        for (int i = 0; i < count; i++)
        {
            kos_sem_wait(g_done);
        }
    }

}

extern "C" void kickos_app_main(void)
{
    line("[selftest] start\n");

    g_done = kos_sem_create(0);
    g_go = kos_sem_create(0);
    g_irq = kos_sem_create(0);

    // SVC roundtrip returns the correct byte count.
    {
        long n = kos_write(1, "abc\n", 4);
        char b[64];
        ksnprintf(b, sizeof(b), "[svc] write returned %ld\n", n);
        line(b);
    }

    line("[fifo] start\n");
    kos::thread::spawn(fifo_worker, const_cast<char*>("A"), "fifoA", 10);
    kos::thread::spawn(fifo_worker, const_cast<char*>("B"), "fifoB", 10);
    wait_done(2);

    line("[preempt] start\n");
    kos::thread::spawn(preempt_high, nullptr, "high", 20);
    kos::thread::spawn(preempt_low, nullptr, "low", 8);
    wait_done(2);

    line("[irq] start\n");
    kos_irq_attach(5, g_irq);
    kos::thread::spawn(irq_waiter, nullptr, "irqwaiter", 15);
    kos::thread::spawn(irq_injector, nullptr, "injector", 8);
    wait_done(2);

    line("[rr] start\n");
    kos::thread::spawn(rr_worker, const_cast<char*>("A"), "rrA", 10,
                       KOS_POLICY_RR, 1000000u, /*privileged=*/true);
    kos::thread::spawn(rr_worker, const_cast<char*>("B"), "rrB", 10,
                       KOS_POLICY_RR, 1000000u, /*privileged=*/true);
    wait_done(2);

    line("[sleep] start\n");
    kos::thread::spawn(sleeper, reinterpret_cast<void*>(uintptr_t{40}), "sleepLong", 10);
    kos::thread::spawn(sleeper, reinterpret_cast<void*>(uintptr_t{10}), "sleepShort", 10);
    wait_done(2);

    line("[multi] start\n");
    g_multi = kos_sem_create(0);
    kos::thread::spawn(multi_worker, const_cast<char*>("A"), "multiA", 10);
    kos::thread::spawn(multi_worker, const_cast<char*>("B"), "multiB", 10);
    kos_sleep_ns(5000000ull); // let both block on g_multi (two waiters, equal prio)
    kos_sem_post(g_multi);
    kos_sem_post(g_multi);
    wait_done(2);

#if defined(KICKOS_ENABLE_SELFTEST)
    // Privileged access to protected memory must survive a syscall: the trap
    // epilogue restores the caller's MPU posture (RW here), not PROT_NONE.
    // (Uses the test-only guard-probe syscall.)
    {
        volatile int* g = static_cast<volatile int*>(kos_guard_addr());
        *g = 0x1234; // privileged: guard is RW; must NOT fault
        char b[64];
        ksnprintf(b, sizeof(b), "[mpu] privileged guard write ok (%d)\n", *g);
        line(b);
    }
#endif

    line("[done] all scheduler demos passed\n");
    line("DEMO COMPLETE\n");

    // Final: an unprivileged wild write traps via mprotect and is reported.
    kos::thread::spawn(wild_writer, nullptr, "wild", 10);
    // root returns here -> exits; 'wild' runs, faults, kernel reports + halts.
}
