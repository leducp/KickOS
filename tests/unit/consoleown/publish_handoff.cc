// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kernel/init/console.cc and kernel/init/console_tx.cc in one program, driven through
// kconsole_write, with kos_console_publish's console sequence transcribed below and seated
// in a mask gap of the writer it races.
//
// The line a byte must not cross is the END of the publish drain, not the ownership flip:
// root spawns the driver only once the drain returns. note_commit marks that line.

#include <gtest/gtest.h>

#include <string>

#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>

#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "console_seam.h"

namespace
{
    // The ring takes the message as one line, so the gap after the insert is the one window in
    // which the line sits queued with its writer still counted.
    constexpr uint32_t kRing = 128u;
    constexpr size_t kMsg = 100u;

    // These races are addressed by mask-gap ORDINAL, so they move with the number of brackets
    // kconsole_write_impl opens before the chip path. The RTT arm takes an IrqLock of its own
    // first and consumes one gap; without this base the ordinals below would land before the
    // writer is admitted at all, where a drop is the designed answer.
#if defined(KICKOS_CONSOLE_RTT) && KICKOS_CONSOLE_RTT
    constexpr uint32_t kGapBase = 1u;
#else
    constexpr uint32_t kGapBase = 0u;
#endif
    // Closes console_emit's state-read-plus-count bracket, before console_tx_insert_line reads
    // `armed`. The second closes the insert, with the line in the ring and its writer counted.
    constexpr uint32_t kGapBeforeRingCheck = kGapBase + 1u;
    constexpr uint32_t kGapAfterInsert = kGapBase + 2u;

    int g_writers_at_handover = -1;
    int g_drain_passes = -1;

    std::string message()
    {
        std::string s;
        s.reserve(kMsg);
        for (size_t i = 0; i < kMsg; i++)
        {
            s.push_back(static_cast<char>('a' + static_cast<char>(i % 26u)));
        }
        return s;
    }

    // KOS_SYS_CONSOLE_PUBLISH, lock-held half: everything the syscall does to the console
    // between installing the cap and releasing IrqLock.
    void publish_lock_held(void)
    {
        g_writers_at_handover = console_chip_writers();
        kickos::IrqLock lock;
        console_handover_begin();
        consoleseam::set_isr_runs_in_gap(false); // irq_detach plus the NVIC line mask
    }

    // The same syscall's tail, run once the publisher is rescheduled. Root may spawn the
    // driver only after this has returned.
    void publish_tail(void)
    {
        g_drain_passes = 0;
        while (console_chip_writers() != 0)
        {
            g_drain_passes = g_drain_passes + 1;
            ASSERT_LT(g_drain_passes, 1000) << "the publish drain did not converge";
        }
        console_owner_set_user();
        consoleseam::note_commit();
    }

    // The ownership state is a one-way street, so each arm gets its own process.
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

    // The seam's kfault_terminate status. An arm that expects the panic path must not accept
    // a plain nonzero exit, which is also what a failed expectation gives.
    constexpr int kPanicExit = 42;

    void run_isolated_expecting_panic(void (*body)())
    {
        fflush(nullptr);
        pid_t const pid = fork();
        ASSERT_NE(pid, -1) << "fork failed, the arm did not run";
        if (pid == 0)
        {
            body();
            fflush(nullptr);
            _exit(0); // the panic path was not taken
        }
        int status = 0;
        ASSERT_EQ(waitpid(pid, &status, 0), pid);
        ASSERT_TRUE(WIFEXITED(status)) << "the arm died on a signal";
        EXPECT_EQ(WEXITSTATUS(status), kPanicExit)
            << "the console was handed over with a writer still counted, and nothing refused it";
    }

    void expect_device_quiet_after_the_drain()
    {
        EXPECT_EQ(consoleseam::pushes_after_commit(), 0u)
            << "a kernel byte reached the UART after the publish drain returned";
        size_t const settled = consoleseam::wire().size();
        kickos::kputs("late");
        EXPECT_EQ(consoleseam::wire().size(), settled)
            << "a kernel write after the handover reached the driver's UART";
        EXPECT_EQ(console_chip_writers(), 0) << "a refused writer was counted";
    }
}

// Anti-vacuity premise for the arms that seat a publish in a gap ordinal: with no publish in
// flight the message reaches the wire whole, through the ring.
TEST(ConsolePublishHandoff, AWriteWithNoPublishReachesTheWireWhole)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        std::string const in = message();
        kickos::kconsole_write(in.data(), in.size());
        EXPECT_EQ(consoleseam::wire(), in);
        EXPECT_GE(consoleseam::gap_count(), kGapAfterInsert) << "a gap the arms below name is missing";
        EXPECT_LE(consoleseam::max_masked_pushes(), 1u);
    });
}

// The publish lands with the writer's line queued in the ring and the writer still counted in
// the in-flight bracket. The handover's flush sends it while the kernel still owns the UART.
TEST(ConsolePublishHandoff, AWriterPublishedOverAfterItsInsertLosesNoBytes)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        consoleseam::set_isr_runs_in_gap(false);
        consoleseam::run_in_gap(kGapAfterInsert, publish_lock_held);
        std::string const in = message();
        kickos::kconsole_write(in.data(), in.size());
        ASSERT_TRUE(consoleseam::seat_fired()) << "the publish never fired: no race was run";
        publish_tail();
        EXPECT_EQ(consoleseam::wire(), in) << "the publish truncated an in-flight message";
        expect_device_quiet_after_the_drain();
    });
}

// The same window one step earlier: the publish lands after console_emit has taken the bracket
// but before the insert reads the ring's arm state, so the WHOLE message goes out through the
// disarmed ring's synchronous writer.
TEST(ConsolePublishHandoff, AWriterPublishedOverBeforeTheRingCheckLosesNoBytes)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        consoleseam::run_in_gap(kGapBeforeRingCheck, publish_lock_held);
        std::string const in = message();
        kickos::kconsole_write(in.data(), in.size());
        ASSERT_TRUE(consoleseam::seat_fired()) << "the publish never fired: no race was run";
        publish_tail();
        EXPECT_EQ(consoleseam::wire(), in) << "the publish dropped a whole in-flight message";
        expect_device_quiet_after_the_drain();
    });
}

// What makes "let it finish" safe: the writer the publish rides over was already counted, so
// the drain cannot declare the device free before that writer is off it.
TEST(ConsolePublishHandoff, TheInFlightWriterIsCountedWhenThePublishBegins)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        consoleseam::run_in_gap(kGapAfterInsert, publish_lock_held);
        std::string const in = message();
        kickos::kconsole_write(in.data(), in.size());
        ASSERT_TRUE(consoleseam::seat_fired());
        EXPECT_GT(g_writers_at_handover, 0)
            << "the publish drain was blind to the writer it raced";
        publish_tail();
        EXPECT_EQ(console_chip_writers(), 0);
    });
}

// The drain converges only because nothing increments once the handover has begun: a writer
// arriving after it must be refused outright rather than extend the drain.
TEST(ConsolePublishHandoff, AWriterArrivingAfterTheHandoverBeginsIsRefused)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        publish_lock_held();
        std::string const in = message();
        size_t const before = consoleseam::wire().size();
        kickos::kconsole_write(in.data(), in.size());
        EXPECT_EQ(consoleseam::wire().size(), before) << "a new writer reached the UART";
        EXPECT_EQ(console_chip_writers(), 0) << "a refused writer was counted";
        publish_tail();
        EXPECT_EQ(g_drain_passes, 0) << "a refused writer extended the drain";
    });
}

// The masked-window bound must survive the protocol: the handover's flush may push no more than
// the ring holds with the mask held.
TEST(ConsolePublishHandoff, ThePublishDoesNotWidenTheMaskedWindow)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        consoleseam::run_in_gap(kGapAfterInsert, publish_lock_held);
        std::string const in = message();
        kickos::kconsole_write(in.data(), in.size());
        ASSERT_TRUE(consoleseam::seat_fired());
        publish_tail();
        EXPECT_LE(consoleseam::max_masked_pushes(), kRing - 1u);
    });
}

// Relinquishing and handing over in one step leaves the racing writer still counted, and that
// writer would finish its message on the driver's UART, so the handover must not complete.
namespace
{
    void publish_without_draining(void)
    {
        kickos::IrqLock lock;
        console_handover_begin();
        console_owner_set_user();
    }
}

TEST(ConsolePublishHandoff, HandingOverWithAWriterStillCountedIsRefused)
{
    run_isolated_expecting_panic([]() {
        consoleseam::reset(kRing);
        consoleseam::run_in_gap(kGapAfterInsert, publish_without_draining);
        std::string const in = message();
        kickos::kconsole_write(in.data(), in.size());
    });
}
