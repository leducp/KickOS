// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// M4.3 per-thread stdout regression. Proves that a root pre-publish printf (whose
// cap index 0 is empty, so its send fails) does NOT poison a post-publish worker
// whose cap 0 IS seated to the console endpoint. _write (user/src/newlib_stubs.cc)
// self-classifies per invocation against the CALLING thread's own cap 0; a
// process-wide probe would instead let root's one failed send divert every later
// thread to the debug console.
//
// Console is DARK to an external observer after publish (stdout routes to the
// software counting driver below, which never re-emits), so the verdict cannot ride
// stdout markers. It rides the EXIT STATUS: main returns 0 iff the driver received
// exactly the worker's known payload byte count, else 1. On mps2 arch_shutdown
// forwards that status via semihosting SYS_EXIT_EXTENDED (see chip_mps2.cc), and the
// CTest gate reads QEMU's process exit code.
//
// qemu (mps2) is a NON-enforcement board, so all threads share one address space and
// the globals below are a legitimate cross-thread channel.

#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/sys/atomic.h>

#include <stdio.h>

namespace
{
    using kickos::Atomic;
    using kickos::Order;

    constexpr uint8_t DRIVER_PRIO = 12; // >= WORKER_PRIO (D9: rendezvous has no PI)
    constexpr uint8_t WORKER_PRIO = 10;

    // Byte count is known at compile time: the driver must receive exactly this many
    // bytes over the endpoint.
    constexpr char WORKER_PAYLOAD[] =
        "[worker] initdemo payload line 1\n"
        "[worker] initdemo payload line 2\n"
        "[worker] initdemo payload line 3\n";
    constexpr size_t PAYLOAD_LEN = sizeof(WORKER_PAYLOAD) - 1;

    // Cross-thread channel (shared address space on this non-enforcement board).
    // g_worker_done is set after the worker's final fflush, before it exits.
    Atomic<int32_t, Order::RELAXED> g_driver_bytes{0};
    Atomic<int, Order::RELAXED> g_worker_done{0};

    // A no-hardware sink: counts the bytes it receives on the delegated endpoint cap
    // (B1: first delegated cap lands at child table index 1). It never prints, because a
    // printf would self-send to the very endpoint it serves and deadlock. A negative recv
    // (dead endpoint) ends it; otherwise it parks in recv until shutdown.
    void console_sink(void*)
    {
        int const ep = KOS_SPAWN_DELEGATED_CAP0;
        char buf[KOS_EP_MSG_MAX];
        while (true)
        {
            // Info-less recv: this sink counts plain sends only (a client kos_call
            // bounces -KOS_ENOSYS rather than minting a reply cap here).
            int32_t const n = kos_recv(ep, buf, sizeof(buf), nullptr);
            if (n < 0)
            {
                break;
            }
            g_driver_bytes = g_driver_bytes + n;
        }
        kos_exit(0);
    }

    // Post-publish worker: an ordinary app that just prints. Its cap 0 was seated to
    // the published endpoint by cap_install_defaults at spawn, so _write self-sends
    // there and the sink counts the bytes.
    void worker(void*)
    {
        fwrite(WORKER_PAYLOAD, 1, PAYLOAD_LEN, stdout);
        fflush(stdout);
        g_worker_done = 1;
        kos_exit(0);
    }
}

KICKOS_APP_AUTHORITY(KOS_AUTH_MEMORY | KOS_AUTH_SYSTEM | KOS_AUTH_CONSOLE);

int main(int, char**)
{
    // The poison write: root's cap 0 is EMPTY (root predates any publish), so this send
    // fails and falls back to the debug console. Must run BEFORE publish.
    printf("[init] pre-publish\n");
    fflush(stdout);

    kos_cap_t ep = KOS_CAP_NONE;
    if (kos_endpoint_create(&ep) != 0)
    {
        kos::print("[initdemo] ERROR: endpoint_create failed\n");
        return 2;
    }

    // Route stdout to the endpoint (kernel chip path drops; children spawned AFTER
    // this get cap 0 seated to it). Gated on AUTH_CONSOLE, which root holds.
    if (kos_console_publish(ep) != 0)
    {
        kos::print("[initdemo] ERROR: console_publish failed\n");
        return 2;
    }

    // Spawn the counting sink with a narrowed {ep | WAIT} recv cap (lands at child
    // index 1). No SIGNAL/TRANSFER: it only receives.
    kos_cap_grant const caps[1] = {
        { .source_cap = ep, .rights_mask = KOS_CAP_WAIT },
    };
    auto const drv = kos::thread::spawn_caps(console_sink, nullptr, "sink", DRIVER_PRIO,
                                             caps, /*cap_count=*/1);
    if (not drv.valid())
    {
        kos::print("[initdemo] ERROR: sink spawn failed\n");
        return 2;
    }

    // Drop root's own WAIT cap so the sink is the sole receiver (S4). g_stdout_target
    // holds the endpoint alive on the kernel ref, so this does not tear it down.
    kos_handle_close(ep);

    // Spawn the printing worker AFTER publish, so its cap 0 is seated to the endpoint.
    auto const w = kos::thread::spawn(worker, nullptr, "worker", WORKER_PRIO);
    if (not w.valid())
    {
        kos::print("[initdemo] ERROR: worker spawn failed\n");
        return 2;
    }

    // Bounded wait for the worker to finish, then a bounded settle so the sink has
    // counted its last batch (the sink's count += n runs just after each rendezvous
    // returns to the worker).
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
        if (g_driver_bytes >= static_cast<int32_t>(PAYLOAD_LEN))
        {
            break;
        }
        kos_sleep_ns(10000000ull);
    }

    int rc = 1;
    if (g_driver_bytes == static_cast<int32_t>(PAYLOAD_LEN))
    {
        rc = 0;
    }
    return rc;
}
