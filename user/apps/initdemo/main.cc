// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// M4.3 per-thread stdout regression. Proves that a root pre-publish printf (whose
// cap index 0 is empty, so its send fails) does NOT poison a post-publish worker
// whose cap 0 IS seated to the console endpoint. The bug this guards was a sticky
// process-wide probe in newlib_stubs.cc: once ANY thread's kos_send(0) failed, every
// later _write skipped the endpoint and fell back to the debug console. The fix makes
// _write self-classify per invocation against the CALLING thread's own cap 0.
//
// Console is DARK to an external observer after publish (stdout routes to the
// software counting driver below, which never re-emits), so the verdict cannot ride
// stdout markers. It rides the EXIT STATUS: main returns 0 iff the driver received
// exactly the worker's known payload byte count, else 1. On mps2 arch_shutdown
// forwards that status via semihosting SYS_EXIT_EXTENDED (see chip_mps2.cc), and the
// CTest gate reads QEMU's process exit code.
//
// PRE-fix: root's pre-publish printf pins the sticky probe dead, the worker's bytes
// bypass the endpoint (kconsole -> dropped), driver count == 0, main returns 1.
// POST-fix: the worker's cap 0 is used, driver count == PAYLOAD_LEN, main returns 0.
//
// qemu (mps2) is a NON-enforcement board, so all threads share one address space and
// the globals below are a legitimate cross-thread channel (no grant needed).

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <stdio.h>

namespace
{
    constexpr uint8_t DRIVER_PRIO = 12; // >= WORKER_PRIO (D9: rendezvous has no PI)
    constexpr uint8_t WORKER_PRIO = 10;

    // The worker's fixed payload. Byte count is known at compile time; the driver
    // must receive exactly this many bytes over the endpoint.
    constexpr char WORKER_PAYLOAD[] =
        "[worker] initdemo payload line 1\n"
        "[worker] initdemo payload line 2\n"
        "[worker] initdemo payload line 3\n";
    constexpr size_t PAYLOAD_LEN = sizeof(WORKER_PAYLOAD) - 1;

    // Cross-thread channel (shared address space on this non-enforcement board).
    // g_driver_bytes: total bytes the counting driver pulled off the endpoint.
    // g_worker_done: set by the worker after its final fflush, before it exits.
    volatile long g_driver_bytes = 0;
    volatile int g_worker_done = 0;

    // Software console driver: a no-hardware sink. Recv on the delegated endpoint cap
    // (B1: first delegated cap lands at child table index 1) and count bytes. It never
    // prints (a printf would self-send to the very endpoint it serves and deadlock).
    // Exits on a negative recv (dead endpoint); otherwise parks in recv until shutdown.
    void console_sink(void*)
    {
        int const ep = 1;
        char buf[KOS_EP_MSG_MAX];
        for (;;)
        {
            uint32_t badge = 0;
            long const n = kos_recv(ep, buf, sizeof(buf), &badge);
            if (n < 0)
            {
                break;
            }
            g_driver_bytes += n;
        }
        kos_exit(0);
    }

    // Post-publish worker: an ordinary app that just prints. Its cap 0 was seated to
    // the published endpoint by cap_install_defaults at spawn, so _write self-sends
    // there -> the sink counts the bytes. Prints the fixed payload, flushes, signals.
    void worker(void*)
    {
        fwrite(WORKER_PAYLOAD, 1, PAYLOAD_LEN, stdout);
        fflush(stdout);
        g_worker_done = 1;
        kos_exit(0);
    }
}

int main(int, char**)
{
    // The poison write: root's cap 0 is EMPTY (root predates any publish), so this
    // send fails and falls back to the debug console. With the old sticky probe this
    // pinned the process route dead for every later thread. Must run BEFORE publish.
    printf("[init] pre-publish\n");
    fflush(stdout);

    int const ep = kos_endpoint_create();
    if (ep < 0)
    {
        kos::print("[initdemo] ERROR: endpoint_create failed\n");
        return 2;
    }

    // Route stdout to the endpoint (kernel chip path drops; children spawned AFTER
    // this get cap 0 seated to it). Privileged syscall; root is privileged.
    if (kos_console_publish(ep) != 0)
    {
        kos::print("[initdemo] ERROR: console_publish failed\n");
        return 2;
    }

    // Spawn the counting sink with a narrowed {ep | WAIT} recv cap (lands at child
    // index 1). No SIGNAL/TRANSFER: it only receives.
    kos_cap_grant const caps[1] = {
        { /*source_cap=*/ep, /*rights_mask=*/KOS_CAP_WAIT },
    };
    int const drv = kos::thread::spawn_caps(console_sink, nullptr, "sink", DRIVER_PRIO,
                                            caps, /*cap_count=*/1);
    if (drv < 0)
    {
        kos::print("[initdemo] ERROR: sink spawn failed\n");
        return 2;
    }

    // Drop root's own WAIT cap so the sink is the sole receiver (S4). g_stdout_target
    // holds the endpoint alive on the kernel ref, so this does not tear it down.
    kos_handle_close(ep);

    // Spawn the printing worker AFTER publish, so its cap 0 is seated to the endpoint.
    int const w = kos::thread::spawn(worker, nullptr, "worker", WORKER_PRIO);
    if (w < 0)
    {
        kos::print("[initdemo] ERROR: worker spawn failed\n");
        return 2;
    }

    // Bounded wait for the worker to finish, then a bounded settle so the sink has
    // counted its last batch (the sink's count += n runs just after each rendezvous
    // returns to the worker). No hang: every wait is capped.
    for (int i = 0; i < 200; i++)
    {
        if (g_worker_done != 0)
        {
            break;
        }
        kos_sleep_ns(10000000ull); // 10 ms
    }
    for (int i = 0; i < 50; i++)
    {
        if (g_driver_bytes >= static_cast<long>(PAYLOAD_LEN))
        {
            break;
        }
        kos_sleep_ns(10000000ull);
    }

    // Verdict rides the exit status. Console is dark, so this is the only signal.
    int rc = 1;
    if (g_driver_bytes == static_cast<long>(PAYLOAD_LEN))
    {
        rc = 0;
    }
    return rc;
}
