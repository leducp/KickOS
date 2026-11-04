// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// thread_create's carve on an arch that seats its thread pointer, with a libc state carve, no
// thread_local and two kernel cores. A pool thread's stack pays the state alone, at its own base,
// a caller-supplied one included; every core's idle pays nothing.

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

static_assert(TLSCARVE_REENT_CARVE != 0, "the libc state is what makes the carve non-zero");
static_assert(TLSCARVE_FROM_SP == 0, "the seating arch, which owes a caller stack no stride");
static_assert(KICKOS_KERNEL_CORES > 1, "a peer core's idle is a TCB under test");

namespace
{
    using kickos::kernel;
    using kickos::Thread;
    using kickos::ThreadPool;

    constexpr size_t IDLE_BYTES = 896;
    // Neither one stride nor a power of two.
    constexpr size_t CALLER_BYTES = 3u * 1024u;

    alignas(TLSCARVE_STRIDE) unsigned char g_block[2 * TLSCARVE_STRIDE];

    // KICKOS_STACK_ALIGN-aligned and never stride-aligned.
    unsigned char* unstrided_base()
    {
        return g_block + KICKOS_STACK_ALIGN;
    }

    Thread const* pool_thread()
    {
        return &kernel().threads.slots()[ThreadPool::ROOT_INDEX + 1];
    }
}

TEST(TlsCarveReent, a_pool_thread_pays_the_libc_state_alone)
{
    EXPECT_EQ(kickos::tls_block_size(0), 0u);
    EXPECT_EQ(kickos::tls_pool_carve_size(0), TLSCARVE_REENT_CARVE);
    EXPECT_EQ(kickos::tls_carve_size(pool_thread()), TLSCARVE_REENT_CARVE);
}

TEST(TlsCarveReent, every_idle_pays_nothing)
{
    EXPECT_EQ(kickos::tls_carve_size(&kernel().idle_tcb), 0u);
    EXPECT_EQ(kickos::tls_carve(&kernel().idle_tcb, unstrided_base(), IDLE_BYTES), nullptr);
    for (Thread const& peer : kernel().idle_tcb_peer)
    {
        EXPECT_EQ(kickos::tls_carve_size(&peer), 0u);
        EXPECT_EQ(kickos::tls_carve(&peer, unstrided_base(), IDLE_BYTES), nullptr);
    }
}

TEST(TlsCarveReent, a_caller_stack_neither_strided_nor_a_power_of_two_is_carved_at_its_base)
{
    ASSERT_NE(reinterpret_cast<uintptr_t>(unstrided_base()) & (TLSCARVE_STRIDE - 1u), 0u);
    ASSERT_NE(CALLER_BYTES & (CALLER_BYTES - 1u), 0u);
    EXPECT_TRUE(kickos::tls_stack_admissible(reinterpret_cast<uintptr_t>(unstrided_base()),
                                             CALLER_BYTES, 0));
    EXPECT_EQ(kickos::tls_carve(pool_thread(), unstrided_base(), CALLER_BYTES), unstrided_base());
}

TEST(TlsCarveReent, a_block_no_larger_than_the_state_is_refused)
{
    uintptr_t const base = reinterpret_cast<uintptr_t>(unstrided_base());
    EXPECT_FALSE(kickos::tls_stack_admissible(base, TLSCARVE_REENT_CARVE, 0));
    EXPECT_TRUE(kickos::tls_stack_admissible(base, TLSCARVE_REENT_CARVE + KICKOS_STACK_ALIGN, 0));
}

TEST(TlsCarveReent, a_base_the_abi_cannot_seat_an_sp_on_is_refused)
{
    unsigned char* const odd = unstrided_base() + 4;
    EXPECT_FALSE(kickos::tls_stack_admissible(reinterpret_cast<uintptr_t>(odd), CALLER_BYTES, 0));
    KICKOS_EXPECT_PANIC(kickos::tls_carve(pool_thread(), odd, CALLER_BYTES),
                        "assert: admissible");
}
