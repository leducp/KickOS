// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The arena-ownership table is instance-scoped: under the multi-instance sim, an instance whose
// table is full leaves the next instance's records untouched, over the REAL kernel/mem/ramown.cc
// that owner_instances_shim.cc compiles at the posture owner_instances.h sets.

#include "owner_instances.h"

#include <kickos/instance_local.h>
#include <kickos/ramown.h>
#include <kickos/task.h>

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>

namespace
{
    constexpr size_t MIN_REGION = 32u;
    constexpr uintptr_t ARENA_BASE = 0x20010000u;
    constexpr size_t ARENA_SIZE = 0x8000u;

    size_t g_arena_used = 0;
}

extern "C"
{
    size_t arch_mpu_min_region(void) { return MIN_REGION; }
    int arch_mpu_region_pow2(void) { return 0; }

    uintptr_t arch_ram_base(void) { return ARENA_BASE; }
    size_t arch_ram_size(void) { return ARENA_SIZE; }

    void* arch_ram_alloc(size_t size)
    {
        size_t const rsz = arch_ram_region_size(size);
        if (size == 0 or rsz > ARENA_SIZE - g_arena_used)
        {
            return nullptr;
        }
        void* const p = reinterpret_cast<void*>(ARENA_BASE + g_arena_used);
        g_arena_used = g_arena_used + rsz;
        return p;
    }
}

namespace kickos
{
    namespace
    {
        Task g_task;
    }

    kos_task_t task_handle(Task const* t)
    {
        if (t != &g_task)
        {
            return KOS_TASK_NONE;
        }
        return 1u;
    }
}

namespace
{
    // How many records the calling instance seats before its table refuses.
    size_t fill()
    {
        size_t seated = 0;
        while (seated <= KICKOS_RAM_OWNER_SLOTS
               and kickos::ram_owner_alloc(&kickos::g_task, MIN_REGION) != nullptr)
        {
            seated++;
        }
        return seated;
    }

    TEST(OwnerInstances, AFullTableInOneInstanceLeavesTheNextOneItsOwn)
    {
        ASSERT_GE(ARENA_SIZE / MIN_REGION, 2u * KICKOS_RAM_OWNER_SLOTS + 1u)
            << "this fixture's arena cannot back two tables, so a refusal would be the arena's";
        unsigned const before = kickos::instance_select(1u);
        EXPECT_EQ(fill(), static_cast<size_t>(KICKOS_RAM_OWNER_SLOTS));
        (void)kickos::instance_select(2u);
        EXPECT_EQ(fill(), static_cast<size_t>(KICKOS_RAM_OWNER_SLOTS))
            << "the second instance found the first one's records";
        (void)kickos::instance_select(before);
    }
}
