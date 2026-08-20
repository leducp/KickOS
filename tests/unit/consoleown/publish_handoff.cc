// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kernel/init/console.cc and kernel/init/console_tx.cc in one program, driven through
// kconsole_write, with kos_console_publish's console sequence transcribed below and seated
// in a mask gap of the writer it races.
//
// The line a byte must not cross is the END of the publish drain, not the ownership flip:
// root spawns the driver only once the drain returns, so a bracketed writer poking the UART
// before that is what the protocol is FOR. note_commit marks that line.
//
// Injecting only console_tx_deinit, with no ownership move, is a composition the syscall
// cannot produce; the last arm keeps that injection beside the real one to show what it
// cannot see.

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

#include "publish_seam.h"

namespace
{
    // 31 usable, so a message a single console_emit carries whole still costs the producer
    // several chunks, and a gap ordinal picks which chunk boundary the publish lands on.
    constexpr uint32_t kRing = 32u;
    constexpr size_t kMsg = 100u;

    // Gap 1 closes console_emit's state-read-plus-count bracket, before console_tx_write
    // reads `armed`. Gap 3 is a chunk boundary, two chunks in.
    constexpr uint32_t kGapBeforeRingCheck = 1u;
    constexpr uint32_t kGapMidChunking = 3u;

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
        consolepub::set_isr_runs_in_gap(false); // irq_detach plus the NVIC line mask
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
        consolepub::note_commit();
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
        EXPECT_EQ(consolepub::pushes_after_commit(), 0u)
            << "a kernel byte reached the UART after the publish drain returned";
        size_t const settled = consolepub::wire().size();
        kickos::kputs("late");
        EXPECT_EQ(consolepub::wire().size(), settled)
            << "a kernel write after the handover reached the driver's UART";
        EXPECT_EQ(console_chip_writers(), 0) << "a refused writer was counted";
    }
}

// Premise for every arm below: with no publish in flight, the message reaches the wire
// whole AND the producer really does chunk, so a gap ordinal names a real boundary.
TEST(ConsolePublishHandoff, AChunkedWriteWithNoPublishReachesTheWireWhole)
{
    run_isolated([]() {
        consolepub::reset(kRing);
        std::string const in = message();
        kickos::kconsole_write(in.data(), in.size());
        EXPECT_EQ(consolepub::wire(), in);
        EXPECT_GE(consolepub::gap_count(), 4u) << "the producer did not chunk";
        EXPECT_LE(consolepub::max_masked_pushes(), 1u);
    });
}

// The publish lands at a chunk boundary of a writer already counted in the in-flight
// bracket; that writer's remainder has no buffered path left, so it must go out
// synchronously while the kernel still owns the UART, not vanish.
TEST(ConsolePublishHandoff, AWriterPublishedOverMidChunkingLosesNoBytes)
{
    run_isolated([]() {
        consolepub::reset(kRing);
        consolepub::run_in_gap(kGapMidChunking, publish_lock_held);
        std::string const in = message();
        kickos::kconsole_write(in.data(), in.size());
        ASSERT_TRUE(consolepub::seat_fired()) << "the publish never fired: no race was run";
        publish_tail();
        EXPECT_EQ(consolepub::wire(), in) << "the publish truncated an in-flight message";
        expect_device_quiet_after_the_drain();
    });
}

// The same window one step earlier: the publish lands after console_emit has taken the
// bracket but before console_tx_write reads the ring's arm state, so the WHOLE message is
// the remainder. This one routes through console_emit's own synchronous arm.
TEST(ConsolePublishHandoff, AWriterPublishedOverBeforeTheRingCheckLosesNoBytes)
{
    run_isolated([]() {
        consolepub::reset(kRing);
        consolepub::run_in_gap(kGapBeforeRingCheck, publish_lock_held);
        std::string const in = message();
        kickos::kconsole_write(in.data(), in.size());
        ASSERT_TRUE(consolepub::seat_fired()) << "the publish never fired: no race was run";
        publish_tail();
        EXPECT_EQ(consolepub::wire(), in) << "the publish dropped a whole in-flight message";
        expect_device_quiet_after_the_drain();
    });
}

// What makes "let it finish" safe: the writer the publish rides over was already counted, so
// the drain cannot declare the device free before that writer is off it.
TEST(ConsolePublishHandoff, TheInFlightWriterIsCountedWhenThePublishBegins)
{
    run_isolated([]() {
        consolepub::reset(kRing);
        consolepub::run_in_gap(kGapMidChunking, publish_lock_held);
        std::string const in = message();
        kickos::kconsole_write(in.data(), in.size());
        ASSERT_TRUE(consolepub::seat_fired());
        EXPECT_GT(g_writers_at_handover, 0)
            << "the publish drain was blind to the writer it raced";
        publish_tail();
        EXPECT_EQ(console_chip_writers(), 0);
    });
}

// The drain converges only because nothing increments once the handover has begun. A writer
// arriving after it must be refused outright, so the drain cannot be extended indefinitely.
TEST(ConsolePublishHandoff, AWriterArrivingAfterTheHandoverBeginsIsRefused)
{
    run_isolated([]() {
        consolepub::reset(kRing);
        publish_lock_held();
        std::string const in = message();
        size_t const before = consolepub::wire().size();
        kickos::kconsole_write(in.data(), in.size());
        EXPECT_EQ(consolepub::wire().size(), before) << "a new writer reached the UART";
        EXPECT_EQ(console_chip_writers(), 0) << "a refused writer was counted";
        publish_tail();
        EXPECT_EQ(g_drain_passes, 0) << "a refused writer extended the drain";
    });
}

// The masked-window bound must survive the protocol: the producer's masked span is still one
// ring copy, and the handover path may not push a transmission's worth of bytes with the
// mask held either.
TEST(ConsolePublishHandoff, ThePublishDoesNotWidenTheMaskedWindow)
{
    run_isolated([]() {
        consolepub::reset(kRing);
        consolepub::run_in_gap(kGapMidChunking, publish_lock_held);
        std::string const in = message();
        kickos::kconsole_write(in.data(), in.size());
        ASSERT_TRUE(consolepub::seat_fired());
        publish_tail();
        EXPECT_LE(consolepub::max_masked_pushes(), kRing - 1u);
    });
}

// The ring producer is exported, so a chip that reached it outside console_emit's bracket
// would be invisible to the drain. Its own ownership re-read covers that, and the one state
// it must refuse is a device a driver already has.
TEST(ConsolePublishHandoff, AnUnbracketedProducerRefusesADriverOwnedUart)
{
    run_isolated([]() {
        consolepub::reset(kRing);
        publish_lock_held();
        publish_tail();
        size_t const settled = consolepub::wire().size();
        console_tx_write("unbracketed", 11);
        EXPECT_EQ(consolepub::wire().size(), settled)
            << "the ring producer poked a UART the driver owns";
    });
}

// Relinquishing and handing over in one step leaves the racing writer still counted, and
// that writer would finish its message on the driver's UART, so the handover must not
// complete.
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
        consolepub::reset(kRing);
        consolepub::run_in_gap(kGapMidChunking, publish_without_draining);
        std::string const in = message();
        kickos::kconsole_write(in.data(), in.size());
    });
}

// The blind spot the sequence above closes: injecting console_tx_deinit alone, as the
// consoletx deinit arms do, leaves the ownership state KERNEL_OWNED, so the remainder is
// written and the arm is green whether the protocol exists or not.
namespace
{
    void deinit_only(void)
    {
        console_tx_deinit();
        consolepub::set_isr_runs_in_gap(false);
    }
}

TEST(ConsolePublishHandoff, ADeinitWithNoOwnershipMoveCannotSeeTheTruncation)
{
    run_isolated([]() {
        consolepub::reset(kRing);
        consolepub::run_in_gap(kGapMidChunking, deinit_only);
        std::string const in = message();
        kickos::kconsole_write(in.data(), in.size());
        ASSERT_TRUE(consolepub::seat_fired());
        ASSERT_NE(console_owner_is_kernel(), 0) << "this injection moved the ownership state";
        EXPECT_EQ(consolepub::wire(), in);
    });
}
