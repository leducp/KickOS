// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Two images over the link's one thread-local segment (<kickos/sys/image.h>): the app's span is
// the whole segment at offset 0, the init's its own .tdata and .tbss at an offset inside it. A
// thread of an image gets a block sized for that image's span, seated from that image's
// template, and a thread pointer below the block by the image's offset, so the offsets the link
// measured from the segment's start land in the thread's own block.

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kickos/config/system.h>
#include <kickos/image.h>
#include <kickos/tls.h>

#include "tlscarve_config.h"

namespace
{
    // The segment as the final script lays it out: the app's 8 bytes of .tdata, the init's 8
    // of .tdata and 8 of .tbss, then the app's 16 of .tbss.
    constexpr size_t APP_TDATA = 8u;
    constexpr size_t INIT_OFFSET = 8u;
    constexpr size_t INIT_TDATA = 8u;
    constexpr size_t INIT_TBSS = 8u;
    constexpr size_t SEGMENT = 40u;
    alignas(16) unsigned char const g_segment[SEGMENT] = {
        0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, // the app's .tdata
        0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, // the init's .tdata
    };

    kos_image_header g_headers[2];

    void seat_records()
    {
        kos_image_header& init = g_headers[0];
        init.tls_template = reinterpret_cast<uintptr_t>(g_segment) + INIT_OFFSET;
        init.tls_offset = INIT_OFFSET;
        init.tls_init = INIT_TDATA;
        init.tls_total = INIT_TDATA + INIT_TBSS;
        kos_image_header& app = g_headers[1];
        app.tls_template = reinterpret_cast<uintptr_t>(g_segment);
        app.tls_offset = 0;
        app.tls_init = APP_TDATA;
        app.tls_total = SEGMENT;
    }

    size_t rounded(size_t span)
    {
        return (TLSCARVE_TCB + span + (KICKOS_STACK_ALIGN - 1u))
               & ~static_cast<size_t>(KICKOS_STACK_ALIGN - 1u);
    }
}

namespace kickos
{
    kos_image_header const& image_record(size_t i)
    {
        return g_headers[i];
    }
}

TEST(TlsCarveImages, each_image_takes_a_block_for_its_own_span)
{
    seat_records();
    EXPECT_EQ(kickos::tls_block_size(0), rounded(INIT_TDATA + INIT_TBSS));
    EXPECT_EQ(kickos::tls_block_size(1), rounded(SEGMENT));
    EXPECT_LT(kickos::tls_block_size(0), kickos::tls_block_size(1));
}

TEST(TlsCarveImages, the_thread_pointer_sits_below_the_block_by_the_image_offset)
{
    seat_records();
    uintptr_t const block = 0x20010040u;
    EXPECT_EQ(kickos::tls_thread_pointer(0, block), block - INIT_OFFSET);
    EXPECT_EQ(kickos::tls_thread_pointer(1, block), block);
}

TEST(TlsCarveImages, an_init_thread_reads_its_own_template_at_its_segment_offsets)
{
    seat_records();
    alignas(16) unsigned char block[64];
    memset(block, 0xEE, sizeof(block));
    kickos::tls_seat(block, 0);
    // A variable at segment offset INIT_OFFSET + k is at thread pointer + TCB + that offset.
    uintptr_t const tp = kickos::tls_thread_pointer(0, reinterpret_cast<uintptr_t>(block));
    unsigned char const* const first = reinterpret_cast<unsigned char const*>(tp) + TLSCARVE_TCB
                                       + INIT_OFFSET;
    for (size_t k = 0; k < INIT_TDATA; ++k)
    {
        EXPECT_EQ(first[k], g_segment[INIT_OFFSET + k]) << "init .tdata byte " << k;
    }
    for (size_t k = INIT_TDATA; k < INIT_TDATA + INIT_TBSS; ++k)
    {
        EXPECT_EQ(first[k], 0u) << "init .tbss byte " << k;
    }
    EXPECT_EQ(block[TLSCARVE_TCB + INIT_TDATA + INIT_TBSS], 0xEEu)
        << "the seat ran past the init's span";
}

TEST(TlsCarveImages, an_app_thread_reads_the_app_template_and_a_zeroed_rest)
{
    seat_records();
    alignas(16) unsigned char block[64];
    memset(block, 0xEE, sizeof(block));
    kickos::tls_seat(block, 1);
    uintptr_t const tp = kickos::tls_thread_pointer(1, reinterpret_cast<uintptr_t>(block));
    unsigned char const* const first = reinterpret_cast<unsigned char const*>(tp) + TLSCARVE_TCB;
    for (size_t k = 0; k < APP_TDATA; ++k)
    {
        EXPECT_EQ(first[k], g_segment[k]) << "app .tdata byte " << k;
    }
    for (size_t k = APP_TDATA; k < SEGMENT; ++k)
    {
        EXPECT_EQ(first[k], 0u) << "segment byte " << k << " past the app's template";
    }
}

// Each image's libc state is its own newlib profile's, as its header states it: a pool thread of
// either image pays that image's state and that image's block.
TEST(TlsCarveImages, each_image_pays_the_libc_state_its_own_header_states)
{
    seat_records();
    constexpr size_t NANO = 144u;
    constexpr size_t FULL = 384u;
    g_headers[0].reent_carve = NANO;
    g_headers[1].reent_carve = FULL;
    EXPECT_EQ(kickos::tls_libc_carve(0), NANO);
    EXPECT_EQ(kickos::tls_libc_carve(1), FULL);
    EXPECT_EQ(kickos::tls_pool_carve_size(0), NANO + kickos::tls_block_size(0));
    EXPECT_EQ(kickos::tls_pool_carve_size(1), FULL + kickos::tls_block_size(1));
    g_headers[0].reent_carve = 0;
    g_headers[1].reent_carve = 0;
}
