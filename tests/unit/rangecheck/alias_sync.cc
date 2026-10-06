// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The cache maintenance the copy seam brackets a non-cacheable user end with, over the REAL
// kernel/syscall/syscall_mem.cc, the arch's maintenance recorded in its place (dcache_seam.cc).
// Each end the kernel reads has its lines dropped first, and each end it writes has them dropped
// before and cleaned and dropped after; a cacheable or a kernel end costs nothing.

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <kickos/arch/arch.h>
#include <kickos/aspace.h>
#include <kickos/domain.h>
#include <kickos/instance.h>
#include <kickos/kernel.h>
#include <kickos/thread.h>

#if KICKOS_HAVE_ASPACE
#include <kickos/task.h>

#include "aspace_seam.h"
#if not KICKOS_ARCH_ALIAS_DCACHE
#error "the translating posture of this gate compiles syscall_mem.cc with a cacheable kernel view"
#endif
#elif not KICKOS_ARCH_ARENA_DCACHE
#error "the region posture of this gate compiles syscall_mem.cc with a data cache over the arena"
#endif

#include "dcache_seam.h"
#include "kseam_test.h"
#include "syscall_internal.h"

namespace
{
    constexpr size_t PAGE = 0x1000u;

    // Two pages the user ends lie in: the first cacheable, the second not.
    alignas(PAGE) unsigned char g_mem[2 * PAGE];
}

extern "C"
{
    // The copy seam asks neither: its callers validate first.
    bool arch_user_text_readable(uintptr_t, size_t)
    {
        return false;
    }

    bool arch_user_data_writable(uintptr_t, size_t)
    {
        return false;
    }
}

#if not KICKOS_HAVE_ASPACE
namespace
{
    // The data region every domain task.cc creates states, when one is seeded.
    arch_mpu_region g_data_region = {};
    size_t g_data_regions = 0;
    // What the arena's ownership record answers, and the last mark grant_sync made in it.
    bool g_owed = false;
    int g_marked = -1;
}

namespace kickos
{
    size_t domain_region_count(Domain const*)
    {
        return g_data_regions;
    }

    arch_mpu_region const* domain_region_at(Domain const*, size_t)
    {
        return &g_data_region;
    }

    // The record itself is ram_owner's gate's subject and region_sync's; here it is what
    // grant_sync is handed and what it leaves.
    bool ram_owner_sync_owed(uintptr_t, size_t)
    {
        return g_owed;
    }

    void ram_owner_set_sync_owed(uintptr_t, size_t, bool owed)
    {
        g_marked = static_cast<int>(owed);
    }
}
#endif

namespace kickos
{
    namespace testfix
    {
        namespace
        {
            constexpr int SLOT_OWNER = 0;
            constexpr uint8_t PRIO = 4;
            constexpr uintptr_t VA = 0x20000000u;
            constexpr size_t LEN = 16;

            unsigned char const g_bytes[LEN] = {1, 2, 3, 4, 5, 6, 7, 8,
                                                9, 10, 11, 12, 13, 14, 15, 16};

            class AliasSync : public KSeam
            {
              protected:
                void SetUp() override
                {
                    KSeam::SetUp();
                    memset(g_mem, 0, sizeof(g_mem));
                    dcache_reset(nullptr);
                    owner_ = seat_pool(SLOT_OWNER, PRIO);
                    kernel().current[kickos_kernel_core()] = owner_;
#if KICKOS_HAVE_ASPACE
                    ASSERT_EQ(arch_aspace_granule(), PAGE);
                    Task* const tk = task(0);
                    join_task(owner_, tk);
                    ASSERT_NE(seat_space(task_domain(tk)), nullptr);
                    seat_backing(VA, g_mem, sizeof(g_mem), VA + PAGE);
#else
                    uintptr_t const base = reinterpret_cast<uintptr_t>(g_mem);
                    ASSERT_TRUE(owner_->mpu.add(base, PAGE, ARCH_MPU_R | ARCH_MPU_W));
                    ASSERT_TRUE(owner_->mpu.add(base + PAGE, PAGE,
                                                ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_NOCACHE));
#endif
                    ASSERT_NE(user_space_of(owner_), nullptr)
                        << "a null owner is kernel storage, which nothing maintains";
                }

#if KICKOS_HAVE_ASPACE
                void TearDown() override
                {
                    seat_backing(0, nullptr, 0, 0);
                }
#endif

                // The address a user end at `off` into the two pages has in its owner's view.
                static uintptr_t user(size_t off)
                {
#if KICKOS_HAVE_ASPACE
                    return VA + off;
#else
                    return reinterpret_cast<uintptr_t>(g_mem) + off;
#endif
                }

                static void expect_op(size_t i, size_t off, size_t n, bool landed)
                {
                    ASSERT_LT(i, DCACHE_OPS_MAX);
                    DcacheOp const& op = dcache_op(i);
                    EXPECT_EQ(op.at, static_cast<void const*>(&g_mem[off])) << "op " << i;
                    EXPECT_EQ(op.n, n) << "op " << i;
                    EXPECT_EQ(op.landed, landed) << "op " << i;
                }

                Thread* owner_ = nullptr;
            };

            TEST_F(AliasSync, a_write_into_a_non_cacheable_end_is_bracketed)
            {
                dcache_reset(g_bytes);
                ASSERT_TRUE(kaccess_to_user(user_space_of(owner_), user(PAGE + 8), g_bytes, LEN));
                EXPECT_EQ(memcmp(&g_mem[PAGE + 8], g_bytes, LEN), 0);
                ASSERT_EQ(dcache_count(), 2u) << "dropped before the write, cleaned after";
                expect_op(0, PAGE + 8, LEN, false);
                expect_op(1, PAGE + 8, LEN, true);
            }

            TEST_F(AliasSync, a_read_out_of_a_non_cacheable_end_drops_its_lines_first)
            {
                memcpy(&g_mem[PAGE + 8], g_bytes, LEN);
                unsigned char out[LEN] = {};
                dcache_reset(g_bytes, out);
                ASSERT_TRUE(kaccess_from_user(out, user_space_of(owner_), user(PAGE + 8), LEN));
                EXPECT_EQ(memcmp(out, g_bytes, LEN), 0);
                ASSERT_EQ(dcache_count(), 1u);
                expect_op(0, PAGE + 8, LEN, false);
                EXPECT_FALSE(dcache_op(0).landed) << "the lines are dropped before the read";
            }

            TEST_F(AliasSync, a_word_into_a_non_cacheable_end_is_bracketed)
            {
                void* const word = &g_mem[0];
                dcache_reset(reinterpret_cast<unsigned char const*>(&word));
                ASSERT_TRUE(kaccess_word_to_user(user_space_of(owner_), user(PAGE + 8), &word));
                ASSERT_EQ(dcache_count(), 2u);
                expect_op(0, PAGE + 8, sizeof(void*), false);
                expect_op(1, PAGE + 8, sizeof(void*), true);
            }

            TEST_F(AliasSync, a_user_to_user_copy_maintains_each_non_cacheable_end)
            {
                memcpy(&g_mem[PAGE + 64], g_bytes, LEN);
                dcache_reset(g_bytes);
                UserOwner const o = user_space_of(owner_);
                ASSERT_TRUE(ep_copy(o, user(PAGE + 8), o, user(PAGE + 64), LEN));
                EXPECT_EQ(memcmp(&g_mem[PAGE + 8], g_bytes, LEN), 0);
                ASSERT_EQ(dcache_count(), 3u);
                expect_op(0, PAGE + 64, LEN, true);
                expect_op(1, PAGE + 8, LEN, false);
                expect_op(2, PAGE + 8, LEN, true);
            }

            TEST_F(AliasSync, a_cacheable_end_costs_nothing)
            {
                memcpy(&g_mem[64], g_bytes, LEN);
                UserOwner const o = user_space_of(owner_);
                unsigned char out[LEN] = {};
                void* const word = &g_mem[0];
                ASSERT_TRUE(kaccess_to_user(o, user(8), g_bytes, LEN));
                ASSERT_TRUE(kaccess_from_user(out, o, user(64), LEN));
                ASSERT_TRUE(kaccess_word_to_user(o, user(128), &word));
                ASSERT_TRUE(ep_copy(o, user(256), o, user(64), LEN));
                EXPECT_EQ(memcmp(&g_mem[256], g_bytes, LEN), 0);
                EXPECT_EQ(dcache_count(), 0u);
            }

            TEST_F(AliasSync, a_kernel_end_costs_nothing)
            {
                unsigned char out[LEN] = {};
                ASSERT_TRUE(kaccess_from_user(out, nullptr, reinterpret_cast<uintptr_t>(g_bytes),
                                              LEN));
                EXPECT_EQ(dcache_count(), 0u);
            }

#if not KICKOS_HAVE_ASPACE
            TEST_F(AliasSync, a_non_cacheable_region_grant_maintains_the_block_once)
            {
                uintptr_t const base = reinterpret_cast<uintptr_t>(&g_mem[PAGE]);
                g_owed = false;
                grant_sync(nullptr, base, PAGE, ARCH_MPU_R | ARCH_MPU_W);
                grant_sync(nullptr, base, PAGE, ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_DEV);
                grant_sync(nullptr, base, PAGE,
                           ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_DEV | ARCH_MPU_NOCACHE);
                EXPECT_EQ(dcache_count(), 0u) << "a cacheable or a device grant costs nothing";
                grant_sync(nullptr, base, PAGE, ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_NOCACHE);
                ASSERT_EQ(dcache_count(), 1u);
                expect_op(0, PAGE, PAGE, false);
                EXPECT_EQ(g_marked, 1) << "the next cacheable region owes the sync";
            }

            TEST_F(AliasSync, a_retype_to_cacheable_maintains_the_block)
            {
                uintptr_t const base = reinterpret_cast<uintptr_t>(g_mem);
                g_owed = false;
                grant_sync(&owner_->mpu, base, PAGE, ARCH_MPU_R | ARCH_MPU_W);
                grant_sync(&owner_->mpu, base + PAGE, 2 * PAGE, ARCH_MPU_R | ARCH_MPU_W);
                EXPECT_EQ(dcache_count(), 0u)
                    << "a cacheable region retyped, or another extent, owes nothing";
                grant_sync(&owner_->mpu, base + PAGE, PAGE, ARCH_MPU_R | ARCH_MPU_W);
                ASSERT_EQ(dcache_count(), 1u) << "the stale lines of the non-cacheable use go";
                expect_op(0, PAGE, PAGE, false);
                EXPECT_EQ(g_marked, 0);
            }

            TEST_F(AliasSync, a_cacheable_region_over_a_block_owing_a_sync_maintains_it)
            {
                uintptr_t const base = reinterpret_cast<uintptr_t>(&g_mem[PAGE]);
                g_owed = true;
                grant_sync(nullptr, base, PAGE, ARCH_MPU_R | ARCH_MPU_W);
                g_owed = false;
                ASSERT_EQ(dcache_count(), 1u)
                    << "a non-cacheable holder's teardown left the kernel's lines stale";
                expect_op(0, PAGE, PAGE, false);
                EXPECT_EQ(g_marked, 0) << "paid";
            }

            // task.cc's seat of a new task's data region.
            TEST_F(AliasSync, a_task_data_region_is_synced_by_type_at_task_create)
            {
                uintptr_t const base = reinterpret_cast<uintptr_t>(&g_mem[PAGE]);
                g_data_regions = 1;
                int err = 0;
                g_owed = false;
                g_data_region = {base, PAGE, ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_NOCACHE};
                Task* const nc = task_create(1, 0, nullptr, 0, 0, nullptr, &err);
                size_t const nc_ops = dcache_count();
                int const nc_mark = g_marked;
                dcache_reset(nullptr);
                g_data_region = {base, PAGE, ARCH_MPU_R | ARCH_MPU_W};
                Task* const plain = task_create(1, 0, nullptr, 0, 0, nullptr, &err);
                size_t const plain_ops = dcache_count();
                dcache_reset(nullptr);
                g_owed = true;
                Task* const owed = task_create(1, 0, nullptr, 0, 0, nullptr, &err);
                g_owed = false;
                g_data_regions = 0;
                ASSERT_NE(nc, nullptr);
                ASSERT_NE(plain, nullptr);
                ASSERT_NE(owed, nullptr);
                EXPECT_EQ(nc_ops, 1u);
                EXPECT_EQ(nc_mark, 1);
                EXPECT_EQ(plain_ops, 0u);
                EXPECT_EQ(dcache_count(), 1u);
                EXPECT_EQ(g_marked, 0);
            }

            TEST_F(AliasSync, a_device_region_over_the_end_is_not_maintained)
            {
                Thread* const dev = seat_pool(1, PRIO);
                ASSERT_TRUE(dev->mpu.add(reinterpret_cast<uintptr_t>(&g_mem[PAGE]), PAGE,
                                         ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_DEV
                                             | ARCH_MPU_NOCACHE));
                ASSERT_TRUE(kaccess_to_user(user_space_of(dev), user(PAGE + 8), g_bytes, LEN));
                EXPECT_EQ(dcache_count(), 0u);
            }

            TEST_F(AliasSync, another_owners_non_cacheable_region_is_not_this_ends)
            {
                Thread* const other = seat_pool(1, PRIO);
                ASSERT_TRUE(kaccess_to_user(user_space_of(other), user(PAGE + 8), g_bytes, LEN));
                EXPECT_EQ(memcmp(&g_mem[PAGE + 8], g_bytes, LEN), 0);
                EXPECT_EQ(dcache_count(), 0u) << "the end's owner answers, not the address";
            }
#else
            TEST_F(AliasSync, a_mapping_owes_a_sync_on_a_non_cacheable_type_or_a_type_change)
            {
                uint8_t const normal = static_cast<uint8_t>(ARCH_MAP_NORMAL);
                uint8_t const nocache = static_cast<uint8_t>(ARCH_MAP_NOCACHE);
                VirtualRanges r;
                ASSERT_TRUE(r.init(PAGE));
                ASSERT_TRUE(r.reserve(VA, 1));
                VirtualRange const* const e = r.find(VA, 1);
                ASSERT_NE(e, nullptr);
                EXPECT_FALSE(aspace_grant_syncs(e, normal)) << "the pool's clear is cacheable";
                EXPECT_TRUE(aspace_grant_syncs(e, nocache));
                ASSERT_TRUE(r.grant(VA, 1, ARCH_MAP_R | ARCH_MAP_W, nocache));
                EXPECT_TRUE(aspace_grant_syncs(e, normal)) << "the retype back to cacheable";
                ASSERT_TRUE(r.grant(VA, 1, ARCH_MAP_R | ARCH_MAP_W, normal));
                EXPECT_FALSE(aspace_grant_syncs(e, normal));
            }
#endif

            // Under translation the copy is split at the granule and only the non-cacheable
            // chunk is maintained; a region set answers for the whole range at once.
            TEST_F(AliasSync, a_write_across_the_boundary_maintains_the_non_cacheable_part)
            {
                ASSERT_TRUE(kaccess_to_user(user_space_of(owner_), user(PAGE - 8), g_bytes, LEN));
                EXPECT_EQ(memcmp(&g_mem[PAGE - 8], g_bytes, LEN), 0);
                ASSERT_EQ(dcache_count(), 2u);
#if KICKOS_HAVE_ASPACE
                expect_op(0, PAGE, LEN - 8, false);
                expect_op(1, PAGE, LEN - 8, false);
#else
                expect_op(0, PAGE - 8, LEN, false);
                expect_op(1, PAGE - 8, LEN, false);
#endif
            }
        }
    }
}
