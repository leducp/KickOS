// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Console reclaim when the DRIVER DIES. USER_OWNED drops every kernel write, so a driver
// that exits without the reclaim hook leaves the system permanently mute: no panic
// banner, no fault dump, no kprintf. The hook is console_on_driver_death, run by
// exit_current AFTER cap_teardown.
//
// The composition names the packaged simcon as stdout, with no restart, and main watches it.
// Sequenced by the wire and the init's death report, not by sleeps. The image is built with
// KICKOS_SIMCON_EXIT_AFTER=2, so the driver serves the handover probe and one message, then
// exits:
//
//   0. a raw kernel console write, dropped because the console is USER_OWNED. Its ABSENCE is
//      the anti-vacuity witness: a line on the wire here means the console was never
//      published and every assertion below is meaningless.
//   1. kos::print() -> the published route, served by the driver, reaches the wire.
//   2. the driver exits, which ends its task: that end notes the console death, and the
//      reclaim runs once no thread of it holds the device.
//   3. the init reports the death on /init/events, then closes the endpoint for good, so a
//      send is refused -KOS_ECONNREFUSED. That is what PROVES the driver is gone rather than
//      merely slow, with no timing assumption.
//   4. the SAME raw write now has to reach the wire. Steps 0 and 4 together are the
//      whole assertion.
//
// Under KICKOS_SIMCON_WINDOW_THREAD the driver is THREE threads: a service thread that
// receives, a window thread at the lowest priority holding the register window, and the
// task's entry. The receiver's exit leaves a live driver with no receiver, where a send parks,
// and a thread faulting then leaves its record held for that driver. The entry's release then
// ends the task while the window is still held: the reclaim writes the record, and a writer
// parked across that end lands its line once, after it.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/atomic.h>
#include <kickos/sys/cap_index.h>
#include <kickos/sys/emit.h>
#include <kickos/sys/errno.h>

#include <stdlib.h>

namespace
{
    // One raw kernel console write: on the wire only while the kernel owns the console.
    void kconsole_raw(char const* s)
    {
        size_t n = 0;
        while (s[n] != '\0')
        {
            n++;
        }
        (void)kos_kconsole_write(s, n); // a dropped line is the measurement
    }

    char const BLOCKED_LINE[] = "[drvdeath] blocked line, written once after the reclaim\n";

    // A writer of a console whose driver is dead gets -KOS_EAGAIN while the init still holds
    // the endpoint, and -KOS_ECONNREFUSED once it closed it: 1 ms steps, a second at most.
    constexpr int REFUSED_TRIES = 1000;
    constexpr uint64_t REFUSED_STEP_NS = 1000000ull;

    int send_until_refused()
    {
        int rc = kos_send(KOS_CAP_STDOUT, "x", 1);
        for (int i = 0; i < REFUSED_TRIES and rc == -KOS_EAGAIN; i++)
        {
            kos_sleep_ns(REFUSED_STEP_NS);
            rc = kos_send(KOS_CAP_STDOUT, "x", 1);
        }
        return rc;
    }

    // Blocks until the init reports the driver's death: bit 0, simcon being the one task main
    // watches.
    bool wait_driver_death(kos_cap_t events)
    {
        uint32_t bits = 0;
        return kos_notify_wait(events, 1u, KOS_TIMEOUT_NONE, &bits) == 0 and (bits & 1u) != 0;
    }
}

#if defined(KICKOS_SIMCON_WINDOW_THREAD) && KICKOS_SIMCON_WINDOW_THREAD
extern "C" void kickos_simcon_window_release(void);

namespace
{
    using kickos::Atomic;
    using kickos::Order;

    // Above the window thread and main, at main's ceiling or below.
    constexpr uint8_t WRITER_PRIO = 9;
    // How long a parked writer is watched before it counts as parked, and how long its line may
    // take once the reclaim lands.
    constexpr uint32_t WRITER_PARKED_US = 20000u;
    constexpr uint32_t WRITER_DONE_US = 2000000u;

    void blocked_writer(void*)
    {
        kos::print(BLOCKED_LINE);
    }

    // Its record is held for a driver that no longer receives, so only the reclaim writes it,
    // and it writes it before it wakes anyone.
    void fault_now(void*)
    {
        __builtin_trap();
    }

    // The kill gate is PARENTHOOD, so witnessing a refusal needs a live thread this thread
    // did NOT spawn. Main spawns the child, hence a grandchild.
    constexpr uint8_t CAP_FULL =
        static_cast<uint8_t>(KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER);
    constexpr int NEST_DONE = KOS_SPAWN_DELEGATED_CAP0;     // the child's gate back to main
    constexpr int NEST_PARK = KOS_SPAWN_DELEGATED_CAP0 + 1; // what the grandchild and child wait on
    constexpr int NEST_PROBE = KOS_SPAWN_DELEGATED_CAP0 + 2; // main's gate back to the child

    Atomic<kos_thread_t, Order::RELAXED> g_grandchild{KOS_THREAD_NONE};
    Atomic<int, Order::RELAXED> g_child_kill_rc{1}; // 1 == the child never got that far
    Atomic<int, Order::RELAXED> g_root_kill_rc{1};  // 1 == the child never got that far

    // Root, which runs the init, takes the FIRST slot the thread pool ever allocates, so it is
    // index 0 at generation 0 on every board and posture, and handle_for(0) is the bare 0.
    // Change that encoding and this case silently names some other thread instead.
    constexpr kos_thread_t ROOT_THREAD = 0;

    void nest_grandchild(void*) // caps: park@1
    {
        kos_sem_wait(KOS_SPAWN_DELEGATED_CAP0); // never posted: alive until its spawner cancels it
        kos_exit(0);
    }

    void nest_child(void*) // caps: done@1, park@2, probe@3
    {
        kos_cap_grant const caps[1] = {{NEST_PARK, KOS_CAP_WAIT}};
        g_grandchild = kos::thread::create(nest_grandchild, nullptr, "nestgc", 9,
                                           KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                           nullptr, 0, nullptr, 0, nullptr, 0, caps, 1)
                           .id();
        // Root is unkillable: it leaves spawner_tag at KILL_TAG_NONE and kill_tag_of never
        // answers NONE.
        g_root_kill_rc = kos_thread_kill(ROOT_THREAD);
        kos_sem_post(NEST_DONE);
        // Main's refuse-half probe needs the grandchild ALIVE, and a cancel reaches a
        // semaphore park, so the accept half below would otherwise race it dead. The gate
        // makes the order explicit instead of resting on cancellation being toothless.
        kos_sem_wait(NEST_PROBE);
        // The accept half of the gate: a spawner may cancel its own child. Main's
        // -KOS_EPERM below is the refuse half.
        kos_thread_t const gc = g_grandchild;
        if (gc != KOS_THREAD_NONE)
        {
            g_child_kill_rc = kos_thread_kill(gc);
        }
        kos_sem_post(NEST_DONE);
        kos_sem_wait(NEST_PARK); // never posted: alive until main cancels it
        kos_exit(0);
    }

    // Runs while the console is still USER_OWNED, so nothing may be printed here: the
    // results have to be carried out through the reclaimed route.
    void kill_gate_matrix(int* bad_handle_rc, int* big_handle_rc, int* stranger_rc,
                          int* child_kill_rc, int* child_twice_rc)
    {
        // Both carry the reserved all-ones index (0x7fffffff at an aged generation), so
        // neither is mintable and both must fail to resolve.
        *bad_handle_rc = kos_thread_kill(KOS_THREAD_NONE);
        *big_handle_rc = kos_thread_kill(0x7fffffffu);
        // 0 is never a legal answer for these, so an unrun matrix fails.
        *stranger_rc = 0;
        *child_kill_rc = 1;
        *child_twice_rc = 0;
        kos_cap_t park = KOS_CAP_NONE;
        kos_cap_t done = KOS_CAP_NONE;
        kos_cap_t probe = KOS_CAP_NONE;
        int const park_rc = kos_sem_create(0, &park);
        int const done_rc = kos_sem_create(0, &done);
        int const probe_rc = kos_sem_create(0, &probe);
        if (park_rc != 0 or done_rc != 0 or probe_rc != 0)
        {
            return;
        }
        kos_cap_grant const caps[3] = {{done, CAP_FULL}, {park, CAP_FULL}, {probe, CAP_FULL}};
        auto const child = kos::thread::create(nest_child, nullptr, "nestch", 9,
                                               KOS_POLICY_FIFO, 0, /*privileged=*/false,
                                               nullptr, 0, nullptr, 0, nullptr, 0, caps, 3);
        if (not child.valid())
        {
            return;
        }
        kos_sem_wait(done); // the grandchild exists
        kos_thread_t const gc = g_grandchild;
        if (gc != KOS_THREAD_NONE)
        {
            *stranger_rc = kos_thread_kill(gc);
        }
        kos_sem_post(probe); // probed: the child may now cancel it for real
        kos_sem_wait(done);  // the child has tried its own cancel
        // Cancellation, not destruction: the child wakes out of its park and exits itself. It
        // runs above main, so it has exited by the time this returns. Killing it TWICE must not
        // resolve: the slot is EXITED and the generation only bumps at reclaim, so the
        // resolver has to reject on state, not on generation.
        *child_kill_rc = kos_thread_kill(child.id());
        *child_twice_rc = kos_thread_kill(child.id());
    }
}
#endif

extern "C" void drvdeath_main(kos_self_t const* self)
{
    kos_cap_t const events = kos_grant_notify(self, "/init/events");
    if (kos_notify_bind(events) != 0)
    {
        kos::print("[drvdeath] ERROR: no /init/events to wait the driver's death on\n");
        exit(9);
    }

    kconsole_raw("[drvdeath] kernel console BEFORE death (must NOT reach the wire)\n");

#if defined(KICKOS_SIMCON_WINDOW_THREAD) && KICKOS_SIMCON_WINDOW_THREAD
    int bad_handle_rc = 0;
    int big_handle_rc = 0;
    int stranger_rc = 0;
    int child_kill_rc = 0;
    int child_twice_rc = 0;
    kill_gate_matrix(&bad_handle_rc, &big_handle_rc, &stranger_rc, &child_kill_rc,
                     &child_twice_rc);
#endif

    // Served by the driver, which then exits (KICKOS_SIMCON_EXIT_AFTER=2, the handover probe
    // being the first). kos_send is a rendezvous, so this returns only once the driver has
    // taken the message.
    kos::print("[drvdeath] published route live\n");

#if defined(KICKOS_SIMCON_WINDOW_THREAD) && KICKOS_SIMCON_WINDOW_THREAD
    // The receiver runs above main and has exited by now, and its task lives on: a send would
    // park, so a send of no deadline and a non-blocking write take nothing, at once.
    int32_t const nowait_rc = kos_send_timed(KOS_CAP_STDOUT, "x", 1, 0);
    int const nonblock_on = kos_task_nonblock(KOS_NONBLOCK_SET);
    kickos::WriteResult const tried = kickos::stdout_write("x", 1);
    int const nonblock_off = kos_task_nonblock(KOS_NONBLOCK_CLEAR);
    // A blocking writer parks: it outranks main, so it is in its send when this returns.
    kos::thread::Handle const writer =
        kos::thread::create(blocked_writer, nullptr, "dwriter", WRITER_PRIO);
    int const parked_rc = writer.join(WRITER_PARKED_US);
    // A fault ends its thread's task, so the faulter gets one of its own.
    kos_task_t fault_task = KOS_TASK_NONE;
    int faulted_rc = kos_task_create(nullptr, 0, 0, &fault_task);
    if (faulted_rc == 0)
    {
        kos::thread::Handle const faulter =
            kos::thread::create(fault_now, nullptr, "dfault", WRITER_PRIO, KOS_POLICY_FIFO, 0,
                                /*privileged=*/false, nullptr, 0, nullptr, 0, nullptr, 0, nullptr,
                                0, 0, nullptr, fault_task);
        faulted_rc = faulter.join(WRITER_DONE_US);
        (void)kos_task_kill(fault_task);
    }
    // The receiver is gone but its task is not, so the console must still be USER_OWNED.
    // Absent on the wire == correct.
    kconsole_raw("[drvdeath] kernel console AFTER death, window HELD "
                 "(must NOT reach the wire)\n");
    // The entry's exit ends the driver's task while the window thread still holds the
    // registers: the reclaim lands at that thread's exit.
    kickos_simcon_window_release();
#endif

    if (not wait_driver_death(events))
    {
        kos_print("[drvdeath] ERROR: the init never reported the driver's death\n");
        exit(2);
    }
    if (send_until_refused() != -KOS_ECONNREFUSED)
    {
        // A live receiver means nothing below tests the reclaim. Reported through BOTH
        // routes: which one works is exactly what is in doubt here.
        kconsole_raw("[drvdeath] ERROR: driver still alive after its bounded serve\n");
        kos::print("[drvdeath] ERROR: driver still alive after its bounded serve\n");
        exit(1);
    }

#if defined(KICKOS_SIMCON_WINDOW_THREAD) && KICKOS_SIMCON_WINDOW_THREAD
    if (nowait_rc != -KOS_ETIMEDOUT or nonblock_on != 1 or tried.sent != 0u
        or tried.error != -KOS_ETIMEDOUT or nonblock_off != 0)
    {
        kos_print("[drvdeath] ERROR: a write of no wait was not refused at once by a live "
                  "driver with no receiver\n");
        exit(10);
    }
    if (parked_rc != -KOS_ETIMEDOUT)
    {
        kos_print("[drvdeath] ERROR: a writer of a live driver with no receiver did not park\n");
        exit(11);
    }
    if (faulted_rc != 0)
    {
        kos_print("[drvdeath] ERROR: the faulter's thread was not reported gone\n");
        exit(13);
    }
    if (writer.join(WRITER_DONE_US) != 0)
    {
        kos_print("[drvdeath] ERROR: the parked writer never finished after the reclaim\n");
        exit(12);
    }
    if (child_kill_rc != 0)
    {
        kos_print("[drvdeath] ERROR: a spawner could not cancel its own child\n");
        exit(3);
    }
    if (child_twice_rc != -KOS_EBADF)
    {
        kos_print("[drvdeath] ERROR: killing an exited thread did not answer EBADF\n");
        exit(4);
    }
    if (bad_handle_rc != -KOS_EBADF or big_handle_rc != -KOS_EBADF)
    {
        kos_print("[drvdeath] ERROR: a bogus thread handle was not EBADF\n");
        exit(5);
    }
    if (stranger_rc != -KOS_EPERM)
    {
        kos_print("[drvdeath] ERROR: killing a thread this app did not spawn was allowed\n");
        exit(6);
    }
    if (g_child_kill_rc != 0)
    {
        kos_print("[drvdeath] ERROR: a spawner could not cancel its own grandchild\n");
        exit(7);
    }
    if (g_root_kill_rc != -KOS_EPERM)
    {
        kos_print("[drvdeath] ERROR: root was killable by an app thread\n");
        exit(8);
    }
    kos_print("[drvdeath] kill gate: EBADF/EPERM refused, root unkillable, spawner accepted\n");
#endif

    // Identical call to the one dropped above; its presence on the wire is the assertion.
    kconsole_raw("[drvdeath] kernel console AFTER death (reclaimed)\n");
    exit(0);
}
