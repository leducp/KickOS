// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/config/system.h>
#include <kickos/reent.h>
#include <kickos/sys/image.h>

#include "kseam_test.h"

#include <stdint.h>

#if not KICKOS_LIBC_REENT or KICKOS_REENT_CARVE != 160 or KICKOS_REENT_CARVE_MAX != 160
#error "this gate's posture is a 160-byte libc state carve, the build's largest"
#endif

namespace
{
    constexpr size_t CELL = KICKOS_REENT_CELL_WORDS * sizeof(void*);
    unsigned char g_shared[KICKOS_REENT_CARVE];

    // One byte wider than the carve leaves below its cell.
    kos_image_header header_with_seam()
    {
        kos_image_header h{};
        h.reent_template = reinterpret_cast<uintptr_t>(g_shared);
        h.reent_width = KICKOS_REENT_CARVE - CELL + 1;
        h.reent_carve = KICKOS_REENT_CARVE;
        return h;
    }

    kos_image_header header_with_carve(uintptr_t carve)
    {
        kos_image_header h{};
        h.reent_template = reinterpret_cast<uintptr_t>(g_shared);
        h.reent_width = 8;
        h.reent_carve = carve;
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

    TEST(ReentCell, the_cell_ends_the_carve_with_zeroed_words_then_the_state_then_the_id)
    {
        alignas(16) uintptr_t block[KICKOS_REENT_CARVE / sizeof(uintptr_t) + 1];
        for (uintptr_t& w : block)
        {
            w = UNWRITTEN;
        }
        unsigned char state[8];
        uint32_t const id = 0x12345u;
        kickos::reent_cell_seat(block, state, id, KICKOS_REENT_CARVE);
        size_t const cell = (KICKOS_REENT_CARVE - CELL) / sizeof(uintptr_t);
        size_t const seat = cell + KICKOS_REENT_CELL_WORDS - 2;
        for (size_t i = cell; i < seat; i++)
        {
            EXPECT_EQ(block[i], 0u) << "the runtime's word " << i - cell << " left unzeroed";
        }
        EXPECT_EQ(block[seat], reinterpret_cast<uintptr_t>(state));
        EXPECT_EQ(block[seat + 1], id);
        for (size_t i = 0; i < sizeof(block) / sizeof(block[0]); i++)
        {
            if (i >= cell and i <= seat + 1)
            {
                continue;
            }
            EXPECT_EQ(block[i], UNWRITTEN) << "word " << i << " is outside the cell";
        }
    }

    TEST(ReentCell, a_state_that_runs_into_the_cell_is_refused_at_boot)
    {
        KICKOS_EXPECT_PANIC(kickos::reent_seam_read(0, header_with_seam()),
                            "libc reentrant state is wider than its image's carve");
    }

    TEST(ReentCell, a_carve_off_the_stack_alignment_or_short_of_the_cell_is_refused_at_boot)
    {
        kos_image_header const off = header_with_carve(KICKOS_REENT_CARVE + 4u);
        KICKOS_EXPECT_PANIC(kickos::reent_seam_read(0, off),
                            "an image's libc state carve is off the stack alignment");
        kos_image_header const short_of_cell = header_with_carve(CELL - KICKOS_STACK_ALIGN);
        KICKOS_EXPECT_PANIC(kickos::reent_seam_read(0, short_of_cell),
                            "an image's libc state carve is off the stack alignment");
    }

    TEST(ReentCell, a_carve_past_the_largest_profile_is_refused_at_boot)
    {
        kos_image_header const wider = header_with_carve(KICKOS_REENT_CARVE + KICKOS_STACK_ALIGN);
        KICKOS_EXPECT_PANIC(kickos::reent_seam_read(0, wider),
                            "an image's libc state carve is larger than every profile's");
        // Added to a TLS block, this wraps to a carve of nothing.
        kos_image_header const wraps
            = header_with_carve(UINTPTR_MAX & ~static_cast<uintptr_t>(KICKOS_STACK_ALIGN - 1u));
        KICKOS_EXPECT_PANIC(kickos::reent_seam_read(0, wraps),
                            "an image's libc state carve is larger than every profile's");
    }

    TEST(ReentCell, a_carve_holding_the_state_and_the_cell_is_admitted)
    {
        kickos::reent_seam_read(0, header_with_carve(KICKOS_REENT_CARVE));
    }
}
