// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <gtest/gtest.h>

#include <kickos/arch/arch.h>
#include <kickos/cap.h>
#include <kickos/console_tx.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>

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
    // writer can still be caught. arch_irq_save is that instant.
    bool g_flip_at_mask = false;

    void note_poke()
    {
        g_pokes = g_pokes + 1;
        if (console_owner_is_kernel() == 0)
        {
            g_pokes_not_owned = g_pokes_not_owned + 1;
        }
        // Recorded whatever the state: a poke the publish cannot see is one it cannot drain.
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

    void arch_console_write(char const*, size_t) { note_poke(); }
    void arch_console_write_sync(char const*, size_t) { note_poke(); }
    void arch_console_flush_sync(void) {}
    void arch_console_reclaim(void) { g_reclaims = g_reclaims + 1; }
    void arch_console_reclaim_window(uintptr_t* base, size_t* size)
    {
        *base = 0x40000000u;
        *size = 0x100u;
    }

    int console_tx_armed(void) { return g_tx_armed; }
    void console_tx_flush_sync(void) {}
    void console_tx_deinit(void) { g_tx_armed = 0; }

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
}

namespace kickos
{
    bool dev_window_free(uintptr_t, size_t) { return g_window_free; }

    int32_t cap_console_deliver(char const*, size_t) { return 0; }

    namespace sched
    {
        Thread* current() { return nullptr; }
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

    // Anti-vacuity premise for every arm below.
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

    // The publish flip lands after this writer has selected the KERNEL_OWNED branch but
    // before it takes the count: the drain then reads 0, root spawns the driver, and the
    // woken writer bit-bangs a UART the driver owns. Reading the state and taking the count
    // as ONE masked operation is what closes that window.
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

    // A console published AGAIN after a reclaim flips straight out of RECLAIMED, where an
    // unbracketed polled writer is invisible to the publish drain and lands on the respawned
    // driver's UART.
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
    // retry reprograms the UART under the NEW driver and leaves the system RECLAIMED, which
    // also refuses the published route for fault records.
    TEST(ConsoleOwnership, ARePublishRetiresAStaleDeathNote)
    {
        run_isolated([]() {
            console_owner_set_user(); // the first driver's publish
            console_note_driver_death();
            g_window_free = false; // its IRQ thread still holds the registers
            console_on_driver_death();
            ASSERT_EQ(g_reclaims, 0) << "the reclaim did not defer, so no stale note can exist";

            console_owner_set_user(); // the supervisor publishes a NEW endpoint

            g_window_free = true; // the OLD driver's IRQ thread finally exits
            console_on_driver_death();
            EXPECT_EQ(g_reclaims, 0)
                << "the old driver's death note reclaimed a console the NEW driver owns";
            EXPECT_EQ(console_owner_is_kernel(), 0);

            // The published route must still be the live one: RECLAIMED would refuse it and
            // put the kernel back on the device.
            kickos::kputs("kernel write under the new driver");
            EXPECT_EQ(g_pokes, 0)
                << "the console left USER_OWNED, so the kernel is back on the new driver's UART";
        });
    }
}
