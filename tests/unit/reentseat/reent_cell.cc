// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/reent.h>
#include <kickos/sys/image.h>

#include "kseam_test.h"

#include <stdint.h>

#if not KICKOS_LIBC_REENT or KICKOS_REENT_CARVE != 64
#error "this gate's posture is a 64-byte libc state carve"
#endif

namespace
{
    constexpr size_t CELL = 2 * sizeof(void*);
    unsigned char g_shared[KICKOS_REENT_CARVE];

    // One byte wider than the carve leaves below its cell.
    kos_image_header header_with_seam()
    {
        kos_image_header h{};
        h.reent_template = reinterpret_cast<uintptr_t>(g_shared);
        h.reent_width = KICKOS_REENT_CARVE - CELL + 1;
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
    constexpr uintptr_t UNWRITTEN = 0xA5A5A5A5u;

    TEST(ReentCell, the_cell_ends_the_carve_with_the_state_then_the_id)
    {
        alignas(16) uintptr_t block[KICKOS_REENT_CARVE / sizeof(uintptr_t) + 1];
        for (uintptr_t& w : block)
        {
            w = UNWRITTEN;
        }
        unsigned char state[8];
        uint32_t const id = 0x12345u;
        kickos::reent_cell_seat(block, state, id);
        size_t const cell = (KICKOS_REENT_CARVE - CELL) / sizeof(uintptr_t);
        EXPECT_EQ(block[cell], reinterpret_cast<uintptr_t>(state));
        EXPECT_EQ(block[cell + 1], id);
        for (size_t i = 0; i < sizeof(block) / sizeof(block[0]); i++)
        {
            if (i == cell or i == cell + 1)
            {
                continue;
            }
            EXPECT_EQ(block[i], UNWRITTEN) << "word " << i << " is outside the cell";
        }
    }

    TEST(ReentCell, a_state_that_runs_into_the_cell_is_refused_at_boot)
    {
        KICKOS_EXPECT_PANIC(kickos::reent_seam_read(0, header_with_seam()),
                            "libc reentrant state is wider than KICKOS_REENT_CARVE");
    }
}
