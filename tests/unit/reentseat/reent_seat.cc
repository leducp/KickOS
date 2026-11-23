// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "../common/seam_pools.h"

#include <kickos/aspace.h>
#include <kickos/instance.h>
#include <kickos/irqlock.h>
#include <kickos/reent.h>
#include <kickos/sys/image.h>
#include <kickos/sched.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include "aspace_seam.h"
#include "kseam_test.h"

#include <stdint.h>

#if not KICKOS_LIBC_REENT or not KICKOS_HAVE_ASPACE
#error "this gate's posture is a translating backend with the libc reentrant seam on"
#endif

using namespace kickos;
using namespace kickos::testfix;

namespace
{
    int g_write_count = 0;
    int g_copy_count = 0;

    constexpr size_t STATE_BYTES = 32;

    // A thread's carve, its state at the base and the cell the prime leaves alone above it.
    unsigned char g_states[KICKOS_SEAM_THREAD_SLOTS][KICKOS_REENT_CARVE];
    // The kernel's view of the loader's frames of the window, apart from the window's own bytes.
    unsigned char g_alias[STATE_BYTES];
}

extern "C"
{
    unsigned char g_reent_image[1];
    // Image 0's window, all of it its libc state, the loader's frames at its own addresses.
    unsigned char g_reent_data[STATE_BYTES];

    uintptr_t arch_aspace_user_offset(void)
    {
        return 0;
    }

    void* kmemcpy(void* dst, void const* src, size_t n)
    {
        g_copy_count++;
        return __builtin_memcpy(dst, src, n);
    }
}

namespace kickos
{
    kos_image_header header_with_seam()
    {
        kos_image_header h{};
        h.data_start = reinterpret_cast<uintptr_t>(g_reent_data);
        h.data_end = h.data_start + STATE_BYTES;
        h.data_load = h.data_start;
        h.reent_template = h.data_start;
        h.reent_width = STATE_BYTES;
        h.reent_carve = KICKOS_REENT_CARVE;
        return h;
    }

    // Image 0's record the libc state is primed from, and image 1's, which links no libc state
    // and has no frames row.
    kos_image_header const& image_record(size_t image)
    {
        static kos_image_header const h = header_with_seam();
        static kos_image_header const none = {};
        if (image == 1)
        {
            return none;
        }
        return h;
    }

    // The kernel's view of a frame, apart from every address a space names: g_alias for the
    // window's, never dereferenced for any other.
    constexpr uintptr_t ALIAS = 0x1000;
    void* frame_pool_alias(arch_phys_addr_t frame)
    {
        uintptr_t const at = static_cast<uintptr_t>(frame);
        uintptr_t const window = reinterpret_cast<uintptr_t>(g_reent_data);
        if (at >= window and at - window < STATE_BYTES)
        {
            return g_alias + (at - window);
        }
        return reinterpret_cast<void*>(at + ALIAS);
    }

    bool kaccess_to_user(UserOwner, uintptr_t, void const*, size_t)
    {
        g_write_count++;
        return true;
    }

    bool kaccess_word_to_user(UserOwner, uintptr_t, void const*)
    {
        g_write_count++;
        return true;
    }
}

namespace
{
    constexpr uint8_t PRIO_LOW = 4;
    constexpr uint8_t PRIO_MID = 5;
    constexpr uint8_t PRIO_HIGH = 6;

    class ReentSeat : public KSeam
    {
      protected:
        void SetUp() override
        {
            KSeam::SetUp();
            aspace_seam_reset();
            reent_seam_read(0, header_with_seam());
            g_write_count = 0;
        }

        static Thread* fresh(int slot, uint8_t prio, Task* group)
        {
            Thread* const t = seat_pool(slot, prio);
            if (group != nullptr)
            {
                join_task(t, group);
            }
            t->reent = g_states[slot];
            t->lock_id = kernel().threads.lock_id_for(slot);
            return t;
        }

        static void switch_now()
        {
            IrqLock lock;
            sched::reschedule(nullptr, lock);
        }
    };

    // The spaceless thread runs over the member's root, still installed on this core.
    TEST_F(ReentSeat, a_spaceless_switch_in_writes_no_reent_state)
    {
        Task* const group = task(0);
        ASSERT_NE(group, nullptr);
        for (int slot = 0; slot < 2; slot++)
        {
            for (size_t i = 0; i < KICKOS_REENT_CARVE; i++)
            {
                g_states[slot][i] = 0xA5u;
            }
        }
        (void)fresh(0, PRIO_MID, group);
        switch_now();

        Thread* const spaceless = fresh(1, PRIO_HIGH, nullptr);
        g_copy_count = 0;
        switch_now();
        ASSERT_EQ(sched::current(), spaceless) << "the spaceless thread never switched in";
        ASSERT_EQ(installed_on(0), space_of(task_domain(group)))
            << "fixture: the case needs another task's root left installed under it";
        EXPECT_EQ(g_write_count, 0) << "a switch-in wrote reent state into user memory";
        EXPECT_EQ(g_copy_count, 0) << "a switch-in primed reent state";
        for (int slot = 0; slot < 2; slot++)
        {
            for (size_t i = 0; i < KICKOS_REENT_CARVE; i++)
            {
                EXPECT_EQ(g_states[slot][i], 0xA5u) << "slot " << slot << " byte " << i;
            }
        }
    }

    TEST_F(ReentSeat, a_prime_copies_the_state_alone_from_the_kernels_view_of_the_loaders_frames)
    {
        for (size_t i = 0; i < STATE_BYTES; i++)
        {
            g_alias[i] = static_cast<unsigned char>(0x40u + i);
            g_reent_data[i] = 0;
        }
        unsigned char* const carve = g_states[1];
        for (size_t i = 0; i < KICKOS_REENT_CARVE; i++)
        {
            carve[i] = 0xA5u;
        }
        reent_prime(carve, 0);
        for (size_t i = 0; i < STATE_BYTES; i++)
        {
            EXPECT_EQ(carve[i], g_alias[i]) << "byte " << i;
        }
        for (size_t i = STATE_BYTES; i < KICKOS_REENT_CARVE; i++)
        {
            EXPECT_EQ(carve[i], 0xA5u) << "byte " << i << " is past the state";
        }
        EXPECT_EQ(g_write_count, 0) << "a prime reached user memory through the kaccess seam";
    }

    TEST_F(ReentSeat, a_prime_of_an_image_with_no_libc_state_copies_nothing)
    {
        unsigned char* const carve = g_states[1];
        for (size_t i = 0; i < KICKOS_REENT_CARVE; i++)
        {
            carve[i] = 0xA5u;
        }
        g_copy_count = 0;
        reent_prime(carve, 1);
        EXPECT_EQ(g_copy_count, 0) << "a prime copied a state the image does not link";
        for (size_t i = 0; i < KICKOS_REENT_CARVE; i++)
        {
            EXPECT_EQ(carve[i], 0xA5u) << "byte " << i;
        }
    }
}
