// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/reent.h>
#include <kickos/sched.h>
#include <kickos/thread.h>

#include "kseam_test.h"

#include <stdint.h>

#if not KICKOS_LIBC_REENT or KICKOS_REENT_PER_THREAD or KICKOS_HAVE_ASPACE
#error "this gate's posture is one seated word on a region backend"
#endif

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    constexpr size_t STATE_BYTES = 64;
    unsigned char g_slots[KICKOS_MAX_INSTANCES * KICKOS_THREAD_SLOTS][STATE_BYTES];
    unsigned char g_shared[STATE_BYTES];
    void* g_seat = nullptr;
    uintptr_t g_self = 0;
}

extern "C"
{
    KickosReentSeam const kickos_reent_seam = {
        g_slots,
        g_shared,
        &g_seat,
        &g_self,
        STATE_BYTES,
        STATE_BYTES,
        KICKOS_MAX_INSTANCES * KICKOS_THREAD_SLOTS,
    };
}

namespace
{
    constexpr uint8_t PRIO_LOW = 4;
    constexpr uint8_t PRIO_HIGH = 6;

    class ReentWord : public KSeam
    {
      protected:
        void SetUp() override
        {
            // Before the reset's sched::start, which seats idle through the seam.
            reent_seam_read();
            KSeam::SetUp();
            g_seat = nullptr;
            g_self = 0;
        }

        static Thread* fresh(int slot, uint8_t prio)
        {
            Thread* const t = seat_pool(slot, prio);
            t->reent = g_slots[slot];
            t->reent_fresh = true;
            t->lock_id = kernel().threads.lock_id_for(slot);
            return t;
        }

        static void switch_now()
        {
            IrqLock lock;
            sched::reschedule(nullptr, lock);
        }
    };

    TEST_F(ReentWord, every_switch_in_seats_the_incoming_threads_state_and_id)
    {
        Thread* const a = fresh(0, PRIO_LOW);
        switch_now();
        ASSERT_EQ(sched::current(), a);
        EXPECT_EQ(g_seat, a->reent);
        EXPECT_EQ(g_self, a->lock_id);

        Thread* const b = fresh(1, PRIO_HIGH);
        switch_now();
        ASSERT_EQ(sched::current(), b);
        ASSERT_NE(b->lock_id, a->lock_id);
        EXPECT_EQ(g_seat, b->reent);
        EXPECT_EQ(g_self, b->lock_id);

        run_exit(0);
        ASSERT_EQ(sched::current(), a);
        EXPECT_FALSE(a->reent_fresh);
        EXPECT_EQ(g_seat, a->reent);
        EXPECT_EQ(g_self, a->lock_id) << "a switch back left the outgoing thread's id seated";
    }
}
