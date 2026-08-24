// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>

#include <kickos/config/system.h>
#include <kickos/tls.h>

#include "tlscarve_config.h"

// The four linker symbols, fabricated at controlled offsets: .tdata of 8 bytes, a 16-byte
// gap, .tbss of 8 bytes. The gap is what the linker may insert to align .tbss above the end
// of .tdata; 16 is the smallest one that survives the round up to KICKOS_STACK_ALIGN, so a
// block sized from the SUM of the two sections lands on a different number here than one
// sized from the SPAN. Writable and initialised, because tls_seat reads the .tdata template
// through memcpy.
asm(".pushsection .data.kickos_tlscarve,\"aw\",@progbits\n"
    ".balign 16\n"
    ".globl __kickos_tdata_start\n"
    "__kickos_tdata_start:\n"
    ".byte 0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88\n"
    ".globl __kickos_tdata_end\n"
    "__kickos_tdata_end:\n"
    ".zero 16\n"
    ".globl __kickos_tbss_start\n"
    "__kickos_tbss_start:\n"
    ".zero 8\n"
    ".globl __kickos_tbss_end\n"
    "__kickos_tbss_end:\n"
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

size_t const k_tdata = 8u;
size_t const k_gap = 16u;
size_t const k_tbss = 8u;
size_t const k_span = k_tdata + k_gap + k_tbss;
size_t const k_sum = k_tdata + k_tbss;

// align_up(TLSCARVE_TCB + 32, 16) == 48, against align_up(TLSCARVE_TCB + 16, 16) == 32 for
// the sum. Stated as literals so the case pins a number and not the formula under test.
size_t const k_block_from_span = 48u;
size_t const k_block_from_sum = 32u;

static_assert(KICKOS_STACK_ALIGN == 16, "the two literals above are that alignment's answer");
static_assert(TLSCARVE_TCB == 8u, "the two literals above are that reserve's answer");

unsigned char const k_template[k_tdata] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};

// The assembler is free to lay a section out other than as written; every case below reads
// the block size the fabricated offsets imply, so a rearranged layout would move the answer
// rather than fail.
void assert_fabricated_layout()
{
    ASSERT_EQ(static_cast<size_t>(__kickos_tdata_end - __kickos_tdata_start), k_tdata);
    ASSERT_EQ(static_cast<size_t>(__kickos_tbss_start - __kickos_tdata_end), k_gap);
    ASSERT_EQ(static_cast<size_t>(__kickos_tbss_end - __kickos_tbss_start), k_tbss);
    ASSERT_EQ(static_cast<size_t>(__kickos_tbss_end - __kickos_tdata_start), k_span);
}

} // namespace

TEST(TlsCarveSpan, block_size_spans_the_linker_pad)
{
    assert_fabricated_layout();
    ASSERT_NE(k_block_from_span, k_block_from_sum);
    EXPECT_EQ(kickos::tls_block_size(), k_block_from_span);
    EXPECT_NE(kickos::tls_block_size(), k_block_from_sum);
}

TEST(TlsCarveSpan, seat_zeroes_the_pad_as_well_as_tbss)
{
    assert_fabricated_layout();
    alignas(16) unsigned char block[64];
    for (size_t i = 0; i < sizeof(block); ++i)
    {
        block[i] = 0xAA;
    }

    kickos::tls_seat(block);

    unsigned char const* const p = block + TLSCARVE_TCB;
    for (size_t i = 0; i < k_tdata; ++i)
    {
        EXPECT_EQ(p[i], k_template[i]) << "template byte " << i;
    }
    // The gap the linker inserted is INSIDE the compiler's offsets, so a thread_local placed
    // after it reads whatever the stack held unless the zeroing runs to the end of the span.
    for (size_t i = k_tdata; i < k_span; ++i)
    {
        EXPECT_EQ(p[i], 0u) << "span byte " << i;
    }
    ASSERT_GT(k_span, k_sum);
}

TEST(TlsCarveSpan, half_stride_stack_is_refused)
{
    uintptr_t const strided = 0x20010000u;
    ASSERT_EQ(strided & (TLSCARVE_STRIDE - 1u), 0u);
    ASSERT_GT(TLSCARVE_STRIDE, kickos::tls_block_size());

    // Two half-stride stacks inside one stride mask to the SAME thread pointer, so both
    // threads would seat their block on one another's and share every thread_local. A
    // half-stride stack is refused at a strided base and at the midpoint alike.
    EXPECT_FALSE(kickos::tls_stack_admissible(strided, TLSCARVE_STRIDE / 2u));
    EXPECT_FALSE(kickos::tls_stack_admissible(strided + TLSCARVE_STRIDE / 2u,
                                              TLSCARVE_STRIDE / 2u));
}

TEST(TlsCarveSpan, one_strided_stride_is_admitted)
{
    uintptr_t const strided = 0x20010000u;
    EXPECT_TRUE(kickos::tls_stack_admissible(strided, TLSCARVE_STRIDE));
}
