// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <gtest/gtest.h>

#include <kickos/arch/arch.h>
#include <kickos/cap.h>
#include <kickos/console_tx.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/sys/errno.h>

#include <stdio.h>
#include <stdlib.h>

#include "isolated_arm.h"

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
    // The writing thread.
    kickos::Thread g_writer;
    int g_wakes = 0;

    void note_poke()
    {
        g_pokes = g_pokes + 1;
        if (console_owner_is_kernel() == 0)
        {
            g_pokes_not_owned = g_pokes_not_owned + 1;
        }
        g_poke_writers = console_chip_writers();
    }

    // A console driver create's console sequence, with no writer in flight to drain.
    void publish()
    {
        console_handover_begin();
        console_owner_set_user();
    }
}

extern "C"
{
    arch_irq_state_t arch_irq_save(void)
    {
        if (g_flip_at_mask)
        {
            g_flip_at_mask = false;
            publish();
        }
        return 0;
    }
    void arch_irq_restore(arch_irq_state_t) {}

    int console_tx_insert_line(char const*, size_t n, int)
    {
        note_poke();
        return static_cast<int>(n);
    }
    bool arch_console_write_sync(char const*, size_t)
    {
        note_poke();
        return true;
    }
    void arch_console_reclaim(void) { g_reclaims = g_reclaims + 1; }

    int console_tx_armed(void) { return g_tx_armed; }
    void console_tx_deinit(void) { g_tx_armed = 0; }
}

namespace kickos
{
    bool dev_window_free(uintptr_t, size_t) { return g_window_free; }

    void console_wake(Held)
    {
        g_wakes = g_wakes + 1;
    }

    namespace sched
    {
        Thread* current() { return &g_writer; }
    }
}

namespace
{
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

    // The buffered transport takes the same bracket, so a lost race drops there: the ring
    // producer has no ownership re-check of its own to lean on.
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
            publish();
            console_note_driver_death();
            { kickos::IrqLock lock; console_on_driver_death(); }
            EXPECT_EQ(g_reclaims, 1);
            EXPECT_EQ(console_owner_is_kernel(), 0);
            EXPECT_EQ(g_pokes, 1) << "the reclaim wrote no line of its own on the driver's death";
            kickos::kputs("after");
            EXPECT_EQ(g_pokes, 2) << "RECLAIMED must carry a kernel write on the polled route";
            EXPECT_EQ(g_poke_writers, 1) << "the RECLAIMED poke was not inside the bracket";
        });
    }

    // A console published AGAIN flips straight out of RECLAIMED, where a polled writer would
    // be unbracketed and so invisible to the publish drain.
    TEST(ConsoleOwnership, AReclaimedWriterThatLosesARePublishNeverReachesTheDevice)
    {
        run_isolated([]() {
            publish();
            console_note_driver_death();
            { kickos::IrqLock lock; console_on_driver_death(); }
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
            publish();
            console_note_driver_death();
            g_window_free = false;
            { kickos::IrqLock lock; console_on_driver_death(); }
            EXPECT_EQ(g_reclaims, 0);
            g_window_free = true;
            { kickos::IrqLock lock; console_on_driver_death(); }
            EXPECT_EQ(g_reclaims, 1);
        });
    }

    // A death note names ONE console, so a re-publish must retire it: otherwise the deferred
    // retry reprograms the UART under the NEW driver and leaves the system RECLAIMED.
    TEST(ConsoleOwnership, ARePublishRetiresAStaleDeathNote)
    {
        run_isolated([]() {
            publish(); // the first driver's publish
            console_note_driver_death();
            g_window_free = false; // its IRQ thread still holds the registers
            { kickos::IrqLock lock; console_on_driver_death(); }
            ASSERT_EQ(g_reclaims, 0) << "the reclaim did not defer, so no stale note can exist";

            publish(); // the supervisor publishes a NEW endpoint

            g_window_free = true; // the OLD driver's IRQ thread finally exits
            { kickos::IrqLock lock; console_on_driver_death(); }
            EXPECT_EQ(g_reclaims, 0)
                << "the old driver's death note reclaimed a console the NEW driver owns";
            EXPECT_EQ(console_owner_is_kernel(), 0);

            // RECLAIMED would refuse the published route and put the kernel back on the device.
            kickos::kputs("kernel write under the new driver");
            EXPECT_EQ(g_pokes, 0)
                << "the console left USER_OWNED, so the kernel is back on the new driver's UART";
        });
    }

    // A console published again after a reclaim is the new driver's: a user write is refused,
    // nothing written.
    TEST(ConsoleOwnership, AUserWriteAfterARePublishIsRefused)
    {
        run_isolated([]() {
            publish();
            console_note_driver_death();
            { kickos::IrqLock lock; console_on_driver_death(); }
            ASSERT_EQ(g_reclaims, 1) << "the console never reached RECLAIMED";
            publish(); // the restart's publish
            g_pokes = 0;
            EXPECT_EQ(kickos::kconsole_write_user("line 6\n", 7, true), -KOS_EBUSY);
            EXPECT_EQ(g_pokes, 0);
            EXPECT_EQ(console_chip_writers(), 0);
        });
    }

    // A kernel-owned console takes a user write; from the publish on, every user write is
    // refused, blocking or not, and a kernel line drops.
    TEST(ConsoleOwnership, EveryUserWriteIsRefusedOnceAConsoleIsPublished)
    {
        run_isolated([]() {
            EXPECT_GT(kickos::kconsole_write_user("boot\n", 5, true), 0);
            EXPECT_EQ(g_pokes, 1);
            publish();
            g_pokes = 0;
            EXPECT_EQ(kickos::kconsole_write_user("lost\n", 5, true), -KOS_EBUSY);
            EXPECT_EQ(kickos::kconsole_write_user("lost\n", 5, false), -KOS_EBUSY);
            kickos::kputs("kernel line");
            EXPECT_EQ(g_pokes, 0);
            EXPECT_EQ(console_chip_writers(), 0);
        });
    }

    // The driver's task ended while a thread of it still holds the device, so the reclaim
    // waits: a user write there is refused at once, with no wait, and the device untouched.
    TEST(ConsoleOwnership, AUserWriteBeforeTheReclaimLandsIsRefusedAtOnce)
    {
        run_isolated([]() {
            publish();
            console_note_driver_death();
            g_window_free = false;
            { kickos::IrqLock lock; console_on_driver_death(); }
            ASSERT_EQ(g_reclaims, 0) << "the reclaim did not defer, so no window was opened";
            EXPECT_EQ(kickos::kconsole_write_user("line 6\n", 7, true), -KOS_EBUSY);
            EXPECT_EQ(kickos::kconsole_write_user("line 6\n", 7, false), -KOS_EBUSY);
            EXPECT_EQ(g_pokes, 0);
            EXPECT_EQ(console_chip_writers(), 0);
        });
    }

    // The reclaimed console is the kernel's alone: a kernel line reaches the device, a user
    // write is refused.
    TEST(ConsoleOwnership, TheReclaimedConsoleRefusesAUserWrite)
    {
        run_isolated([]() {
            publish();
            console_note_driver_death();
            { kickos::IrqLock lock; console_on_driver_death(); }
            ASSERT_EQ(g_reclaims, 1) << "the console never reached RECLAIMED";
            g_pokes = 0;
            EXPECT_EQ(kickos::kconsole_write_user("line 6\n", 7, true), -KOS_EBUSY);
            EXPECT_EQ(g_pokes, 0) << "a user write reached the reclaimed device";
            kickos::kputs("kernel line");
            EXPECT_EQ(g_pokes, 1);
            EXPECT_EQ(console_chip_writers(), 0);
        });
    }

    // A publish wakes the writers parked on the console before it, who then find it refused.
    TEST(ConsoleOwnership, APublishWakesTheConsolesWriters)
    {
        run_isolated([]() {
            publish();
            EXPECT_GE(g_wakes, 1) << "a publish did not wake the console's writers";
        });
    }
}
