// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>

#include <kickos/tls.h>

#include "tlscarve_config.h"

// Both sections empty, with .tbss landing BELOW .tdata: with no thread_local anywhere the
// empty .tdata's ALIGN(8) still advances the location counter past where the empty .tbss
// sits, so the span reads as a huge unsigned number and only the two section sizes can
// decide emptiness. Reproduced here at the same four-byte offset a real link produces.
asm(".pushsection .data.kickos_tlscarve,\"aw\",@progbits\n"
    ".balign 16\n"
    ".zero 16\n"
    ".globl __kickos_tdata_start\n"
    "__kickos_tdata_start:\n"
    ".globl __kickos_tdata_end\n"
    "__kickos_tdata_end:\n"
    ".globl __kickos_tbss_start\n"
    ".globl __kickos_tbss_end\n"
    ".set __kickos_tbss_start, __kickos_tdata_start - 4\n"
    ".set __kickos_tbss_end, __kickos_tdata_start - 4\n"
    ".popsection\n");

extern "C"
{
    extern unsigned char __kickos_tdata_start[];
    extern unsigned char __kickos_tdata_end[];
    extern unsigned char __kickos_tbss_start[];
    extern unsigned char __kickos_tbss_end[];
}

namespace
{

void assert_fabricated_layout()
{
    ASSERT_EQ(__kickos_tdata_end - __kickos_tdata_start, 0);
    ASSERT_EQ(__kickos_tbss_end - __kickos_tbss_start, 0);
    ASSERT_EQ(__kickos_tbss_end - __kickos_tdata_start, -4);
}

} // namespace

TEST(TlsCarveEmpty, no_thread_local_carves_nothing)
{
    assert_fabricated_layout();
    EXPECT_EQ(kickos::tls_block_size(), 0u);
}

TEST(TlsCarveEmpty, seat_touches_nothing)
{
    assert_fabricated_layout();
    alignas(16) unsigned char block[64];
    for (size_t i = 0; i < sizeof(block); ++i)
    {
        block[i] = 0xAA;
    }

    kickos::tls_seat(block);

    for (size_t i = 0; i < sizeof(block); ++i)
    {
        EXPECT_EQ(block[i], 0xAAu) << "byte " << i;
    }
}

TEST(TlsCarveEmpty, every_stack_is_admissible)
{
    // Idle's 512-byte unstrided block reaches no thread_local, and with nothing to carve
    // there is no thread pointer to collide on.
    EXPECT_TRUE(kickos::tls_stack_admissible(0x20010200u, 512u));
    EXPECT_TRUE(kickos::tls_stack_admissible(0x20010000u, TLSCARVE_STRIDE));
}
