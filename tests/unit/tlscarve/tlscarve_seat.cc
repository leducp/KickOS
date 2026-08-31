// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The SEATING arch's half of tls_stack_admissible, compiled with TLSCARVE_FROM_SP 0 and
// otherwise identical to the other cases: one clause of configuration apart, so what the
// two report is attributable to that clause and not to a different fabrication.
//
// What it pins is the lifting of the power-of-two stride. Where the thread pointer is a
// register the kernel seats from the context, the block base is computed by SUBTRACTION,
// so the stride is not a constraint and a stack may be any number of pages at any granule
// boundary. That is what lets a stack be frames with a guard page below it instead of a
// naturally aligned power-of-two arena block, and a build that kept the stride refusal
// here would refuse every spawn on such a board at run time rather than at configure.

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>

#include <kickos/config/system.h>
#include <kickos/tls.h>

#include "tlscarve_config.h"

// A minimal non-empty layout: this case reads only whether a block is ADMITTED, so the
// smallest pair that makes tls_block_size() non-zero is all it needs. Adjacent, no gap.
asm(".pushsection .data.kickos_tlscarve,\"aw\",@progbits\n"
    ".balign 16\n"
    ".globl __kickos_tdata_start\n"
    "__kickos_tdata_start:\n"
    ".byte 0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88\n"
    ".globl __kickos_tdata_end\n"
    ".globl __kickos_tbss_start\n"
    "__kickos_tdata_end:\n"
    "__kickos_tbss_start:\n"
    ".zero 8\n"
    ".globl __kickos_tbss_end\n"
    "__kickos_tbss_end:\n"
    ".popsection\n");

static_assert(TLSCARVE_FROM_SP == 0, "this case exists to compile the seating half");

TEST(TlsCarveSeat, the_carve_is_non_empty_here)
{
    // Everything below is about a block being admitted DESPITE the stride, so a zero-size
    // carve would make tls_stack_admissible answer true unconditionally and gate nothing.
    ASSERT_NE(kickos::tls_block_size(), 0u);
    ASSERT_LT(kickos::tls_block_size(), TLSCARVE_STRIDE);
}

TEST(TlsCarveSeat, a_block_that_is_neither_strided_nor_a_power_of_two_is_admitted)
{
    // BOTH clauses of the tax at once. The base carries the ABI's alignment and nothing
    // more, which is what a frame run gives and what the masking arm refuses; and the size is
    // three pages, which is neither one stride nor a power of two. The offset is
    // KICKOS_STACK_ALIGN rather than a page because the fabricated stride here is SMALLER
    // than a page, so a page-aligned base would satisfy it by accident.
    uintptr_t const base = 0x40320000u + KICKOS_STACK_ALIGN;
    ASSERT_NE(base & (TLSCARVE_STRIDE - 1u), 0u);
    ASSERT_EQ(base & (KICKOS_STACK_ALIGN - 1u), 0u);
    size_t const three_pages = 3u * 4096u;
    ASSERT_NE(three_pages, TLSCARVE_STRIDE);
    ASSERT_NE(three_pages & (three_pages - 1u), 0u);
    EXPECT_TRUE(kickos::tls_stack_admissible(base, three_pages));
}

TEST(TlsCarveSeat, the_ABI_alignment_is_still_required)
{
    // The stride goes and the arch's own floor does not: the carve is taken off the base, so
    // a base the ABI cannot seat an sp on stays refused.
    static_assert(KICKOS_STACK_ALIGN > 1, "there would be nothing left to require");
    EXPECT_FALSE(kickos::tls_stack_admissible(0x40321000u + 1u, 3u * 4096u));
}

TEST(TlsCarveSeat, a_block_no_larger_than_the_carve_is_still_refused)
{
    // The one refusal BOTH halves owe: the carve comes off the block, so a block that is not
    // strictly larger than it leaves the thread no stack at all.
    size_t const block = kickos::tls_block_size();
    EXPECT_FALSE(kickos::tls_stack_admissible(0x40321000u, block));
    EXPECT_TRUE(kickos::tls_stack_admissible(0x40321000u, block + KICKOS_STACK_ALIGN));
}
