// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A publish's drain over the REAL kernel/init/console.cc, kernel/thread/park.cc and
// kernel/sched/sched.cc: the publisher parks while a kernel chip writer it raced is still inside
// the bracket, and that writer's leave wakes it, or its slay's exit does. The writer runs on a
// context of its own, so it can be stopped inside the bracket and resumed, or never resumed.

#include <ucontext.h>
#include <unistd.h>

#include <string>

#include <kickos/console_tx.h>
#include <kickos/irqlock.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sync.h>
#include <kickos/thread.h>

#include "dark_seam.h"
#include "isolated_arm.h"
#include "kseam_test.h"

extern "C" void kickos_thread_slay_exit(void* arg);

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    constexpr uint8_t PRIO_WRITER = 8;
    constexpr uint8_t PRIO_PUBLISHER = 10;

    // The RTT arm takes an IrqLock of its own ahead of the chip path, one gap more.
#if defined(KICKOS_CONSOLE_RTT) && KICKOS_CONSOLE_RTT
    constexpr uint32_t kGapBracketEntry = 2u;
#else
    constexpr uint32_t kGapBracketEntry = 1u;
#endif

    class ConsoleDrain : public KSeam
    {
    };

    ucontext_t g_main_ctx;
    ucontext_t g_writer_ctx;
    ucontext_t g_waker_ctx;
    alignas(16) char g_writer_stack[256u * 1024u];

    Thread* g_writer = nullptr;
    Thread* g_publisher = nullptr;
    bool g_writer_done = false;
    int g_parks = 0;

    void writer_body()
    {
        kputs("line\n");
        g_writer_done = true;
    }

    // The gap that closes console_emit's bracket entry: the writer is counted and stops there,
    // as an interrupt preempting it would stop it.
    void stop_inside_the_bracket()
    {
        swapcontext(&g_writer_ctx, &g_main_ctx);
    }

    // The writer running and stopped inside the bracket, the publisher not yet seated.
    void stage_writer_inside(GapAction inside, ucontext_t* after = &g_waker_ctx)
    {
        darkseam::reset();
        g_writer_done = false;
        g_parks = 0;
        g_publisher = nullptr;
        g_writer = seat_pool(0, PRIO_WRITER);
        {
            IrqLock lock;
            sched::reschedule(nullptr, lock);
        }
        ASSERT_EQ(sched::current(), g_writer);
        getcontext(&g_writer_ctx);
        g_writer_ctx.uc_stack.ss_sp = g_writer_stack;
        g_writer_ctx.uc_stack.ss_size = sizeof(g_writer_stack);
        g_writer_ctx.uc_link = after;
        makecontext(&g_writer_ctx, writer_body, 0);
        run_in_chunk_gap(inside, kGapBracketEntry);
        swapcontext(&g_main_ctx, &g_writer_ctx);
        ASSERT_FALSE(g_writer_done) << "fixture: the writer never stopped inside the bracket";
        ASSERT_EQ(console_chip_writers(), 1) << "fixture: the writer is not counted";
    }

    // The publisher preempts and runs the publish's console sequence: the handover, the drain,
    // the flip. `killed`: a kill already stands against it.
    void publish(bool killed = false)
    {
        if (g_publisher == nullptr)
        {
            g_publisher = seat_pool(1, PRIO_PUBLISHER);
        }
        if (killed)
        {
            g_publisher->cancel_kind = CANCEL_KILL;
        }
        {
            IrqLock lock;
            sched::reschedule(nullptr, lock);
        }
        ASSERT_EQ(sched::current(), g_publisher);
        {
            IrqLock lock;
            console_handover_begin();
        }
        console_publish_drain();
        console_owner_set_user();
    }

    void expect_published()
    {
        EXPECT_EQ(console_chip_writers(), 0);
        size_t const before = darkseam::g_wire.size();
        kputs("late\n");
        EXPECT_EQ(darkseam::g_wire.size(), before) << "the console was not handed over";
    }

    // The publisher parked: the writer runs on to its leave, which has to wake it.
    void wake_by_the_writers_leave(Thread* parked)
    {
        g_parks = g_parks + 1;
        EXPECT_EQ(parked, g_publisher);
        EXPECT_EQ(parked->wait_kind, WAIT_CONSOLE) << "the publisher did not park on the console";
        EXPECT_EQ(console_owner_is_kernel(), 0);
        EXPECT_EQ(darkseam::g_wire, "") << "the writer ran before the publisher parked";
        swapcontext(&g_waker_ctx, &g_writer_ctx);
        EXPECT_TRUE(g_writer_done);
        EXPECT_EQ(parked->wait_kind, WAIT_NONE) << "the writer's leave did not wake the publisher";
        EXPECT_EQ(parked->wait_result, 0);
    }
}

// The writer counted before the handover keeps the publisher parked on the console until its
// own leave, and that leave wakes it.
TEST_F(ConsoleDrain, the_publisher_parks_until_the_writer_inside_the_bracket_leaves)
{
    run_isolated([]() {
        ASSERT_NO_FATAL_FAILURE(stage_writer_inside(stop_inside_the_bracket));
        wake_next_park(wake_by_the_writers_leave);
        publish();
        EXPECT_EQ(g_parks, 1);
        EXPECT_EQ(darkseam::g_wire, "line\n");
        expect_published();
    });
}

// A panic on another core while the publisher waits: the last writer's leave still wakes it,
// and its last step then hands nothing over.
TEST_F(ConsoleDrain, a_panic_during_the_drain_still_wakes_the_publisher)
{
    run_isolated([]() {
        ASSERT_NO_FATAL_FAILURE(stage_writer_inside(stop_inside_the_bracket));
        wake_next_park([](Thread* parked) {
            g_parks = g_parks + 1;
            kpanic_enter();
            swapcontext(&g_waker_ctx, &g_writer_ctx);
            EXPECT_TRUE(g_writer_done);
            EXPECT_EQ(parked->wait_kind, WAIT_NONE)
                << "the last writer's leave did not wake the publisher after the panic";
        });
        publish();
        EXPECT_EQ(g_parks, 1);
        kputs("after\n");
        EXPECT_EQ(darkseam::g_wire, "line\nafter\n") << "the publish handed a panicked console over";
    });
}

// A kill standing against the publisher does not end the wait: the writer's leave still does,
// and the kill is honoured at the publisher's next entry.
TEST_F(ConsoleDrain, a_killed_publisher_still_waits_for_the_writer)
{
    run_isolated([]() {
        alarm(10u); // a drain that will not park spins: end it red
        ASSERT_NO_FATAL_FAILURE(stage_writer_inside(stop_inside_the_bracket));
        wake_next_park(wake_by_the_writers_leave);
        publish(true);
        EXPECT_EQ(g_parks, 1);
        expect_published();
    });
}

// A writer slain inside the bracket never reaches its leave: its exit leaves the bracket, so the
// drain has nothing to wait for. An interrupt's write nested in that bracket does not end it.
TEST_F(ConsoleDrain, a_writer_slain_inside_the_bracket_leaves_it_with_its_exit)
{
    run_isolated([]() {
        ASSERT_NO_FATAL_FAILURE(stage_writer_inside([]() {
            g_in_isr = true;
            kputs("isr\n");
            g_in_isr = false;
            stop_inside_the_bracket();
        }));
        EXPECT_NE(g_writer->console_writing, 0) << "an interrupt's leave ended the thread's bracket";
        // Live, so the slain writer is not the last thread.
        g_publisher = seat_pool(1, PRIO_PUBLISHER);
        run_noreturn([]() { kickos_thread_slay_exit(nullptr); });
        wake_next_park([](Thread*) { g_parks = g_parks + 1; });
        publish();
        EXPECT_EQ(g_parks, 0) << "the drain waited for a writer that was slain";
        EXPECT_EQ(darkseam::g_wire, "isr\n");
        expect_published();
    });
}

namespace
{
    ucontext_t g_publisher_ctx;
    alignas(16) char g_publisher_stack[256u * 1024u];

    char g_ring[128];

    int ring_slot_free(void)
    {
        return 1;
    }

    void ring_push(uint8_t b)
    {
        darkseam::g_wire.push_back(static_cast<char>(b));
    }

    void ring_gate(void)
    {
    }

    console_tx_backend const kRing = {ring_slot_free, ring_push, ring_gate, ring_gate, nullptr};

    void publisher_body()
    {
        publish();
    }

    // The publisher parked in its drain: a fault record is held for the driver it is handing
    // over to, then the publisher is stopped for good.
    void hold_a_record_then_stop(Thread*)
    {
        g_parks = g_parks + 1;
        kprintf_fault("held record\n");
        krecord_end();
        swapcontext(&g_waker_ctx, &g_main_ctx);
    }
}

// A publisher slain while it waits for a writer never hands the console over: the console is the
// kernel's again, polled, what the hand-off held goes out, the writer finishes and leaves, and the
// next publish goes through.
TEST_F(ConsoleDrain, a_publisher_slain_in_its_drain_leaves_the_console_the_kernels)
{
    run_isolated([]() {
        ASSERT_NO_FATAL_FAILURE(stage_writer_inside(stop_inside_the_bracket, &g_main_ctx));
        console_tx_init(&kRing, g_ring, sizeof(g_ring), -1);
        wake_next_park(hold_a_record_then_stop);
        getcontext(&g_publisher_ctx);
        g_publisher_ctx.uc_stack.ss_sp = g_publisher_stack;
        g_publisher_ctx.uc_stack.ss_size = sizeof(g_publisher_stack);
        g_publisher_ctx.uc_link = &g_main_ctx;
        makecontext(&g_publisher_ctx, publisher_body, 0);
        swapcontext(&g_main_ctx, &g_publisher_ctx);
        ASSERT_EQ(g_parks, 1) << "fixture: the publisher never parked in its drain";
        {
            IrqLock lock;
            thread_cancel_kind(g_publisher, CANCEL_SLAY, lock);
        }
        ASSERT_EQ(sched::current(), g_publisher);
        run_noreturn([]() { kickos_thread_slay_exit(nullptr); });
        EXPECT_EQ(darkseam::g_wire, "held record\n") << "the hand-off's held record was lost";
        swapcontext(&g_main_ctx, &g_writer_ctx);
        ASSERT_TRUE(g_writer_done);
        EXPECT_EQ(console_chip_writers(), 0);
        EXPECT_EQ(console_dark(), 0);
        kputs("after\n");
        EXPECT_EQ(darkseam::g_wire, "held record\nline\nafter\n")
            << "the console stayed handing over with no publisher left";
        g_publisher = seat_pool(2, PRIO_PUBLISHER);
        publish();
        expect_published();
    });
}
