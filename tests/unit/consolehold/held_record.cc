// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// kernel/init/console.cc and kernel/init/console_tx.cc in one program: a thread-fault record
// printed while a userspace driver owns the console is held whole in the disarmed ring's storage,
// nothing of it reaches the device, a reader never sees a record still being written, and the
// reclaim writes each of its lines exactly once.

#include "held_fixture.h"

#include <kickos/irqlock.h>

using namespace heldfix;

namespace
{
    constexpr uint32_t kRing = 256u;
}

// Premise: with the kernel owning the console the record is an ordinary kernel print, so the
// arms below can tell the published route from a record that went nowhere.
TEST(ConsoleHeld, AKernelOwnedRecordGoesToTheDeviceAndIsNotHeld)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        record("t1", 0x100u);
        console_tx_flush_sync();
        EXPECT_EQ(consoleseam::wire(), text("t1", 0x100u));
        EXPECT_EQ(held(), "");
        EXPECT_EQ(consoleseam::g_deliveries, 0u);
    });
}

// The driver is mid-write, so nobody is parked to take the record. It may not go to the
// device, where the driver's own drain is writing, and it is held whole for the driver.
TEST(ConsoleHeld, ARecordUnderADriverTouchesNoDeviceAndIsHeldWhole)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        publish();
        record("t1", 0x100u);
        EXPECT_EQ(consoleseam::pushes_after_commit(), 0u)
            << "a fault record was written at a device the driver owns";
        EXPECT_EQ(held(), text("t1", 0x100u));
        EXPECT_EQ(consoleseam::g_deliveries, 1u) << "the finished record was not handed on";
    });
}

// A second thread reports while the first is still writing its record. Both are held, each
// whole and in the order they finished, and neither reaches the device.
TEST(ConsoleHeld, TwoRecordsWrittenAtOnceAreBothHeldWhole)
{
    run_isolated([]() {
        consoleseam::reset(kRing * 2u);
        publish();
        consoleseam::g_current = thread(1);
        kickos::kprintf_fault(kHead, "t1");
        consoleseam::g_current = thread(2);
        record("t2", 0x200u);
        consoleseam::g_current = thread(1);
        kickos::kprintf_fault(kPc, 0x100u);
        kickos::kprintf_fault(kAddr, 0x101u);
        kickos::krecord_end();
        EXPECT_EQ(consoleseam::pushes_after_commit(), 0u);
        std::string const first = held();
        EXPECT_EQ(first, text("t2", 0x200u)) << "the record finished first is not read first";
        take(first.size());
        EXPECT_EQ(held(), text("t1", 0x100u));
        EXPECT_EQ(consoleseam::g_deliveries, 2u);
    });
}

// The driver takes every finished record while another is still being written: the open one
// keeps its place and arrives whole.
TEST(ConsoleHeld, TakingEverythingFinishedLeavesTheOpenRecordWhole)
{
    run_isolated([]() {
        consoleseam::reset(kRing * 2u);
        publish();
        record("t1", 0x100u);
        consoleseam::g_current = thread(1);
        kickos::kprintf_fault(kHead, "t2");
        {
            std::string const first = held();
            ASSERT_EQ(first, text("t1", 0x100u));
            kickos::IrqLock lock;
            console_held_take(static_cast<uint32_t>(first.size()));
        }
        kickos::kprintf_fault(kPc, 0x200u);
        kickos::kprintf_fault(kAddr, 0x201u);
        kickos::krecord_end();
        EXPECT_EQ(held(), text("t2", 0x200u));
    });
}

// Two records in a row, the driver taking neither yet: both held, in order, each whole.
TEST(ConsoleHeld, RecordsTheDriverHasNotTakenQueueInOrder)
{
    run_isolated([]() {
        consoleseam::reset(kRing * 2u);
        publish();
        record("t1", 0x100u);
        record("t2", 0x200u);
        std::string all;
        while (true)
        {
            std::string const part = held();
            if (part.empty())
            {
                break;
            }
            EXPECT_TRUE(part == text("t1", 0x100u) or part == text("t2", 0x200u))
                << "one receive carried part of a record or two of them: " << part;
            all += part;
            kickos::IrqLock lock;
            console_held_take(static_cast<uint32_t>(part.size()));
        }
        EXPECT_EQ(all, text("t1", 0x100u) + text("t2", 0x200u));
    });
}

// Sixty-two bytes of room take the banner and the PC line but not the ADDR line: the record
// keeps its whole lines and no cut one.
TEST(ConsoleHeld, ALineTheRoomCannotTakeEndsTheRecord)
{
    run_isolated([]() {
        consoleseam::reset(64u);
        publish();
        record("t1", 0x100u);
        EXPECT_EQ(held(), "\n=== THREAD FAULT === thread 't1' killed\n  PC=0x100\n");
        EXPECT_EQ(consoleseam::pushes_after_commit(), 0u);
    });
}

// The driver died before taking it: the reclaim gives the kernel the device back, and the
// held record goes out there.
TEST(ConsoleHeld, AReclaimWritesWhatTheDriverNeverTook)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        publish();
        record("t1", 0x100u);
        console_note_driver_death();
        console_on_driver_death(kickos::IrqLock());
        EXPECT_EQ(consoleseam::wire(), text("t1", 0x100u));
        EXPECT_EQ(held(), "");
    });
}

// No room for even its first line: nothing is held, nothing reaches the device, and nothing
// is handed on.
TEST(ConsoleHeld, ARecordWithNoRoomForItsFirstLineIsNotHeld)
{
    run_isolated([]() {
        consoleseam::reset(32u);
        publish();
        record("t1", 0x100u);
        EXPECT_EQ(held(), "");
        EXPECT_EQ(consoleseam::pushes_after_commit(), 0u);
    });
}

// A thread slain inside its record never commits it. Its open record is dropped at the slay,
// or every later record would find one still open and none would be held again.
TEST(ConsoleHeld, ARecordItsWriterWasSlainInsideIsDroppedForTheNext)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        publish();
        consoleseam::g_current = thread(1);
        kickos::kprintf_fault(kHead, "t1");
        kickos::krecord_abandon();
        consoleseam::g_current = thread(2);
        record("t2", 0x200u);
        EXPECT_EQ(held(), text("t2", 0x200u));
        EXPECT_EQ(consoleseam::pushes_after_commit(), 0u);
    });
}

// The console's task ends inside a record: the reclaim writes the lines held so far, the later
// ones reach the device as they are printed, and the record's end writes nothing again and
// reclaims nothing again.
TEST(ConsoleHeld, AReclaimInsideARecordWritesEachLineOnce)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        publish();
        kickos::kprintf_fault(kHead, "t1");
        task_ends();
        ASSERT_EQ(consoleseam::g_reclaims, 1u);
        kickos::kprintf_fault(kPc, 0x100u);
        kickos::kprintf_fault(kAddr, 0x101u);
        kickos::krecord_end();
        EXPECT_EQ(consoleseam::wire(), text("t1", 0x100u));
        EXPECT_EQ(consoleseam::g_reclaims, 1u) << "the record's end reclaimed the device again";
        EXPECT_EQ(held(), "");
    });
}

// Another core panics while a record is open. The panic reclaims once and its banner is the last
// thing on the wire: the record's end may neither reclaim under it nor write the record again.
TEST(ConsoleHeld, APanicInsideARecordIsTheLastWriter)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        publish();
        kickos::kprintf_fault(kHead, "t1");
        kpanic_enter();
        kickos::kputs("\nKERNEL PANIC: banner\n");
        kickos::krecord_end();
        EXPECT_EQ(consoleseam::wire(),
                  "\n=== THREAD FAULT === thread 't1' killed\n\nKERNEL PANIC: banner\n");
        EXPECT_EQ(consoleseam::g_reclaims, 1u) << "the record's end reprogrammed the device again";
    });
}

// Another core ends its record after this core's panic stored RECLAIMED and before its reclaim
// sealed the record: that end may neither reclaim under the panic nor lose the record.
TEST(ConsoleHeld, ARecordEndingInsideAnotherCoresPanicReclaimsNothing)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        publish();
        kickos::kprintf_fault(kHead, "t1");
        consoleseam::g_flush_hook = []() { kickos::krecord_end(); };
        kpanic_enter();
        kickos::kputs("\nKERNEL PANIC: banner\n");
        EXPECT_EQ(consoleseam::g_reclaims, 1u) << "the record's end reprogrammed the device again";
        EXPECT_EQ(consoleseam::wire(),
                  "\n=== THREAD FAULT === thread 't1' killed\n\nKERNEL PANIC: banner\n");
    });
}

// The console's task ends while the publish naming it is still handing the device over. The
// death is acted on when the hand-off completes: the kernel takes the console back.
TEST(ConsoleHeld, ATaskEndingDuringTheHandOffIsReclaimedWhenItCompletes)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        {
            kickos::IrqLock lock;
            console_handover_begin();
        }
        task_ends();
        EXPECT_EQ(consoleseam::g_reclaims, 0u) << "the reclaim ran under a hand-off still draining";
        uint32_t const wakes = consoleseam::g_dark_wakes;
        console_owner_set_user();
        EXPECT_EQ(consoleseam::g_reclaims, 1u) << "the death noted in the hand-off was erased";
        EXPECT_GT(consoleseam::g_dark_wakes, wakes);
        EXPECT_EQ(console_dark(), 0);
        kickos::kputs("after\n");
        EXPECT_EQ(consoleseam::wire(), "after\n") << "the console stayed with a dead driver";
    });
}

// The same, with a thread of that task still holding the device: the console is dark from the
// hand-off's end until that thread's exit, which reclaims.
TEST(ConsoleHeld, ATaskEndingDuringTheHandOffWaitsForItsWindow)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        consoleseam::g_window_free = false;
        {
            kickos::IrqLock lock;
            console_handover_begin();
        }
        task_ends();
        console_owner_set_user();
        EXPECT_NE(console_dark(), 0) << "a writer finds a console it can neither use nor wait for";
        EXPECT_EQ(consoleseam::g_reclaims, 0u);
        consoleseam::g_window_free = true;
        {
            kickos::IrqLock lock;
            console_on_driver_death(lock);
        }
        EXPECT_EQ(consoleseam::g_reclaims, 1u);
        EXPECT_EQ(console_dark(), 0);
    });
}

// A record begun while the kernel still owned the console, with a publish landing inside it:
// the lines printed after the publish are held for the driver, and none is lost.
TEST(ConsoleHeld, ARecordAcrossAPublishKeepsItsTail)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        kickos::kprintf_fault(kHead, "t1");
        publish();
        kickos::kprintf_fault(kPc, 0x100u);
        kickos::kprintf_fault(kAddr, 0x101u);
        kickos::krecord_end();
        EXPECT_EQ(consoleseam::wire(), "\n=== THREAD FAULT === thread 't1' killed\n");
        EXPECT_EQ(held(), "  PC=0x100\n  ADDR=0x101\n");
        EXPECT_EQ(consoleseam::g_deliveries, 1u);
    });
}

// The driver took part of a record's first line and died: what the reclaim writes starts at the
// next line, never inside one.
TEST(ConsoleHeld, AReclaimAfterAPartTakenStartsOnALine)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        publish();
        record("t1", 0x100u);
        {
            kickos::IrqLock lock;
            console_held_take(5u);
        }
        task_ends();
        EXPECT_EQ(consoleseam::wire(), "  PC=0x100\n  ADDR=0x101\n");
        EXPECT_EQ(held(), "");
    });
}
