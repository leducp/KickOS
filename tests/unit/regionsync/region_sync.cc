// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A region chip's seats of a block, over the real seat code: a self-grant (thread_self_grant)
// and a new thread's windows, task data and stack (thread_region_sync, from thread_create), each
// syncing the block through grant_sync and the arena's ownership record of what a non-cacheable
// region left owed; and the refusal of a cacheable stack over a block another region holds
// non-cacheable (stack_type_free). And the set thread_create seats: a spawn's staged one as
// staged, and idle's or root's judged before it.

#include <kickos/aspace.h>
#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/ramown.h>
#include <kickos/task.h>
#include <kickos/thread.h>

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#if not KICKOS_ARCH_ARENA_DCACHE or KICKOS_HAVE_ASPACE
#error "this gate compiles the region seats with a data cache over the arena"
#endif

namespace
{
    constexpr size_t BLOCK = 0x400u;
    constexpr uint32_t RW = ARCH_MPU_R | ARCH_MPU_W;
    constexpr uint32_t RW_NC = RW | ARCH_MPU_NOCACHE;

    // The arena, bump-allocated and never reset: the ownership table has no reset either, so
    // each case takes blocks no earlier case named.
    alignas(BLOCK) unsigned char g_arena[32 * BLOCK];
    size_t g_arena_used = 0;

    // The ranges arch_dcache_invalidate was handed, in order.
    struct Op
    {
        uintptr_t base;
        size_t size;
    };
    Op g_ops[16];
    size_t g_op_count = 0;

    kickos::Domain g_domain;
    arch_mpu_region g_data = {};
    size_t g_data_count = 0;
}

extern "C"
{
    void arch_dcache_invalidate(void* p, size_t n)
    {
        if (g_op_count < 16)
        {
            g_ops[g_op_count] = {reinterpret_cast<uintptr_t>(p), n};
        }
        g_op_count++;
    }

    void* arch_ram_alloc(size_t size)
    {
        if (g_arena_used + size > sizeof(g_arena))
        {
            return nullptr;
        }
        void* const p = &g_arena[g_arena_used];
        g_arena_used += size;
        return p;
    }

    uintptr_t arch_ram_base(void)
    {
        return reinterpret_cast<uintptr_t>(g_arena);
    }

    size_t arch_ram_size(void)
    {
        return sizeof(g_arena);
    }

    int arch_mpu_nocache_support(void)
    {
        return ARCH_MPU_NOCACHE_PROGRAMMED;
    }

    size_t arch_mpu_min_region(void)
    {
        return 32u;
    }

    int arch_mpu_region_pow2(void)
    {
        return 1;
    }

    bool arch_mpu_region_encodable(uintptr_t base, size_t size)
    {
        return size != 0 and (base & (size - 1u)) == 0u;
    }

    struct arch_reserved_span arch_reserved_blocks(void)
    {
        return {};
    }

    struct arch_reserved_span arch_bus_master_apertures(void)
    {
        return {};
    }

    int arch_bitband_present(void)
    {
        return 0;
    }

    uint32_t arch_mpu_encode(struct arch_mpu_region const*, size_t n, struct arch_mpu_encoded*)
    {
        if (n >= 32u)
        {
            return 0xFFFFFFFFu;
        }
        return (1u << n) - 1u;
    }

    void arch_mpu_apply(struct arch_mpu_region const*, size_t, struct arch_mpu_encoded const*)
    {
    }

    arch_irq_state_t arch_irq_save(void)
    {
        return arch_irq_state_t{};
    }

    void arch_irq_restore(arch_irq_state_t)
    {
    }

    void arch_mpu_apply_now(struct arch_mpu_region const*, size_t,
                            struct arch_mpu_encoded const*)
    {
    }

    size_t arch_domain_static_regions(struct arch_mpu_region*, size_t)
    {
        return 0;
    }

    void arch_context_init(struct arch_context*, void (*)(void*), void*, void*, size_t, int) {}
}

namespace kickos
{
    namespace detail
    {
        constinit InstanceLocal<Kernel> g_instance;
    }

    void kpanic(char const* msg)
    {
        fprintf(stderr, "kernel panic: %s\n", msg);
        ADD_FAILURE() << "kernel panic: " << msg;
        abort();
    }

    kos_task_t task_handle(Task const* t)
    {
        return static_cast<kos_task_t>(reinterpret_cast<uintptr_t>(t) & 0xFFFFu);
    }

    Task* task_for(uint32_t, void*, size_t, Domain*, int*)
    {
        return nullptr;
    }

    void task_ref(Task*) {}

    size_t tls_block_size()
    {
        return 0;
    }

    void* tls_carve(Thread const*, void*, size_t)
    {
        return nullptr;
    }

    Domain* task_domain(Task const*)
    {
        return &g_domain;
    }

    Domain* domain_kernel(void)
    {
        return nullptr;
    }

    size_t domain_region_count(Domain const* d)
    {
        if (d != &g_domain)
        {
            return 0;
        }
        return g_data_count;
    }

    arch_mpu_region const* domain_region_at(Domain const*, size_t)
    {
        return &g_data;
    }
}

namespace
{
    alignas(8) unsigned char g_task_storage[64];
    kickos::Task* task()
    {
        return reinterpret_cast<kickos::Task*>(g_task_storage);
    }

    class RegionSync : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            g_op_count = 0;
            g_data_count = 0;
            blk_[0] = take();
            blk_[1] = take();
            t_.mpu.clear();
            t_.task = task();
            t_.state = kickos::ThreadState::RUNNING;
        }

        uintptr_t take()
        {
            void* const p = kickos::ram_owner_alloc(task(), BLOCK);
            EXPECT_NE(p, nullptr);
            return reinterpret_cast<uintptr_t>(p);
        }

        size_t synced()
        {
            size_t const n = g_op_count;
            g_op_count = 0;
            return n;
        }

        uintptr_t blk_[2] = {};
        kickos::Thread t_{};
    };

    TEST_F(RegionSync, a_self_grant_pays_and_leaves_the_debt_by_type)
    {
        EXPECT_EQ(kickos::thread_self_grant(&t_, blk_[0], BLOCK, RW_NC), 0);
        EXPECT_EQ(synced(), 1u);
        EXPECT_TRUE(kickos::ram_owner_sync_owed(blk_[0], BLOCK));
        EXPECT_EQ(kickos::thread_self_grant(&t_, blk_[0], BLOCK, RW), 0);
        EXPECT_EQ(synced(), 1u);
        EXPECT_FALSE(kickos::ram_owner_sync_owed(blk_[0], BLOCK));
        EXPECT_EQ(kickos::thread_self_grant(&t_, blk_[1], BLOCK, RW), 0);
        EXPECT_EQ(synced(), 0u);
    }

    TEST_F(RegionSync, a_cacheable_self_grant_over_an_owed_block_pays_it_once)
    {
        kickos::ram_owner_set_sync_owed(blk_[1], BLOCK, true);
        EXPECT_EQ(kickos::thread_self_grant(&t_, blk_[1], BLOCK, RW), 0);
        EXPECT_EQ(synced(), 1u);
        EXPECT_FALSE(kickos::ram_owner_sync_owed(blk_[1], BLOCK));
    }

    TEST_F(RegionSync, a_non_cacheable_task_data_region_is_not_synced_per_member)
    {
        g_data = {blk_[0], BLOCK, RW_NC};
        g_data_count = 1;
        kickos::ram_owner_set_sync_owed(blk_[0], BLOCK, true);
        kickos::thread_region_sync(&t_);
        kickos::thread_region_sync(&t_);
        EXPECT_EQ(synced(), 0u);
        EXPECT_TRUE(kickos::ram_owner_sync_owed(blk_[0], BLOCK));
    }

    TEST_F(RegionSync, a_cacheable_task_data_region_pays_the_debt_once)
    {
        g_data = {blk_[0], BLOCK, RW};
        g_data_count = 1;
        kickos::ram_owner_set_sync_owed(blk_[0], BLOCK, true);
        kickos::thread_region_sync(&t_);
        EXPECT_EQ(synced(), 1u);
        kickos::thread_region_sync(&t_);
        EXPECT_EQ(synced(), 0u);
        EXPECT_FALSE(kickos::ram_owner_sync_owed(blk_[0], BLOCK));
    }

    TEST_F(RegionSync, a_stack_seated_over_an_owed_block_pays_the_debt)
    {
        kickos::ram_owner_set_sync_owed(blk_[1], BLOCK, true);
        t_.stack_base = reinterpret_cast<void*>(blk_[1]);
        t_.stack_size = BLOCK;
        kickos::thread_region_sync(&t_);
        ASSERT_EQ(synced(), 1u);
        EXPECT_EQ(g_ops[0].base, blk_[1]);
        EXPECT_EQ(g_ops[0].size, BLOCK);
        EXPECT_FALSE(kickos::ram_owner_sync_owed(blk_[1], BLOCK));
        kickos::thread_region_sync(&t_);
        EXPECT_EQ(synced(), 0u);
    }

    TEST_F(RegionSync, a_window_syncs_by_type_and_records_the_debt)
    {
        ASSERT_TRUE(t_.mpu.add_window(blk_[0], BLOCK, RW_NC, 0, 0));
        ASSERT_TRUE(t_.mpu.add_window(blk_[1], BLOCK, RW, 1, 0));
        kickos::thread_region_sync(&t_);
        EXPECT_EQ(synced(), 1u);
        EXPECT_TRUE(kickos::ram_owner_sync_owed(blk_[0], BLOCK));
        EXPECT_FALSE(kickos::ram_owner_sync_owed(blk_[1], BLOCK));
    }

    void entry(void*) {}

    TEST_F(RegionSync, a_new_threads_stack_over_an_owed_block_pays_the_debt)
    {
        kickos::ram_owner_set_sync_owed(blk_[1], BLOCK, true);
        kickos::ThreadAttr attr;
        attr.task = task();
        kickos::thread_regions_boot(attr, reinterpret_cast<void*>(blk_[1]), BLOCK);
        kickos::Thread child{};
        kickos::thread_create(&child, entry, nullptr, reinterpret_cast<void*>(blk_[1]), BLOCK,
                              attr);
        ASSERT_EQ(synced(), 1u);
        EXPECT_EQ(g_ops[0].base, blk_[1]);
        EXPECT_FALSE(kickos::ram_owner_sync_owed(blk_[1], BLOCK));
    }

    TEST_F(RegionSync, a_stack_inside_the_spawn_s_own_uncached_window_is_not_free)
    {
        kos_window w = {blk_[1], BLOCK, KOS_WINDOW_MEMORY, KOS_WINDOW_UNCACHED};
        EXPECT_FALSE(kickos::stack_type_free(blk_[1], BLOCK, &w, 1));
        EXPECT_TRUE(kickos::stack_type_free(blk_[0], BLOCK, &w, 1));
        w.flags = 0;
        EXPECT_TRUE(kickos::stack_type_free(blk_[1], BLOCK, &w, 1));
    }

    // The spawn assembles and judges the set once; thread_create seats it as staged.
    TEST_F(RegionSync, a_staged_set_is_seated_as_staged)
    {
        kickos::MpuSet staged;
        staged.clear();
        ASSERT_TRUE(staged.add(blk_[0], BLOCK, RW));
        kickos::ThreadAttr attr;
        attr.task = task();
        attr.regions = &staged;
        kickos::Thread child{};
        kickos::thread_create(&child, entry, nullptr, reinterpret_cast<void*>(blk_[1]), BLOCK,
                              attr);
        ASSERT_EQ(child.mpu.end() - child.mpu.begin(), 1);
        EXPECT_EQ(child.mpu.begin()->base, blk_[0]);
    }

    // No spawn judges idle's or root's set: one this MPU decides otherwise than the kernel
    // stops the boot.
    TEST_F(RegionSync, a_boot_thread_whose_set_the_mpu_decides_otherwise_panics)
    {
        ASSERT_EQ(ARCH_MPU_OVERLAP, ARCH_MPU_OVERLAP_HIGHER);
        kos_window const w = {blk_[1], BLOCK, KOS_WINDOW_MEMORY, KOS_WINDOW_RO};
        uintptr_t const stack = blk_[1];
        EXPECT_DEATH(
            {
                kickos::ThreadAttr attr;
                attr.task = task();
                attr.windows = &w;
                attr.window_count = 1;
                kickos::thread_regions_boot(attr, reinterpret_cast<void*>(stack), BLOCK);
            },
            "regions overlap where the MPU decides");
    }
}
