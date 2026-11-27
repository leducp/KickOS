// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Console reclaim when the DRIVER DIES. USER_OWNED drops every kernel write, so a driver
// that exits without the reclaim hook leaves the system permanently mute: no panic
// banner, no fault dump, no kprintf. The hook is console_on_driver_death, run by
// exit_current AFTER cap_teardown. The reclaimed console is the kernel's alone: a user write
// there is refused, as it is from the publish on.
//
// The composition names the packaged simcon as stdout, with no restart, and main watches it.
// Sequenced by the wire and the init's death report, not by sleeps. The image is built with
// KICKOS_SIMCON_EXIT_AFTER=1, so the driver serves one message, then exits:
//
//   0. a raw kernel console write, refused because the console is published. Its ABSENCE is
//      the anti-vacuity witness: a line on the wire here means the console was never
//      published and every assertion below is meaningless.
//   1. kos::print() -> the published route, served by the driver, reaches the wire.
//   2. the driver exits, which ends its task: that end notes the console death, and the
//      reclaim runs once no thread of it holds the device.
//   3. the init reports the death on /init/events, and a send is refused -KOS_ECONNREFUSED.
//      That is what PROVES the driver is gone rather than merely slow, with no timing
//      assumption.
//   4. a stdout write and the SAME raw write are refused at once, and neither reaches the wire.
//   5. a faulter started now: the kernel's record of it on the wire is the console back.
//
// Every verdict after the death is main's exit status, no user line reaching the wire then.
//
// Under KICKOS_SIMCON_WINDOW_THREAD the driver is THREE threads: a service thread that
// receives, a window thread at the lowest priority holding the register window, and the
// task's entry. The receiver's exit leaves a live driver with no receiver, where a send parks,
// and a thread faulting then leaves its record held for that driver. The entry's release then
// ends the task while the window is still held: a writer parked across that end is refused, and
// the reclaim writes the record.

#include <kickos/apps/peer_run.h>
#include <kickos/config/priorities.h> // KICKOS_PRIO_MIN
#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/atomic.h>
#include <kickos/sys/cap_index.h>
#include <kickos/sys/emit.h>
#include <kickos/sys/errno.h>

#include <stdlib.h>

namespace
{
    size_t length(char const* s)
    {
        size_t n = 0;
        while (s[n] != '\0')
        {
            n++;
        }
        return n;
    }

    // One raw kernel console write: refused once the console is published.
    int32_t kconsole_raw(char const* s)
    {
        return KICKOS_KCONSOLE_MEASURED(ANSWER, s, length(s));
    }

    bool refused(kickos::WriteResult r, int32_t error)
    {
        return r.sent == 0u and r.error == error;
    }

    char const BLOCKED_LINE[] = "[drvdeath] blocked line, refused at the driver's death\n";

    // Blocks until the init reports the driver's death: bit 0, simcon being the one task main
    // watches.
    bool wait_driver_death()
    {
        uint32_t bits = 0;
        return kos_notify_wait(1u, KOS_NEVER, &bits) == 0 and (bits & 1u) != 0;
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
    constexpr std::chrono::milliseconds WRITER_PARKED{20};
    constexpr std::chrono::seconds WRITER_DONE{2};

    kickos::WriteResult g_blocked = {1u, 0};

    void blocked_writer(void*)
    {
        g_blocked = kickos::stdout_write(BLOCKED_LINE, length(BLOCKED_LINE));
    }

    // The kill gate is PARENTHOOD, so witnessing a refusal needs a live thread this thread
    // did NOT spawn. Main spawns the child, hence a grandchild.
    kos_sem_t g_nest_done = KOS_SEM_INIT(0);  // the child's gate back to main
    kos_sem_t g_nest_park = KOS_SEM_INIT(0);  // what the grandchild and child wait on
    kos_sem_t g_nest_probe = KOS_SEM_INIT(0); // main's gate back to the child

    Atomic<kos_thread_t, Order::RELAXED> g_grandchild{KOS_THREAD_NONE};
    Atomic<int, Order::RELAXED> g_child_kill_rc{1}; // 1 == the child never got that far
    Atomic<uint32_t, Order::RELAXED> g_spin_release{0u};
    // The cancel of a target making no system call, its join, and its second cancel.
    int g_spin_rc[3] = {1, 1, 0};

    void spinner(void*)
    {
        while (g_spin_release == 0u)
        {
        }
        kos_yield(); // its first entry since the kill: its death point
        kos_exit(0);
    }
    Atomic<int, Order::RELAXED> g_root_kill_rc{1};  // 1 == the child never got that far

    // Root, which runs the init, takes the FIRST slot the thread pool ever allocates, so it is
    // index 0 at generation 0 on every board and posture, and handle_for(0) is the bare 0.
    // Change that encoding and this case silently names some other thread instead.
    constexpr kos_thread_t ROOT_THREAD = 0;

    void nest_grandchild(void*)
    {
        kos_sem_wait(&g_nest_park, KOS_NEVER); // never posted: alive until its spawner cancels it
        kos_exit(0);
    }

    void nest_child(void*)
    {
        g_grandchild = kos::thread::create(nest_grandchild, nullptr, "nestgc", 9).id();
        // Root is unkillable: it leaves spawner_tag at KILL_TAG_NONE and kill_tag_of never
        // answers NONE.
        g_root_kill_rc = kos_thread_cancel(ROOT_THREAD, KOS_CANCEL_KILL);
        kos_sem_post(&g_nest_done);
        // Main's refuse-half probe needs the grandchild ALIVE, and a cancel reaches a
        // semaphore park, so the accept half below would otherwise race it dead. The gate
        // makes the order explicit instead of resting on cancellation being toothless.
        kos_sem_wait(&g_nest_probe, KOS_NEVER);
        // The accept half of the gate: a spawner may cancel its own child. Main's
        // -KOS_EPERM below is the refuse half.
        kos_thread_t const gc = g_grandchild;
        if (gc != KOS_THREAD_NONE)
        {
            g_child_kill_rc = kos_thread_cancel(gc, KOS_CANCEL_KILL);
        }
        kos_sem_post(&g_nest_done);
        kos_sem_wait(&g_nest_park, KOS_NEVER); // never posted: alive until main cancels it
        kos_exit(0);
    }

    // Runs while the console is still USER_OWNED, so nothing may be printed here: the
    // results are carried out as main's exit status.
    void kill_gate_matrix(int* bad_handle_rc, int* big_handle_rc, int* stranger_rc,
                          int* child_kill_rc, int* child_twice_rc)
    {
        // Both carry the reserved all-ones index (0x7fffffff at an aged generation), so
        // neither is mintable and both must fail to resolve.
        *bad_handle_rc = kos_thread_cancel(KOS_THREAD_NONE, KOS_CANCEL_KILL);
        *big_handle_rc = kos_thread_cancel(0x7fffffffu, KOS_CANCEL_KILL);
        // 0 is never a legal answer for these, so an unrun matrix fails.
        *stranger_rc = 0;
        *child_kill_rc = 1;
        *child_twice_rc = 0;
        auto const child = kos::thread::create(nest_child, nullptr, "nestch", 9);
        if (not child.valid())
        {
            return;
        }
        kos_sem_wait(&g_nest_done, KOS_NEVER); // the grandchild exists
        kos_thread_t const gc = g_grandchild;
        if (gc != KOS_THREAD_NONE)
        {
            *stranger_rc = kos_thread_cancel(gc, KOS_CANCEL_KILL);
        }
        kos_sem_post(&g_nest_probe); // probed: the child may now cancel it for real
        kos_sem_wait(&g_nest_done, KOS_NEVER); // the child has tried its own cancel
        // Cancellation, not destruction: the child wakes out of its park and exits itself. It
        // runs above main, so it has exited by the time this returns. Killing it TWICE must not
        // resolve: the slot is EXITED and the generation only bumps at reclaim, so the
        // resolver has to reject on state, not on generation.
        *child_kill_rc = kos_thread_cancel(child.id(), KOS_CANCEL_KILL);
        *child_twice_rc = kos_thread_cancel(child.id(), KOS_CANCEL_KILL);
        // A kill answers once raised and parks nothing: this target makes no system call until
        // released.
        kos::thread::Handle const spin =
            kos::thread::create(spinner, nullptr, "nestsp", KICKOS_PRIO_MIN);
        if (spin.valid())
        {
            g_spin_rc[0] = spin.cancel(KOS_CANCEL_KILL);
            g_spin_release = 1u;
            g_spin_rc[1] = spin.join_for(WRITER_DONE);
            g_spin_rc[2] = spin.cancel(KOS_CANCEL_KILL);
        }
    }
}
#endif

namespace
{
    // The faulter's run: 0 once its task is dead.
    int run_faulter()
    {
        kos_program_t prog = KOS_CAP_NONE;
        int rc = kos_program_find("drvdeath_fault", &prog);
        if (rc != 0)
        {
            return rc;
        }
        kos_task_params fp = {};
        kos_task_t task = KOS_TASK_NONE;
        rc = kickos::apps::create_peer(prog, fp, &task);
        if (rc == 0)
        {
            int status = 0;
            rc = kickos::apps::wait_dead(task, &status, std::chrono::seconds{2});
        }
        (void)kos_handle_close(prog);
        return rc;
    }
}

int main(int, char**)
{
    kos_cap_t const events = kos_grant_notify("/init/events");
    if (kos_notify_bind(events) != 0)
    {
        kos::print("[drvdeath] ERROR: no /init/events to wait the driver's death on\n");
        exit(9);
    }

    int32_t const before_rc =
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

    // Served by the driver, which then exits (KICKOS_SIMCON_EXIT_AFTER=1). kos_send is a
    // rendezvous, so this returns only once the driver has taken the message.
    kos::print("[drvdeath] published route live\n");

#if defined(KICKOS_SIMCON_WINDOW_THREAD) && KICKOS_SIMCON_WINDOW_THREAD
    // The receiver runs above main and has exited by now, and its task lives on: a send would
    // park, so a polling send and a non-blocking write take nothing, at once.
    int32_t const nowait_rc = kos_send(KOS_CAP_STDOUT, "x", 1, KOS_POLL);
    kickos::stdout_nonblock() = 1u;
    kickos::WriteResult const tried = kickos::stdout_write("x", 1);
    kickos::stdout_nonblock() = 0u;
    // A blocking writer parks: it outranks main, so it is in its send when this returns.
    kos::thread::Handle const writer =
        kos::thread::create(blocked_writer, nullptr, "dwriter", WRITER_PRIO);
    int const parked_rc = writer.join_for(WRITER_PARKED);
    // A fault ends its thread's task, so the faulter is a program of its own.
    int const faulted_rc = run_faulter();
    // The receiver is gone but its task is not, so the console is still published.
    int32_t const held_rc = kconsole_raw("[drvdeath] kernel console AFTER death, window HELD "
                                         "(must NOT reach the wire)\n");
    // The entry's exit ends the driver's task while the window thread still holds the
    // registers: the reclaim lands at that thread's exit.
    kickos_simcon_window_release();
#endif

    if (not wait_driver_death())
    {
        exit(2);
    }
    if (kos_send(KOS_CAP_STDOUT, "x", 1, KOS_NEVER) != -KOS_ECONNREFUSED)
    {
        exit(1);
    }

    // The driver is gone and the console the kernel's: refused at once, on neither route.
    char const AFTER_PRINT[] = "[drvdeath] stdout AFTER death (must NOT reach the wire)\n";
    kickos::WriteResult const after_print = kickos::stdout_write(AFTER_PRINT, length(AFTER_PRINT));
    int32_t const after_rc =
        kconsole_raw("[drvdeath] kernel console AFTER death (must NOT reach the wire)\n");
    if (before_rc != -KOS_EBUSY)
    {
        exit(15);
    }
    if (not refused(after_print, -KOS_ECONNREFUSED))
    {
        exit(16);
    }
    if (after_rc != -KOS_EBUSY)
    {
        exit(17);
    }

#if defined(KICKOS_SIMCON_WINDOW_THREAD) && KICKOS_SIMCON_WINDOW_THREAD
    if (nowait_rc != -KOS_EAGAIN or tried.sent != 0u or tried.error != -KOS_EAGAIN)
    {
        exit(10);
    }
    if (parked_rc != -KOS_ETIMEDOUT)
    {
        exit(11);
    }
    if (writer.join_for(WRITER_DONE) != 0)
    {
        exit(12);
    }
    if (not refused(g_blocked, -KOS_ECONNREFUSED))
    {
        exit(18);
    }
    if (faulted_rc != 0)
    {
        exit(13);
    }
    if (held_rc != -KOS_EBUSY)
    {
        exit(19);
    }
    if (child_kill_rc != 0)
    {
        exit(3);
    }
    if (g_spin_rc[0] != 0 or g_spin_rc[1] != 0 or g_spin_rc[2] != -KOS_EBADF)
    {
        exit(14);
    }
    if (child_twice_rc != -KOS_EBADF)
    {
        exit(4);
    }
    if (bad_handle_rc != -KOS_EBADF or big_handle_rc != -KOS_EBADF)
    {
        exit(5);
    }
    if (stranger_rc != -KOS_EPERM)
    {
        exit(6);
    }
    if (g_child_kill_rc != 0)
    {
        exit(7);
    }
    if (g_root_kill_rc != -KOS_EPERM)
    {
        exit(8);
    }
#else
    // The kernel's record of a fault now reaches the wire only on the console it took back.
    if (run_faulter() != 0)
    {
        exit(13);
    }
#endif
    exit(0);
}
