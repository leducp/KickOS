// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Console reclaim when the DRIVER DIES. USER_OWNED drops every kernel write, so a driver
// that exits without the reclaim hook leaves the system permanently mute: no panic
// banner, no fault dump, no kprintf. Sec.4.4 of docs/design-m4.6-irq-driver.md; the hook
// is console_on_driver_death, run by exit_current AFTER cap_teardown.
//
// Sequenced by the wire, not by sleeps. The service list is built with
// KICKOS_SIMCON_EXIT_AFTER=1, so the driver serves exactly one message and exits:
//
//   0. kos_print, dropped because the console is USER_OWNED. Its ABSENCE is the
//      anti-vacuity witness: a line on the wire here means the console was never
//      published and every assertion below is meaningless.
//   1. emit() -> the published route, served by the driver, reaches the wire.
//   2. the driver exits; its cap_teardown takes the endpoint's recv_holders to 0, which
//      notes the console death, and exit_current then runs the reclaim.
//   3. a second emit() must fail -KOS_EPIPE. That is what PROVES the driver is gone
//      rather than merely slow, with no timing assumption.
//   4. the SAME kos_print now has to reach the wire. Steps 0 and 4 together are the
//      whole assertion.
//
// Under KICKOS_SIMCON_WINDOW_THREAD the driver is TWO threads: a service thread that
// receives, and a thread that holds the register window and parks in kos_irq_wait. Step 4
// then splits in two, because the reclaim must WAIT for the register holder, not for the
// last receiver.

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/atomic.h>
#include <kickos/sys/cap_index.h>
#include <kickos/sys/emit.h>
#include <kickos/sys/errno.h>

using kickos::emit;

#if defined(KICKOS_SIMCON_WINDOW_THREAD) && KICKOS_SIMCON_WINDOW_THREAD
extern "C" kos_thread_t kickos_simcon_window_thread(void);

namespace
{
    using kickos::Atomic;
    using kickos::Order;

    // The kill gate is PARENTHOOD, so witnessing a refusal needs a live thread this app
    // did NOT spawn. Root spawns everything else in the image, hence a grandchild.
    constexpr uint8_t CAP_FULL =
        static_cast<uint8_t>(KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER);
    constexpr int NEST_DONE = KOS_SPAWN_DELEGATED_CAP0;     // the child's gate back to root
    constexpr int NEST_PARK = KOS_SPAWN_DELEGATED_CAP0 + 1; // what the grandchild waits on
    constexpr int NEST_PROBE = KOS_SPAWN_DELEGATED_CAP0 + 2; // root's gate back to the child

    Atomic<kos_thread_t, Order::RELAXED> g_grandchild{KOS_THREAD_NONE};
    Atomic<int, Order::RELAXED> g_child_kill_rc{1}; // 1 == the child never got that far
    Atomic<int, Order::RELAXED> g_root_kill_rc{1};  // 1 == the child never got that far

    // Root's slot is the FIRST allocation the thread pool ever makes, so it is index 0 at
    // generation 0 on every board and posture, and handle_for(0) is the bare 0. Change that
    // encoding and this case silently names some other thread instead.
    constexpr kos_thread_t ROOT_THREAD = 0;

    void nest_grandchild(void*) // caps: park@1
    {
        kos_sem_wait(KOS_SPAWN_DELEGATED_CAP0); // never posted: alive for the whole matrix
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
        // answers NONE. Issued from a CHILD, because root aiming at itself is -KOS_EINVAL
        // (that is kos_exit's path) and would witness nothing about the parenthood gate.
        g_root_kill_rc = kos_thread_kill(ROOT_THREAD);
        kos_sem_post(NEST_DONE);
        // Root's refuse-half probe needs the grandchild ALIVE, and a cancel reaches a
        // semaphore park, so the accept half below would otherwise race it dead. The gate
        // makes the order explicit instead of resting on cancellation being toothless.
        kos_sem_wait(NEST_PROBE);
        // The accept half of the gate: a spawner may cancel its own child. Root's
        // -KOS_EPERM below is the refuse half.
        kos_thread_t const gc = g_grandchild;
        if (gc != KOS_THREAD_NONE)
        {
            g_child_kill_rc = kos_thread_kill(gc);
        }
        kos_sem_post(NEST_DONE);
        kos_exit(0);
    }

    // Runs while the console is still USER_OWNED, so nothing may be printed here: the
    // results have to be carried out through the reclaimed route.
    void kill_gate_matrix(int* bad_handle_rc, int* big_handle_rc, int* stranger_rc)
    {
        // Both carry the reserved all-ones index (0x7fffffff at an aged generation), so
        // neither is mintable and both must fail to resolve.
        *bad_handle_rc = kos_thread_kill(KOS_THREAD_NONE);
        *big_handle_rc = kos_thread_kill(0x7fffffffu);
        *stranger_rc = 0; // 0 is never a legal answer here, so an unrun matrix fails
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
    }
}
#endif

int main(int, char**)
{
    // Must NOT reach the wire: the console is USER_OWNED, so console_emit drops it.
    kos_print("[drvdeath] kernel console BEFORE death (must NOT reach the wire)\n");

#if defined(KICKOS_SIMCON_WINDOW_THREAD) && KICKOS_SIMCON_WINDOW_THREAD
    int bad_handle_rc = 0;
    int big_handle_rc = 0;
    int stranger_rc = 0;
    kill_gate_matrix(&bad_handle_rc, &big_handle_rc, &stranger_rc);
#endif

    // Served by the driver, which then exits (KICKOS_SIMCON_EXIT_AFTER=1). kos_send
    // is a rendezvous, so this returns only once the driver has taken the message.
    emit("[drvdeath] published route live\n");

    // The driver runs above root and has exited by now, so recv_holders is 0 and the
    // dead-endpoint check refuses this send outright.
    int const rc = kos_send(KOS_CAP_STDOUT, "x", 1);
    if (rc != -KOS_EPIPE)
    {
        // A live receiver means nothing below tests the reclaim. Reported through BOTH
        // routes: which one works is exactly what is in doubt here.
        kos_print("[drvdeath] ERROR: driver still alive after its bounded serve\n");
        emit("[drvdeath] ERROR: driver still alive after its bounded serve\n");
        return 1;
    }

#if defined(KICKOS_SIMCON_WINDOW_THREAD) && KICKOS_SIMCON_WINDOW_THREAD
    // The receiver is gone but the thread owning the UART registers is not, so the console
    // must still be USER_OWNED: reclaiming here would reprogram a live driver's device.
    // Absent on the wire == correct.
    kos_print("[drvdeath] kernel console AFTER death, window HELD "
              "(must NOT reach the wire)\n");

    kos_thread_t const wt = kickos_simcon_window_thread();
    if (wt == KOS_THREAD_NONE)
    {
        kos_print("[drvdeath] ERROR: no window thread handle\n");
        return 2;
    }
    // Cancellation, not destruction: this wakes the thread out of kos_irq_wait with
    // -KOS_ECANCELED and it exits itself. Releasing the window is what finally lets the
    // sticky death note reclaim the console. The thread runs above root, so it has exited
    // by the time this returns.
    int const kill_rc = kos_thread_kill(wt);
    if (kill_rc != 0)
    {
        kos_print("[drvdeath] ERROR: thread_kill of the window thread rc\n");
        return 3;
    }
    // Killing it TWICE must not resolve: the slot is EXITED and the generation only bumps
    // at reclaim, so the resolver has to reject on state, not on generation.
    if (kos_thread_kill(wt) != -KOS_EBADF)
    {
        kos_print("[drvdeath] ERROR: killing an exited thread did not answer EBADF\n");
        return 4;
    }
    if (bad_handle_rc != -KOS_EBADF or big_handle_rc != -KOS_EBADF)
    {
        kos_print("[drvdeath] ERROR: a bogus thread handle was not EBADF\n");
        return 5;
    }
    if (stranger_rc != -KOS_EPERM)
    {
        kos_print("[drvdeath] ERROR: killing a thread this app did not spawn was allowed\n");
        return 6;
    }
    if (g_child_kill_rc != 0)
    {
        kos_print("[drvdeath] ERROR: a spawner could not cancel its own child\n");
        return 7;
    }
    if (g_root_kill_rc != -KOS_EPERM)
    {
        kos_print("[drvdeath] ERROR: root was killable by a thread it spawned\n");
        return 8;
    }
    kos_print("[drvdeath] kill gate: EBADF/EPERM refused, root unkillable, spawner accepted\n");
#endif

    // Identical call to the one dropped above; its presence on the wire is the assertion.
    kos_print("[drvdeath] kernel console AFTER death (reclaimed)\n");
    return 0;
}
