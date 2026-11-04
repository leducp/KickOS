// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Check that main's failed stdout send before publication does not redirect
// later workers away from their valid console capability.
// The counting console driver does not print, so exit status reports whether
// it received the expected payload. QEMU reads the semihosting exit code.
// The sink is a console driver, whose byte count main asks for with a one-byte call.

#include <kickos/board_config.h>
#include <kickos/kos.h>
#include <kickos/sys.h>

#include <kickos/sys/atomic.h>
#include <kickos/sys/driver_service.h>

#include <stdio.h>
#include <string.h>

namespace
{
    using kickos::Atomic;
    using kickos::Order;

    // Below the sink's priority, which the composition states: the rendezvous has no priority
    // inheritance.
    constexpr uint8_t WORKER_PRIO = 10;

    // Byte count is known at compile time: the driver must receive exactly this many
    // bytes over the endpoint.
    constexpr char WORKER_PAYLOAD[] =
        "[worker] initdemo payload line 1\n"
        "[worker] initdemo payload line 2\n"
        "[worker] initdemo payload line 3\n";
    constexpr size_t PAYLOAD_LEN = sizeof(WORKER_PAYLOAD) - 1;

    // g_worker_done is set after the worker's final fflush, before it exits.
    Atomic<int, Order::RELAXED> g_worker_done{0};

    // The bytes the sink has taken, or -1.
    int32_t sink_bytes(kos_cap_t ep)
    {
        char buf[KOS_EP_MSG_MAX] = {'Q'};
        int32_t const n = kos_call(ep, buf, 1, sizeof(buf), 1000000u);
        if (n != static_cast<int32_t>(sizeof(int32_t)))
        {
            return -1;
        }
        int32_t count = 0;
        memcpy(&count, buf, sizeof(count));
        return count;
    }

    // Post-publish worker: an ordinary app that just prints. Its cap 0 was seated to
    // the published endpoint by cap_install_defaults at spawn, so _write sends there and the
    // sink counts the bytes.
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
    // The poison write: main's cap 0 is EMPTY (main was spawned before any publish), so this
    // send fails and falls back to the debug console. Must run BEFORE publish.
    printf("[init] pre-publish\n");
    fflush(stdout);

    kos_cap_t ep = KOS_CAP_NONE;
    if (kos_endpoint_create(&ep) != 0)
    {
        kos::print("[initdemo] ERROR: endpoint_create failed\n");
        return 2;
    }

    // The sink serves the console from a task of its own: a thread of the serving task gets no
    // stdout, so the worker below must live outside it. Its declaration publishes the console
    // through the endpoint main passes.
    kos_program_t prog = KOS_CAP_NONE;
    if (kos_program_find(KICKOS_APP_IMAGE, &prog) != 0)
    {
        kos::print("[initdemo] ERROR: no program for the sink\n");
        return 2;
    }
    kos_cap_grant const caps[1] = {
        { .source_cap = ep, .rights_mask = KOS_CAP_WAIT | KOS_CAP_TRANSFER },
    };
    kos_task_params params = {};
    params.instance = "sink";
    params.caps = caps;
    params.cap_count = 1;
    kos_task_t sink_task = KOS_TASK_NONE;
    int const created = kos_task_create(prog, &params, &sink_task);
    (void)kos_handle_close(prog);
    if (created != 0)
    {
        kos::print("[initdemo] ERROR: sink create failed\n");
        return 2;
    }

    // Drop main's own WAIT so the sink is the sole receiver; the endpoint stays to ask its count.
    (void)kos_cap_narrow(ep, kickos::driver::KOS_DRV_HANDOVER_KEPT);

    // Spawn the printing worker AFTER publish, so its cap 0 is seated to the endpoint.
    auto const w = kos::thread::create(worker, nullptr, "worker", WORKER_PRIO);
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
        if (sink_bytes(ep) >= static_cast<int32_t>(PAYLOAD_LEN))
        {
            break;
        }
        kos_sleep_ns(10000000ull);
    }

    int rc = 1;
    if (sink_bytes(ep) == static_cast<int32_t>(PAYLOAD_LEN))
    {
        rc = 0;
    }
    return rc;
}
