// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// thread_create's carve at the shared-kernel lx6 posture: the reentrant-state pointer in the
// control block, two kernel cores, no thread_local, and the thread-pointer mechanism as
// ARCH_LX6 declares it in arch/Kconfig. Every stack the ABI can seat an sp on takes the carve
// at its own base, a caller-supplied one included.

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

extern "C"
{
    extern unsigned char __kickos_tdata_start[];
    extern unsigned char __kickos_tdata_end[];
    extern unsigned char __kickos_tbss_start[];
    extern unsigned char __kickos_tbss_end[];
}

static_assert(TLSCARVE_REENT_IN_TCB == 1, "the control block is what makes the carve non-zero");
static_assert(KICKOS_KERNEL_CORES > 1, "a peer core's idle is a TCB under test");

namespace
{
    using kickos::kernel;
    using kickos::Thread;
    using kickos::ThreadPool;

    // KICKOS_IDLE_STACK_SIZE in boards/esp32-wroom/configs/smp/defconfig.
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
        return &kernel().threads.slots[ThreadPool::ROOT_INDEX + 1];
    }

    void expect_carved_at_base(Thread const* t, unsigned char* base, size_t bytes)
    {
        EXPECT_EQ(kickos::tls_carve(t, base, bytes), base);
    }
}

TEST(TlsCarveReent, the_control_block_is_carved_with_no_thread_local)
{
    ASSERT_EQ(__kickos_tdata_end - __kickos_tdata_start, 0);
    ASSERT_EQ(__kickos_tbss_end - __kickos_tbss_start, 0);
    EXPECT_NE(kickos::tls_block_size(), 0u);
    EXPECT_GE(kickos::tls_block_size(), TLSCARVE_TCB);
    EXPECT_EQ(kickos::tls_block_size() % KICKOS_STACK_ALIGN, 0u);
}

TEST(TlsCarveReent, a_caller_stack_neither_strided_nor_a_power_of_two_is_admitted)
{
    ASSERT_NE(reinterpret_cast<uintptr_t>(unstrided_base()) & (TLSCARVE_STRIDE - 1u), 0u);
    ASSERT_NE(CALLER_BYTES & (CALLER_BYTES - 1u), 0u);
    EXPECT_TRUE(kickos::tls_stack_admissible(reinterpret_cast<uintptr_t>(unstrided_base()),
                                             CALLER_BYTES));
}

TEST(TlsCarveReent, a_caller_stack_neither_strided_nor_a_power_of_two_is_carved_at_its_base)
{
    expect_carved_at_base(pool_thread(), unstrided_base(), CALLER_BYTES);
}

TEST(TlsCarveReent, the_boot_core_idle_is_carved_at_its_base)
{
    expect_carved_at_base(&kernel().idle_tcb, unstrided_base(), IDLE_BYTES);
}

TEST(TlsCarveReent, every_peer_core_idle_is_carved_at_its_base)
{
    for (Thread const& peer : kernel().idle_tcb_peer)
    {
        expect_carved_at_base(&peer, unstrided_base(), IDLE_BYTES);
    }
}

TEST(TlsCarveReent, a_base_the_abi_cannot_seat_an_sp_on_is_refused)
{
    unsigned char* const odd = unstrided_base() + 4;
    EXPECT_FALSE(kickos::tls_stack_admissible(reinterpret_cast<uintptr_t>(odd), CALLER_BYTES));
    KICKOS_EXPECT_PANIC(kickos::tls_carve(pool_thread(), odd, CALLER_BYTES),
                        "assert: admissible");
}
