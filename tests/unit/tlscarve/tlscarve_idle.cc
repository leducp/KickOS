// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// thread_create's carve where the thread pointer is SP masked to a 4096 stride, a thread_local
// is declared and two kernel cores run: every core's idle stack is refused by admission and
// must still be created, uncarved.

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

static_assert(TLSCARVE_FROM_SP == 1, "the masking arch is the one whose idle stacks refuse");
static_assert(KICKOS_KERNEL_CORES > 1, "a peer core's idle is a TCB under test");

namespace
{
    using kickos::kernel;
    using kickos::Thread;
    using kickos::ThreadPool;

    constexpr size_t IDLE_BYTES = 896;

    alignas(TLSCARVE_STRIDE) unsigned char g_strided[TLSCARVE_STRIDE];
    // A peer's idle stack is 16-aligned static storage, so its base is not stride-aligned.
    alignas(TLSCARVE_STRIDE) unsigned char g_unstrided[TLSCARVE_STRIDE];

    unsigned char* idle_base()
    {
        return g_unstrided + KICKOS_STACK_ALIGN;
    }

    void expect_uncarved(Thread const* t)
    {
        EXPECT_EQ(kickos::tls_carve(t, idle_base(), IDLE_BYTES), nullptr);
    }
}

TEST(TlsCarveIdle, the_carve_is_non_empty_here)
{
    ASSERT_NE(kickos::tls_block_size(), 0u);
}

TEST(TlsCarveIdle, an_idle_stack_is_refused_by_admission)
{
    EXPECT_FALSE(kickos::tls_stack_admissible(reinterpret_cast<uintptr_t>(idle_base()),
                                              IDLE_BYTES));
}

TEST(TlsCarveIdle, the_boot_core_idle_takes_its_stack_uncarved)
{
    expect_uncarved(&kernel().idle_tcb);
}

TEST(TlsCarveIdle, every_peer_core_idle_takes_its_stack_uncarved)
{
    for (Thread const& peer : kernel().idle_tcb_peer)
    {
        expect_uncarved(&peer);
    }
}

TEST(TlsCarveIdle, a_pool_thread_on_an_idle_stack_is_refused)
{
    Thread const* const t = &kernel().threads.slots[ThreadPool::ROOT_INDEX + 1];
    KICKOS_EXPECT_PANIC(kickos::tls_carve(t, idle_base(), IDLE_BYTES), "assert: admissible");
}

TEST(TlsCarveIdle, a_strided_block_is_carved_and_seated_at_its_base)
{
    Thread const* const root = &kernel().threads.slots[ThreadPool::ROOT_INDEX];
    EXPECT_EQ(kickos::tls_carve(root, g_strided, TLSCARVE_STRIDE), g_strided);
}
