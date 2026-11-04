// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "../common/seam_pools.h"

#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/reent.h>
#include <kickos/sys/image.h>
#include <kickos/sched.h>
#include <kickos/task.h>
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
    // Exactly what the carve leaves below its cell, the widest state the descriptor read admits.
    constexpr size_t STATE_BYTES = KICKOS_REENT_CARVE - KICKOS_REENT_CELL_WORDS * sizeof(void*);
    unsigned char g_states[KICKOS_SEAM_THREAD_SLOTS][KICKOS_REENT_CARVE];
    unsigned char g_shared[STATE_BYTES];
    void* g_seat = nullptr;
    // The task every thread here belongs to, which caches its image's seat.
    Task g_task;
}

namespace
{
    kos_image_header header_with_seam()
    {
        kos_image_header h{};
        h.reent_template = reinterpret_cast<uintptr_t>(g_shared);
        h.reent_seat = reinterpret_cast<uintptr_t>(&g_seat);
        h.reent_width = STATE_BYTES;
        h.reent_carve = KICKOS_REENT_CARVE;
        return h;
    }
}

namespace kickos
{
    kos_image_header const& image_record(size_t)
    {
        static kos_image_header const h = header_with_seam();
        return h;
    }
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
            reent_seam_read(0, header_with_seam());
            KSeam::SetUp();
            g_seat = nullptr;
            g_task = Task{};
            g_task.reent_seat = &g_seat;
        }

        static Thread* fresh(int slot, uint8_t prio)
        {
            Thread* const t = seat_pool(slot, prio);
            t->task = &g_task;
            t->reent = g_states[slot];
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

    TEST_F(ReentWord, every_switch_in_seats_the_incoming_threads_state)
    {
        Thread* const a = fresh(0, PRIO_LOW);
        switch_now();
        ASSERT_EQ(sched::current(), a);
        EXPECT_EQ(g_seat, a->reent);

        Thread* const b = fresh(1, PRIO_HIGH);
        switch_now();
        ASSERT_EQ(sched::current(), b);
        EXPECT_EQ(g_seat, b->reent);

        run_exit(0);
        ASSERT_EQ(sched::current(), a);
        EXPECT_FALSE(a->reent_fresh);
        EXPECT_EQ(g_seat, a->reent) << "a switch back left the outgoing thread's state seated";
    }
}
