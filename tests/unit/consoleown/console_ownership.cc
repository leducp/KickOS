// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <gtest/gtest.h>

#include <kickos/arch/arch.h>
#include <kickos/cap.h>
#include <kickos/console_tx.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sys/errno.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
    int g_pokes = 0;           // chip writes that reached the device, either transport
    int g_pokes_not_owned = 0;
    int g_poke_writers = 0;    // in-flight writer count observed by the LAST poke
    int g_reclaims = 0;
    int g_tx_armed = 0;
    bool g_window_free = true;
    // One-shot: the flip lands as a racing writer masks interrupts, the last instant that
    // writer can still be caught.
    bool g_flip_at_mask = false;
    // Whether the writing thread's own stdout send would be taken, and how often it was asked.
    bool g_serves = false;
    int g_serves_asked = 0;
    // Never dereferenced: it only names the writer to the predicate above.
    alignas(16) unsigned char g_writer[64];
    // The dark-window wait: how often a writer entered it, and what it answers. Answering 0
    // stands for the reclaim that ended it, so the wait frees the window and lands it first.
    int g_dark_waits = 0;
    int g_dark_wait_answer = 0;
    int g_pokes_in_wait = -1;
    int g_dark_wakes = 0;

    void note_poke()
    {
        g_pokes = g_pokes + 1;
        if (console_owner_is_kernel() == 0)
        {
            g_pokes_not_owned = g_pokes_not_owned + 1;
        }
        g_poke_writers = console_chip_writers();
    }
}

extern "C"
{
    arch_irq_state_t arch_irq_save(void)
    {
        if (g_flip_at_mask)
        {
            g_flip_at_mask = false;
            console_owner_set_user();
        }
        return 0;
    }
    void arch_irq_restore(arch_irq_state_t) {}
    int arch_in_isr(void) { return 0; }

    int arch_console_write(char const*, size_t) { note_poke(); return 1; }
    void arch_console_write_sync(char const*, size_t) { note_poke(); }
    void arch_console_flush_sync(void) {}
    void arch_console_reclaim(void) { g_reclaims = g_reclaims + 1; }
    void arch_console_reclaim_window(uintptr_t* base, size_t* size)
    {
        *base = 0x40000000u;
        *size = 0x100u;
    }

    int console_tx_armed(void) { return g_tx_armed; }
    int console_tx_insert_record_line(char const*, size_t, int) { return 0; }
    void console_tx_flush_sync(void) {}
    void console_tx_deinit(void) { g_tx_armed = 0; }
    int console_held_append(uint32_t, char const*, uint32_t, uint32_t) { return 0; }
    int console_held_commit(uint32_t) { return 0; }
    void console_held_abandon(uint32_t) {}
    void console_held_write_sync(void) {}
    void console_held_clear(void) {}
    uint32_t console_held_ready(void) { return 0; }
    char const* console_held_data(void) { return nullptr; }
    void console_held_take(uint32_t) {}

    int kvsnprintf(char* buf, size_t size, char const* fmt, va_list ap)
    {
        return vsnprintf(buf, size, fmt, ap);
    }

    void arch_shutdown(int status)
    {
        printf("FIXTURE FAIL: arch_shutdown(%d) ended the arm\n", status);
        exit(1);
    }
    int arch_reboot(void) { return 0; }
    void kfault_terminate(void)
    {
        printf("FIXTURE FAIL: kfault_terminate ended the arm\n");
        exit(1);
    }
    // No stack switch, as on ARCH_SIM: a host thread stack is megabytes and no red-zone
    // class measures one.
    void kickos_panic_stack_enter(char const* msg, char const* file, unsigned line,
                                  uintptr_t top)
    {
        (void)top;
        kickos_panic_report(msg, file, line);
    }
}

namespace kickos
{
    bool dev_window_free(uintptr_t, size_t) { return g_window_free; }
    bool dev_window_held_outside(uintptr_t, size_t, Task const*) { return false; }
    bool task_serves_console(Task const*) { return false; }

    int console_dark_wait(void)
    {
        g_dark_waits = g_dark_waits + 1;
        g_pokes_in_wait = g_pokes;
        if (g_dark_wait_answer == 0)
        {
            g_window_free = true;
            console_on_driver_death();
        }
        return g_dark_wait_answer;
    }

    void console_dark_wake(void)
    {
        g_dark_wakes = g_dark_wakes + 1;
    }

    void cap_console_deliver() {}

    bool cap_console_serves(Thread const* t)
    {
        g_serves_asked = g_serves_asked + 1;
        EXPECT_EQ(static_cast<void const*>(t), static_cast<void const*>(g_writer));
        return g_serves;
    }

    namespace sched
    {
        Thread* current() { return reinterpret_cast<Thread*>(g_writer); }
    }
}

namespace
{
    // The ownership state is a one-way street: nothing returns it to KERNEL_OWNED, and the
    // arms below need it as their starting state, so each arm gets its own process.
    void run_isolated(void (*body)())
    {
        fflush(nullptr);
        pid_t const pid = fork();
        ASSERT_NE(pid, -1) << "fork failed, the arm did not run";
        if (pid == 0)
        {
            body();
            fflush(nullptr);
            if (::testing::Test::HasFailure())
            {
                _exit(1);
            }
            _exit(0);
        }
        int status = 0;
        ASSERT_EQ(waitpid(pid, &status, 0), pid);
        ASSERT_TRUE(WIFEXITED(status)) << "the arm died on a signal";
        ASSERT_EQ(WEXITSTATUS(status), 0) << "see the arm's own failure text above";
    }

    // Anti-vacuity premise for the arms that assert NO poke reached the device.
    TEST(ConsoleOwnership, KernelOwnedWriteReachesTheDeviceAndReleasesTheBracket)
    {
        run_isolated([]() {
            ASSERT_NE(console_owner_is_kernel(), 0) << "the state machine did not start KERNEL_OWNED";
            kickos::kputs("x");
            EXPECT_EQ(g_pokes, 1);
            EXPECT_EQ(g_pokes_not_owned, 0);
            EXPECT_EQ(g_poke_writers, 1) << "the poke was not inside the bracket";
            EXPECT_EQ(console_chip_writers(), 0);
        });
    }

    // Reading the ownership state and taking the writer count must be ONE masked operation:
    // a flip between them leaves the drain reading 0 while this writer is still on its way to
    // a UART the spawned driver owns.
    TEST(ConsoleOwnership, AWriterThatLosesThePublishRaceNeverReachesTheDevice)
    {
        run_isolated([]() {
            g_flip_at_mask = true;
            kickos::kputs("stale");
            EXPECT_FALSE(g_flip_at_mask) << "the injected publish never fired: no race was run";
            EXPECT_EQ(console_owner_is_kernel(), 0);
            EXPECT_EQ(g_pokes_not_owned, 0)
                << "a kernel chip write reached the UART after the publish flip: the state read "
                   "and the writer count are not one operation";
            EXPECT_EQ(g_pokes, 0);
            EXPECT_EQ(console_chip_writers(), 0);
        });
    }

    // The buffered transport takes the same bracket, so a lost race drops there without
    // leaning on console_tx_write's own recheck.
    TEST(ConsoleOwnership, TheBufferedArmDropsTheSameRace)
    {
        run_isolated([]() {
            g_tx_armed = 1;
            g_flip_at_mask = true;
            kickos::kputs("stale");
            EXPECT_FALSE(g_flip_at_mask) << "the injected publish never fired: no race was run";
            EXPECT_EQ(g_pokes, 0);
            EXPECT_EQ(console_chip_writers(), 0);
        });
    }

    // Anti-vacuity for the reclaim arms below: with no re-publish, the note DOES reclaim once
    // the register window is free.
    TEST(ConsoleOwnership, ADeathNoteReclaimsThePublishedConsole)
    {
        run_isolated([]() {
            console_owner_set_user();
            console_note_driver_death();
            console_on_driver_death();
            EXPECT_EQ(g_reclaims, 1);
            EXPECT_EQ(console_owner_is_kernel(), 0);
            kickos::kputs("after");
            EXPECT_EQ(g_pokes, 1) << "RECLAIMED must carry a kernel write on the polled route";
            EXPECT_EQ(g_poke_writers, 1) << "the RECLAIMED poke was not inside the bracket";
        });
    }

    // A console published AGAIN flips straight out of RECLAIMED, where a polled writer would
    // be unbracketed and so invisible to the publish drain.
    TEST(ConsoleOwnership, AReclaimedWriterThatLosesARePublishNeverReachesTheDevice)
    {
        run_isolated([]() {
            console_owner_set_user();
            console_note_driver_death();
            console_on_driver_death();
            ASSERT_EQ(g_reclaims, 1) << "the console never reached RECLAIMED";
            g_pokes = 0;
            g_pokes_not_owned = 0;

            g_flip_at_mask = true; // a supervisor re-publishes as this writer masks
            kickos::kputs("stale");
            EXPECT_FALSE(g_flip_at_mask) << "the injected publish never fired: no race was run";
            EXPECT_EQ(g_pokes, 0)
                << "a polled kernel write reached the UART after the re-publish flip";
            EXPECT_EQ(console_chip_writers(), 0);
        });
    }

    // The deferral that lets a death note outlive a publish: refused while a peer thread
    // holds the register window, retried at that holder's exit.
    TEST(ConsoleOwnership, ARefusedReclaimIsRetriedWhenTheWindowIsReleased)
    {
        run_isolated([]() {
            console_owner_set_user();
            console_note_driver_death();
            g_window_free = false;
            console_on_driver_death();
            EXPECT_EQ(g_reclaims, 0);
            g_window_free = true;
            console_on_driver_death();
            EXPECT_EQ(g_reclaims, 1);
        });
    }

    // A death note names ONE console, so a re-publish must retire it: otherwise the deferred
    // retry reprograms the UART under the NEW driver and leaves the system RECLAIMED.
    TEST(ConsoleOwnership, ARePublishRetiresAStaleDeathNote)
    {
        run_isolated([]() {
            console_owner_set_user(); // the first driver's publish
            console_note_driver_death();
            g_window_free = false; // its IRQ thread still holds the registers
            console_on_driver_death();
            ASSERT_EQ(g_reclaims, 0) << "the reclaim did not defer, so no stale note can exist";

            console_handover_begin(); // the supervisor publishes a NEW endpoint
            console_owner_set_user();

            g_window_free = true; // the OLD driver's IRQ thread finally exits
            console_on_driver_death();
            EXPECT_EQ(g_reclaims, 0)
                << "the old driver's death note reclaimed a console the NEW driver owns";
            EXPECT_EQ(console_owner_is_kernel(), 0);

            // RECLAIMED would refuse the published route and put the kernel back on the device.
            kickos::kputs("kernel write under the new driver");
            EXPECT_EQ(g_pokes, 0)
                << "the console left USER_OWNED, so the kernel is back on the new driver's UART";
        });
    }

    // A writer whose send was refused while the endpoint stood vacated reaches the kernel
    // console only after a restart published it again: the chunk belongs on the endpoint now,
    // and a "taken" answer would lose it.
    TEST(ConsoleOwnership, AFallbackAfterARePublishIsHandedBackToTheServedEndpoint)
    {
        run_isolated([]() {
            console_owner_set_user();
            console_note_driver_death();
            console_on_driver_death();
            ASSERT_EQ(g_reclaims, 1) << "the console never reached RECLAIMED";
            console_owner_set_user(); // the restart's publish
            g_pokes = 0;
            g_serves = true;
            EXPECT_EQ(kickos::kconsole_write_user("line 6\n", 7, true), -KOS_EBUSY);
            EXPECT_EQ(g_serves_asked, 1);
            EXPECT_EQ(g_pokes, 0);
            EXPECT_EQ(console_chip_writers(), 0);
        });
    }

    // Boundaries of the answer above: a writer the endpoint would not take keeps the designed
    // drop, a kernel writer is never asked, and a kernel-owned console takes the bytes.
    TEST(ConsoleOwnership, OnlyAServedUserWriterIsRefused)
    {
        run_isolated([]() {
            EXPECT_GT(kickos::kconsole_write_user("boot\n", 5, true), 0);
            EXPECT_EQ(g_pokes, 1);
            EXPECT_EQ(g_serves_asked, 0) << "a kernel-owned console asked about the endpoint";
            console_owner_set_user();
            g_pokes = 0;
            g_serves = false;
            EXPECT_EQ(kickos::kconsole_write_user("lost\n", 5, true), 5);
            EXPECT_EQ(g_serves_asked, 1);
            g_serves = true;
            kickos::kputs("kernel line");
            EXPECT_EQ(g_serves_asked, 1) << "a kernel write asked about a thread's endpoint";
            EXPECT_EQ(g_pokes, 0);
        });
    }

    // The dark window: the driver's task ended while a thread of it still holds the device, so
    // the console stays published and unserved until that thread exits. A writer waits it out
    // with the device untouched, and its line lands once, after the reclaim.
    TEST(ConsoleOwnership, AWriterInTheDarkWindowWaitsAndWritesOnceTheReclaimLands)
    {
        run_isolated([]() {
            console_owner_set_user();
            console_note_driver_death();
            g_window_free = false;
            console_on_driver_death();
            ASSERT_EQ(g_reclaims, 0) << "the reclaim did not defer, so no window was opened";
            ASSERT_NE(console_dark(), 0);
            g_serves = false;
            EXPECT_EQ(kickos::kconsole_write_user("line 6\n", 7, true), 7);
            EXPECT_EQ(g_dark_waits, 1) << "the writer did not wait out the dark window";
            EXPECT_EQ(g_pokes_in_wait, 0) << "the device was written while a live thread held it";
            EXPECT_EQ(g_reclaims, 1);
            EXPECT_GT(g_pokes, 0) << "the line did not land after the reclaim";
            EXPECT_EQ(console_chip_writers(), 0);
        });
    }

    // The same window for a non-blocking writer: told to try again at once, nothing written.
    TEST(ConsoleOwnership, ANonBlockingWriterInTheDarkWindowIsAnsweredAtOnce)
    {
        run_isolated([]() {
            console_owner_set_user();
            console_note_driver_death();
            g_window_free = false;
            console_on_driver_death();
            g_serves = false;
            EXPECT_EQ(kickos::kconsole_write_user("line 6\n", 7, false), -KOS_EAGAIN);
            EXPECT_EQ(g_dark_waits, 0) << "a non-blocking writer waited";
            EXPECT_EQ(g_pokes, 0);
            EXPECT_EQ(console_chip_writers(), 0);
        });
    }

    // A writer cancelled in the wait leaves it with nothing written.
    TEST(ConsoleOwnership, ACancelledWaitEndsTheWrite)
    {
        run_isolated([]() {
            console_owner_set_user();
            console_note_driver_death();
            g_window_free = false;
            console_on_driver_death();
            g_serves = false;
            g_dark_wait_answer = -KOS_ECANCELED;
            EXPECT_EQ(kickos::kconsole_write_user("line 6\n", 7, true), -KOS_ECANCELED);
            EXPECT_EQ(g_dark_waits, 1);
            EXPECT_EQ(g_pokes, 0);
        });
    }

    // What ends the wait: the reclaim landing, and a publish. A refused reclaim wakes nobody.
    TEST(ConsoleOwnership, TheReclaimAndAPublishWakeTheDarkWindowsWriters)
    {
        run_isolated([]() {
            console_owner_set_user();
            int const after_publish = g_dark_wakes;
            EXPECT_GE(after_publish, 1) << "a publish did not wake the dark window's writers";
            console_note_driver_death();
            g_window_free = false;
            console_on_driver_death();
            EXPECT_EQ(g_dark_wakes, after_publish) << "a refused reclaim woke the writers";
            g_window_free = true;
            console_on_driver_death();
            EXPECT_EQ(g_dark_wakes, after_publish + 1) << "the reclaim did not wake the writers";
            EXPECT_EQ(console_dark(), 0);
        });
    }
}
