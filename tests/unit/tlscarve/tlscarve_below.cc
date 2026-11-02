// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The order of a pool thread's carve where the thread pointer is SP masked and a thread_local is
// declared: the libc state at the stack's base, then the TLS block, whose template is seated past
// the state and leaves the state's bytes as the stack held them.

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>

#include <kickos/config/system.h>
#include <kickos/instance.h>
#include <kickos/tls.h>

#include "kseam_test.h"
#include "tlscarve_config.h"

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

static_assert(TLSCARVE_REENT_CARVE != 0, "the libc state is the carve's first part");
static_assert(TLSCARVE_FROM_SP == 1, "the masking arch, whose thread pointer is the masked base");

namespace
{
    using kickos::kernel;
    using kickos::Thread;
    using kickos::ThreadPool;

    constexpr unsigned char STACK_FILL = 0xAA;
    // align_up(TLSCARVE_TCB + 16, 16).
    constexpr size_t TLS_BLOCK = 32u;

    alignas(TLSCARVE_STRIDE) unsigned char g_strided[TLSCARVE_STRIDE];

    Thread const* pool_thread()
    {
        return &kernel().threads.slots()[ThreadPool::ROOT_INDEX + 1];
    }
}

TEST(TlsCarveBelow, a_pool_thread_pays_the_state_and_the_block)
{
    ASSERT_EQ(kickos::tls_block_size(0), TLS_BLOCK);
    EXPECT_EQ(kickos::tls_pool_carve_size(0), TLSCARVE_REENT_CARVE + TLS_BLOCK);
    EXPECT_LT(kickos::tls_pool_carve_size(0), TLSCARVE_STRIDE);
}

TEST(TlsCarveBelow, the_template_is_seated_past_the_state)
{
    for (unsigned char& b : g_strided)
    {
        b = STACK_FILL;
    }
    EXPECT_EQ(kickos::tls_carve(pool_thread(), g_strided, TLSCARVE_STRIDE), g_strided);
    for (size_t i = 0; i < TLSCARVE_REENT_CARVE + TLSCARVE_TCB; i++)
    {
        EXPECT_EQ(g_strided[i], STACK_FILL) << "byte " << i << " is the state or the TCB";
    }
    unsigned char const* const payload = g_strided + TLSCARVE_REENT_CARVE + TLSCARVE_TCB;
    unsigned char const want[8] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
    for (size_t i = 0; i < sizeof(want); i++)
    {
        EXPECT_EQ(payload[i], want[i]) << "template byte " << i;
    }
    for (size_t i = sizeof(want); i < 16; i++)
    {
        EXPECT_EQ(payload[i], 0u) << ".tbss byte " << i;
    }
}

TEST(TlsCarveBelow, a_half_stride_stack_is_refused)
{
    EXPECT_FALSE(kickos::tls_stack_admissible(reinterpret_cast<uintptr_t>(g_strided),
                                              TLSCARVE_STRIDE / 2u, 0));
}
