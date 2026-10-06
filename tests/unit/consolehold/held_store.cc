// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The held store's edges: several records open at once, its record ids spent and reused, a store
// too full for a line or a marker, one record of several abandoned, the reclaim's write of a
// part-taken record and open ones, a corrupt entry, and a panic landing inside a commit.

#include "held_fixture.h"

#include <string.h>

using namespace heldfix;

namespace
{
    constexpr uint32_t kRing = 512u;
    constexpr char kBanner[] = "\nKERNEL PANIC: banner\n";

    // kickos_held_step runs g_step_fn once, at the g_step_at-th step since the arm began.
    uint32_t g_steps = 0;
    uint32_t g_step_at = 0;
    void (*g_step_fn)(void) = nullptr;

    void as(uintptr_t n)
    {
        heldseam::g_current = thread(n);
    }

    // Bytes no walk of the store may reach: every byte of the block past the ring.
    void poison_past(uint32_t ring)
    {
        memset(consoleseam::storage() + ring, 'Z', consoleseam::STORAGE_SIZE - ring);
    }

    void panic_now(void)
    {
        kpanic_enter();
        kickos::kputs(kBanner);
    }

    // Three records under one driver: one committed, and two open with their lines interleaved.
    void three_records(void)
    {
        as(1);
        kickos::kprintf_fault("a1\n");
        kickos::kprintf_fault("a2\n");
        kickos::krecord_end();
        as(2);
        kickos::kprintf_fault("b1\n");
        as(3);
        kickos::kprintf_fault("c1\n");
        as(2);
        kickos::kprintf_fault("b2\n");
        as(3);
        kickos::kprintf_fault("c2\n");
    }
}

extern "C" void kickos_held_step(void)
{
    g_steps = g_steps + 1u;
    if (g_steps == g_step_at and g_step_fn != nullptr)
    {
        void (*const fn)(void) = g_step_fn;
        g_step_fn = nullptr;
        fn();
    }
}

// Three threads report at once, their lines interleaved: each record is held whole, read in the
// order the records ended.
TEST(ConsoleHeldStore, ThreeRecordsWrittenAtOnceAreEachHeldWhole)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        publish();
        as(1);
        kickos::kprintf_fault(kHead, "t1");
        as(2);
        kickos::kprintf_fault(kHead, "t2");
        as(3);
        kickos::kprintf_fault(kHead, "t3");
        as(1);
        kickos::kprintf_fault(kPc, 0x100u);
        as(3);
        kickos::kprintf_fault(kPc, 0x300u);
        as(2);
        kickos::kprintf_fault(kPc, 0x200u);
        kickos::kprintf_fault(kAddr, 0x201u);
        kickos::krecord_end();
        as(3);
        kickos::kprintf_fault(kAddr, 0x301u);
        kickos::krecord_end();
        as(1);
        kickos::kprintf_fault(kAddr, 0x101u);
        kickos::krecord_end();
        for (std::string const& want : {text("t2", 0x200u), text("t3", 0x300u), text("t1", 0x100u)})
        {
            std::string const got = held();
            EXPECT_EQ(got, want);
            take(got.size());
        }
        EXPECT_EQ(held(), "");
        EXPECT_EQ(consoleseam::pushes_after_commit(), 0u);
    });
}

// Every record id open at once: one more record finds none and is not held, and an id a commit
// frees is spent again.
TEST(ConsoleHeldStore, EveryIdOpenRefusesTheNextRecordUntilACommitFreesOne)
{
    run_isolated([]() {
        consoleseam::reset(2048u);
        publish();
        for (uintptr_t t = 1; t <= 128u; t++)
        {
            as(t);
            kickos::kprintf_fault("x\n");
        }
        as(129);
        kickos::kprintf_fault("lost\n");
        as(1);
        kickos::krecord_end();
        EXPECT_EQ(held(), "x\n");
        take(2u);
        as(129);
        kickos::kprintf_fault("kept\n");
        kickos::krecord_end();
        EXPECT_EQ(held(), "kept\n") << "a record took a line while every id was spent, or its "
                                       "id was not spent again after the commit";
    });
}

// A line the room cannot take marks its record full, and the record drops every later line
// even once a commit has made room for it.
TEST(ConsoleHeldStore, AFullRecordDropsItsLaterLinesWhenRoomComesBack)
{
    run_isolated([]() {
        consoleseam::reset(64u);
        publish();
        std::string const wide = std::string(45u, 'w') + "\n";
        as(2);
        kickos::kprintf_fault("b\n");
        as(1);
        kickos::kprintf_fault("%s", wide.c_str());
        kickos::kprintf_fault("c\n");
        as(2);
        kickos::krecord_end();
        as(1);
        kickos::kprintf_fault("d\n");
        kickos::krecord_end();
        EXPECT_EQ(held(), "b\n");
        take(2u);
        EXPECT_EQ(held(), wide) << "a full record took a line";
    });
}

// No room for a marker while another record fills the store: the new record is not held and
// the full one is untouched.
TEST(ConsoleHeldStore, NoRoomForAMarkerLeavesTheNewRecordUnheld)
{
    run_isolated([]() {
        consoleseam::reset(64u);
        publish();
        std::string const wide = std::string(54u, 'w') + "\n";
        as(1);
        kickos::kprintf_fault("%s", wide.c_str());
        as(2);
        kickos::kprintf_fault("b\n");
        kickos::krecord_end();
        EXPECT_EQ(held(), "");
        as(1);
        kickos::krecord_end();
        EXPECT_EQ(held(), wide);
    });
}

// A record whose every line was dropped leaves no record at its commit, and the next record is
// held as on an empty store.
TEST(ConsoleHeldStore, ARecordOfNoLineLeavesNothingAtItsCommit)
{
    run_isolated([]() {
        consoleseam::reset(32u);
        publish();
        as(1);
        kickos::kprintf_fault("%s", (std::string(40u, 'w') + "\n").c_str());
        kickos::krecord_end();
        EXPECT_EQ(held(), "");
        as(2);
        kickos::kprintf_fault("b\n");
        kickos::krecord_end();
        EXPECT_EQ(held(), "b\n");
    });
}

// One of three open records is abandoned: its lines go, the other two keep theirs.
TEST(ConsoleHeldStore, AbandoningOneOpenRecordKeepsTheOthersWhole)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        publish();
        three_records();
        as(1);
        kickos::kprintf_fault("a3\n");
        as(2);
        kickos::krecord_abandon();
        as(3);
        kickos::krecord_end();
        as(1);
        kickos::krecord_end();
        for (char const* want : {"a1\na2\n", "c1\nc2\n", "a3\n"})
        {
            std::string const got = held();
            EXPECT_EQ(got, want);
            take(got.size());
        }
        EXPECT_EQ(held(), "");
    });
}

// The driver took part of a committed record and its task ended with two records open: the
// reclaim writes the rest of the first from its next line, then each open record whole.
TEST(ConsoleHeldStore, AReclaimWritesAPartTakenRecordThenEachOpenOne)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        publish();
        three_records();
        take(1u);
        task_ends();
        EXPECT_EQ(consoleseam::wire(), "a2\nb1\nb2\nc1\nc2\n");
        as(2);
        kickos::kprintf_fault("b3\n");
        kickos::krecord_end();
        EXPECT_EQ(consoleseam::wire(), "a2\nb1\nb2\nc1\nc2\nb3\n");
        EXPECT_EQ(held(), "");
    });
}

// A store a full record fills goes out whole at the reclaim.
TEST(ConsoleHeldStore, AReclaimWritesAFullStore)
{
    run_isolated([]() {
        consoleseam::reset(64u);
        publish();
        std::string const wide = std::string(52u, 'w') + "\n";
        as(1);
        kickos::kprintf_fault("%s", wide.c_str());
        kickos::kprintf_fault("lost\n");
        task_ends();
        EXPECT_EQ(consoleseam::wire(), wide);
    });
}

// Any byte of the store corrupted, size bytes included: every walk ends inside the ring, so
// nothing past it reaches the wire and nothing hangs or faults.
TEST(ConsoleHeldStore, ACorruptByteNeverLeadsAWalkOffTheRing)
{
    constexpr uint32_t ring = 128u;
    for (uint32_t at = 0; at < 64u; at++)
    {
        for (uint8_t const value : {0x00, 0x01, 0x02, 0x05, 0x06, 0x7F, 0x80, 0xFE, 0xFF})
        {
            static uint32_t s_at = 0;
            static uint8_t s_value = 0;
            s_at = at;
            s_value = value;
            run_isolated([]() {
                consoleseam::reset(ring);
                publish();
                three_records();
                poison_past(ring);
                consoleseam::storage()[s_at] = static_cast<char>(s_value);
                as(2);
                kickos::krecord_end();
                as(3);
                kickos::krecord_abandon();
                as(1);
                kickos::kprintf_fault("a3\n");
                task_ends();
                EXPECT_EQ(consoleseam::wire().find('Z'), std::string::npos)
                    << "byte " << s_at << " set to " << unsigned{s_value}
                    << " led a walk past the ring: " << consoleseam::wire();
            });
            if (::testing::Test::HasFailure())
            {
                return;
            }
        }
    }
}

// A panic lands at every step of a commit, as a fault inside it or another core's panic would:
// nothing faults, and the banner is the first thing on the wire, whole.
TEST(ConsoleHeldStore, APanicAtEveryStepOfACommitWritesTheBannerFirstAndWhole)
{
    uint32_t step = 1;
    while (true)
    {
        g_step_at = step;
        fflush(nullptr);
        pid_t const pid = fork();
        ASSERT_NE(pid, -1);
        if (pid == 0)
        {
            consoleseam::reset(kRing);
            publish();
            three_records();
            as(2);
            g_steps = 0;
            g_step_fn = panic_now;
            kickos::krecord_end();
            if (g_step_fn != nullptr)
            {
                _exit(3);
            }
            EXPECT_EQ(consoleseam::wire(), kBanner) << "a panic at step " << g_step_at;
            fflush(nullptr);
            if (::testing::Test::HasFailure())
            {
                _exit(1);
            }
            _exit(0);
        }
        int status = 0;
        ASSERT_EQ(waitpid(pid, &status, 0), pid);
        ASSERT_TRUE(WIFEXITED(status)) << "a panic at step " << step << " died on a signal";
        if (WEXITSTATUS(status) == 3)
        {
            break;
        }
        ASSERT_EQ(WEXITSTATUS(status), 0) << "see the failure text above";
        step = step + 1u;
    }
    EXPECT_GT(step, 20u) << "the commit took too few steps to have rotated anything";
}

// A device window over the console's registers goes to no thread outside the task the console is
// published for, from the publish to the reclaim; an adjacent window does, and the console's own
// task does.
TEST(ConsoleHeldStore, TheConsoleWindowIsWithheldFromOtherTasksWhilePublished)
{
    run_isolated([]() {
        consoleseam::reset(kRing);
        kickos::Task const* const other = nullptr;
        EXPECT_FALSE(kickos::console_window_withheld(0x40000080u, 0x10u, other))
            << "withheld before any publish";
        publish();
        EXPECT_TRUE(kickos::console_window_withheld(0x40000080u, 0x10u, other));
        EXPECT_TRUE(kickos::console_window_withheld(0x3FFFFF00u, 0x200u, other));
        EXPECT_FALSE(kickos::console_window_withheld(0x40000100u, 0x100u, other));
        heldseam::g_serves_console = true;
        EXPECT_FALSE(kickos::console_window_withheld(0x40000080u, 0x10u, other))
            << "withheld from the console's own task";
        heldseam::g_serves_console = false;
        task_ends();
        EXPECT_FALSE(kickos::console_window_withheld(0x40000080u, 0x10u, other))
            << "withheld after the reclaim";
    });
}
