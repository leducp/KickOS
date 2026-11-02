// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A writer the ring refuses, end to end over the REAL kernel/init/console.cc, console_tx.cc and
// park.cc: on a backend with no TX interrupt it drains the ring itself, and on one whose TX
// interrupt drains it, it sleeps until that drain frees the room.

#include <string.h>

#include <stdio.h>

#include <string>

#include <kickos/console_tx.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

#include "dark_seam.h"
#include "isolated_arm.h"
#include "kseam_test.h"

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    constexpr uint8_t PRIO_WRITER = 8;
    constexpr uint32_t kRing = 64u;
    constexpr int kTxLine = 5;
    constexpr int kNoTxLine = -1;

    class ConsoleRoom : public KSeam
    {
    };

    char g_storage[kRing];
    bool g_slot_free = true;
    // Bytes the device takes before its slot stays full, where nonzero.
    uint32_t g_slot_budget = 0;
    uint32_t g_pushes = 0;
    uint8_t g_park_kind = WAIT_NONE;
    bool g_writer_parked = false;

    int slot_free(void)
    {
        return static_cast<int>(g_slot_free);
    }

    void push(uint8_t b)
    {
        g_pushes = g_pushes + 1;
        darkseam::g_wire.push_back(static_cast<char>(b));
        if (g_slot_budget != 0)
        {
            g_slot_budget = g_slot_budget - 1;
            g_slot_free = g_slot_budget != 0;
        }
    }

    void gate(void)
    {
    }

    console_tx_backend const kBackend = {slot_free, push, gate, gate, nullptr};

    std::string const kQueued(kRing - 8u, 'L');

    // A running writer and a ring whose earlier lines, a lower writer's, fill it but for a few
    // bytes: none has left yet. `preempted`, where given, runs in the gap after the lower
    // writer's insert and before its own drain, which on a backend with no TX interrupt is the
    // only time its bytes sit queued.
    void stage(int irq_line, GapAction preempted = nullptr)
    {
        darkseam::reset();
        g_slot_budget = 0;
        g_pushes = 0;
        g_park_kind = WAIT_NONE;
        g_writer_parked = false;
        g_slot_free = false;
        Thread* const writer = seat_pool(0, PRIO_WRITER);
        attach_caps(writer, KICKOS_SEAM_CAP_WIDTH);
        {
            IrqLock lock;
            sched::reschedule(nullptr, lock);
        }
        EXPECT_EQ(sched::current(), writer);
        console_tx_init(&kBackend, g_storage, kRing, irq_line);
        if (preempted != nullptr)
        {
            run_in_chunk_gap(preempted, 1u);
        }
        EXPECT_EQ(console_tx_insert_line(kQueued.data(), kQueued.size(), 0),
                  static_cast<int>(kQueued.size()));
        if (preempted == nullptr)
        {
            EXPECT_EQ(g_pushes, 0u) << "fixture: a byte left before the writer ran";
        }
        g_slot_free = true;
    }

    std::string const kLine = "high priority line\n";

    constexpr char kRecord[] = "  PC=0x%x\n";
    std::string const kRecordLine = "  PC=0x100\n";
}

// No TX interrupt: only producers drain, and the lower writer is not running. The writer sends
// the bytes ahead of its line itself, then its line, and never waits.
TEST_F(ConsoleRoom, a_writer_the_ring_refuses_drains_it_itself_on_a_producer_drained_backend)
{
    run_isolated([]() {
        stage(kNoTxLine, []() {
            g_slot_free = true;
            wake_next_park([](Thread*) { g_writer_parked = true; });
            EXPECT_EQ(kconsole_write_user(kLine.data(), kLine.size(), true),
                      static_cast<int>(kLine.size()));
        });
        EXPECT_FALSE(g_writer_parked) << "the writer waited for a drain only it could run";
        EXPECT_EQ(darkseam::g_wire, kQueued + kLine);
    });
}

// A TX interrupt drains the ring: the writer sleeps on the console until that drain has freed the
// room its line needs, then the line queues behind the lower writer's.
TEST_F(ConsoleRoom, a_writer_the_ring_refuses_sleeps_until_the_tx_drain_frees_room)
{
    run_isolated([]() {
        stage(kTxLine);
        wake_next_park([](Thread* parked) {
            g_park_kind = parked->wait_kind;
            EXPECT_EQ(darkseam::g_wire, "") << "a byte left before the drain ran";
            EXPECT_FALSE(parked->on_timer) << "a user writer's wait for room has a deadline";
            console_tx_isr();
        });
        EXPECT_EQ(kconsole_write_user(kLine.data(), kLine.size(), true),
                  static_cast<int>(kLine.size()));
        EXPECT_EQ(g_park_kind, WAIT_CONSOLE) << "the writer did not sleep on the console";
        console_tx_isr();
        EXPECT_EQ(darkseam::g_wire, kQueued + kLine);
    });
}

// A task that set O_NONBLOCK is answered at once, nothing taken.
TEST_F(ConsoleRoom, a_non_blocking_writer_the_ring_refuses_is_answered_at_once)
{
    run_isolated([]() {
        stage(kTxLine);
        wake_next_park([](Thread*) {
            g_writer_parked = true;
            console_tx_isr();
        });
        EXPECT_EQ(kconsole_write_user(kLine.data(), kLine.size(), false), -KOS_ETIMEDOUT);
        EXPECT_FALSE(g_writer_parked) << "a non-blocking writer waited";
    });
}

// The writer's own task holds the console's registers: the drain it would wait for may never
// come, so the line is lost to the kernel console at once.
TEST_F(ConsoleRoom, a_writer_whose_own_task_holds_the_uart_does_not_wait)
{
    run_isolated([]() {
        stage(kTxLine);
        darkseam::g_window_free = false;
        wake_next_park([](Thread*) {
            g_writer_parked = true;
            console_tx_isr();
        });
        EXPECT_EQ(kconsole_write_user(kLine.data(), kLine.size(), true),
                  static_cast<int>(kLine.size()));
        EXPECT_FALSE(g_writer_parked) << "the writer waited on a UART its own task holds";
        console_tx_isr();
        EXPECT_EQ(darkseam::g_wire, kQueued);
    });
}

// A fault record's line behind a full ring drained by a TX interrupt: refused, it sleeps on the
// console with interrupts open until the drain frees its room, then queues whole. The polled
// writer is never used.
TEST_F(ConsoleRoom, a_record_line_the_ring_refuses_sleeps_until_the_tx_drain_frees_room)
{
    run_isolated([]() {
        stage(kTxLine);
        uint32_t const need = kprintf_fault(kRecord, 0x100u);
        ASSERT_EQ(need, static_cast<uint32_t>(kRecordLine.size()))
            << "premise: the ring has room for the record line";
        wake_next_park([](Thread* parked) {
            g_park_kind = parked->wait_kind;
            EXPECT_EQ(darkseam::g_wire, "") << "a byte left before the drain ran";
            console_tx_isr();
        });
        EXPECT_TRUE(kconsole_record_wait(need));
        EXPECT_EQ(g_park_kind, WAIT_CONSOLE) << "the record line did not sleep on the console";
        EXPECT_EQ(kprintf_fault(kRecord, 0x100u), 0u);
        console_tx_isr();
        EXPECT_EQ(darkseam::g_wire, kQueued + kRecordLine);
        EXPECT_EQ(darkseam::g_sync_calls, 0u) << "the record line used the polled writer";
    });
}

// No TX interrupt: the record's writer sends the bytes ahead of its line itself, unmasked, and
// never sleeps.
TEST_F(ConsoleRoom, a_record_line_the_ring_refuses_drains_it_itself_on_a_producer_drained_backend)
{
    run_isolated([]() {
        stage(kNoTxLine, []() {
            g_slot_free = true;
            wake_next_park([](Thread*) { g_writer_parked = true; });
            uint32_t const need = kprintf_fault(kRecord, 0x100u);
            ASSERT_NE(need, 0u) << "premise: the ring has room for the record line";
            EXPECT_TRUE(kconsole_record_wait(need));
            EXPECT_EQ(kprintf_fault(kRecord, 0x100u), 0u);
        });
        EXPECT_FALSE(g_writer_parked) << "the record waited for a drain only it could run";
        EXPECT_EQ(darkseam::g_wire, kQueued + kRecordLine);
        EXPECT_EQ(darkseam::g_sync_calls, 0u) << "the record line used the polled writer";
    });
}

// A TX channel that never drains: the stall window passes with no byte leaving the ring, and
// the line is given up so the thread can die.
TEST_F(ConsoleRoom, a_record_line_behind_a_wedged_channel_is_dropped_after_one_stall)
{
    run_isolated([]() {
        stage(kTxLine);
        uint32_t const need = kprintf_fault(kRecord, 0x100u);
        ASSERT_NE(need, 0u);
        wake_next_park([](Thread* parked) {
            g_park_kind = parked->wait_kind;
            EXPECT_TRUE(parked->on_timer) << "the record line parked with no stall deadline";
            IrqLock lock;
            thread_abort_park(parked, -KOS_ETIMEDOUT, lock);
        });
        EXPECT_FALSE(kconsole_record_wait(need)) << "a wedged channel kept the record waiting";
        EXPECT_EQ(g_park_kind, WAIT_CONSOLE);
        EXPECT_EQ(darkseam::g_sync_calls, 0u);
    });
}

// A slow channel: bytes left during the window, if not yet enough of them, so the line is
// offered again.
TEST_F(ConsoleRoom, a_record_line_behind_a_slow_channel_is_offered_again)
{
    run_isolated([]() {
        stage(kTxLine);
        uint32_t const need = kprintf_fault(kRecord, 0x100u);
        ASSERT_NE(need, 0u);
        wake_next_park([](Thread* parked) {
            g_slot_budget = 1u;
            console_tx_isr();
            IrqLock lock;
            thread_abort_park(parked, -KOS_ETIMEDOUT, lock);
        });
        EXPECT_TRUE(kconsole_record_wait(need)) << "a channel still moving gave the line up";
        EXPECT_EQ(darkseam::g_wire, kQueued.substr(0, 1));
    });
}

// The faulting thread's own task holds the console's registers: the drain may never come, so the
// line is dropped without waiting.
TEST_F(ConsoleRoom, a_record_line_whose_own_task_holds_the_uart_is_dropped)
{
    run_isolated([]() {
        stage(kTxLine);
        darkseam::g_window_free = false;
        uint32_t const need = kprintf_fault(kRecord, 0x100u);
        ASSERT_NE(need, 0u);
        wake_next_park([](Thread*) {
            g_writer_parked = true;
            console_tx_isr();
        });
        EXPECT_FALSE(kconsole_record_wait(need));
        EXPECT_FALSE(g_writer_parked) << "the record waited on a UART its own task holds";
    });
}

// A thread cancelled before its record waits gives each line up at once.
TEST_F(ConsoleRoom, a_cancelled_thread_gives_its_record_line_up)
{
    run_isolated([]() {
        stage(kTxLine);
        uint32_t const need = kprintf_fault(kRecord, 0x100u);
        ASSERT_NE(need, 0u);
        ASSERT_TRUE(thread_cancel_escalate(sched::current(), CANCEL_KILL));
        wake_next_park([](Thread*) { g_writer_parked = true; });
        EXPECT_FALSE(kconsole_record_wait(need));
        EXPECT_FALSE(g_writer_parked) << "a cancelled thread waited for the console";
    });
}

// The whole record line through the entry the fault stub calls: refused, it sleeps until the
// drain frees its room, and is offered again once.
TEST_F(ConsoleRoom, a_record_line_through_kprintf_record_waits_and_goes_whole)
{
    run_isolated([]() {
        stage(kTxLine);
        wake_next_park([](Thread* parked) {
            g_park_kind = parked->wait_kind;
            console_tx_isr();
        });
        kprintf_record(kRecord, 0x100u);
        EXPECT_EQ(g_park_kind, WAIT_CONSOLE) << "the record line did not wait for room";
        console_tx_isr();
        EXPECT_EQ(darkseam::g_wire, kQueued + kRecordLine);
        EXPECT_EQ(darkseam::g_sync_calls, 0u);
    });
}

namespace
{
    Thread* g_line_writer = nullptr;
    uint32_t g_record_need = UINT32_MAX;

    // Another thread faults while the writer is between two bytes of its line.
    void a_record_between_two_bytes(void)
    {
        Thread* const dying = seat_pool(1, PRIO_WRITER + 1u);
        kernel().current(kickos_kernel_core()) = dying;
        g_record_need = kprintf_fault("\nrec\n");
        krecord_end();
        kernel().current(kickos_kernel_core()) = g_line_writer;
    }
}

// The ring is not armed, so a line goes out byte by byte. A fault record lands between two bytes
// of another thread's line, ahead of it, and that line goes on after the record.
TEST_F(ConsoleRoom, a_record_goes_ahead_of_a_polled_line_which_goes_on_after_it)
{
    run_isolated([]() {
        darkseam::reset();
        g_line_writer = seat_pool(0, PRIO_WRITER);
        attach_caps(g_line_writer, KICKOS_SEAM_CAP_WIDTH);
        {
            IrqLock lock;
            sched::reschedule(nullptr, lock);
        }
        ASSERT_EQ(sched::current(), g_line_writer);
        std::string const line = "abcdefgh\n";
        run_in_chunk_gap(a_record_between_two_bytes, 6u);
        EXPECT_EQ(kconsole_write_user(line.data(), line.size(), true),
                  static_cast<int>(line.size()));
        EXPECT_EQ(g_record_need, 0u) << "the record waited for the line it goes ahead of";
        std::string const wire = darkseam::g_wire;
        size_t const at = wire.find("\nrec\n");
        ASSERT_NE(at, std::string::npos) << wire;
        EXPECT_GT(at, 0u) << "the record did not land inside the line: " << wire;
        EXPECT_LT(at, line.size() - 1u) << "the record did not land inside the line: " << wire;
        EXPECT_EQ(wire.substr(0, at) + wire.substr(at + 5u), line) << wire;
    });
}
