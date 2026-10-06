// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A writer the ring refuses, end to end over the REAL kernel/init/console.cc, console_tx.cc and
// park.cc: on a backend with no TX interrupt it drains the ring itself, and on one whose TX
// interrupt drains it, it sleeps until that drain frees the room.

#include <string.h>

#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include <kickos/console_tx.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/thread.h>

#include <kickos/sys/errno.h>

#include "dark_seam.h"
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
    }

    void gate(void)
    {
    }

    console_tx_backend const kBackend = {slot_free, push, gate, gate};

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

    // A running writer and a ring whose earlier lines, a lower writer's, fill it but for a few
    // bytes: none has left yet.
    std::string stage(int irq_line)
    {
        darkseam::reset();
        darkseam::g_ring_backed = true;
        g_pushes = 0;
        g_park_kind = WAIT_NONE;
        g_writer_parked = false;
        g_slot_free = false;
        Thread* const writer = seat_pool(0, PRIO_WRITER);
        attach_caps(writer, KICKOS_CAP_CHILD_WIDTH);
        {
            IrqLock lock;
            sched::reschedule();
        }
        EXPECT_EQ(sched::current(), writer);
        console_tx_init(&kBackend, g_storage, kRing, irq_line);
        std::string const queued(kRing - 8u, 'L');
        console_tx_write(queued.data(), queued.size());
        EXPECT_EQ(g_pushes, 0u) << "fixture: a byte left before the writer ran";
        g_slot_free = true;
        return queued;
    }

    std::string const kLine = "high priority line\n";
}

// No TX interrupt: only producers drain, and the lower writer is not running. The writer sends
// the bytes ahead of its line itself, then its line, and never waits.
TEST_F(ConsoleRoom, a_writer_the_ring_refuses_drains_it_itself_on_a_producer_drained_backend)
{
    run_isolated([]() {
        std::string const queued = stage(kNoTxLine);
        wake_next_park([](Thread*) { g_writer_parked = true; });
        EXPECT_EQ(kconsole_write_user(kLine.data(), kLine.size(), true),
                  static_cast<int>(kLine.size()));
        EXPECT_FALSE(g_writer_parked) << "the writer waited for a drain only it could run";
        EXPECT_EQ(darkseam::g_wire, queued + kLine);
    });
}

// A TX interrupt drains the ring: the writer sleeps on the console until that drain has freed the
// room its line needs, then the line queues behind the lower writer's.
TEST_F(ConsoleRoom, a_writer_the_ring_refuses_sleeps_until_the_tx_drain_frees_room)
{
    run_isolated([]() {
        std::string const queued = stage(kTxLine);
        wake_next_park([](Thread* parked) {
            g_park_kind = parked->wait_kind;
            EXPECT_EQ(darkseam::g_wire, "") << "a byte left before the drain ran";
            console_tx_isr();
        });
        EXPECT_EQ(kconsole_write_user(kLine.data(), kLine.size(), true),
                  static_cast<int>(kLine.size()));
        EXPECT_EQ(g_park_kind, WAIT_CONSOLE) << "the writer did not sleep on the console";
        console_tx_isr();
        EXPECT_EQ(darkseam::g_wire, queued + kLine);
    });
}

// A task that set O_NONBLOCK is answered at once, nothing taken.
TEST_F(ConsoleRoom, a_non_blocking_writer_the_ring_refuses_is_answered_at_once)
{
    run_isolated([]() {
        (void)stage(kTxLine);
        wake_next_park([](Thread*) {
            g_writer_parked = true;
            console_tx_isr();
        });
        EXPECT_EQ(kconsole_write_user(kLine.data(), kLine.size(), false), 0);
        EXPECT_FALSE(g_writer_parked) << "a non-blocking writer waited";
    });
}

// The writer's own task holds the console's registers: the drain it would wait for may never
// come, so the line is lost to the kernel console at once.
TEST_F(ConsoleRoom, a_writer_whose_own_task_holds_the_uart_does_not_wait)
{
    run_isolated([]() {
        std::string const queued = stage(kTxLine);
        darkseam::g_window_free = false;
        wake_next_park([](Thread*) {
            g_writer_parked = true;
            console_tx_isr();
        });
        EXPECT_EQ(kconsole_write_user(kLine.data(), kLine.size(), true),
                  static_cast<int>(kLine.size()));
        EXPECT_FALSE(g_writer_parked) << "the writer waited on a UART its own task holds";
        console_tx_isr();
        EXPECT_EQ(darkseam::g_wire, queued);
    });
}
