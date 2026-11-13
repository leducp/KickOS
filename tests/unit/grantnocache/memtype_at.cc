// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The translating board's memory-type admission per physical address, over the REAL
// kernel/grant/grant.cc at an address-space posture: where a page entry carries no type
// (RISC-V without Svpbmt) the address's own attribute decides, so a chip-stated aperture takes
// the uncached types and RAM takes none of them.

#include <kickos/grant.h>

#include <gtest/gtest.h>

#include <stddef.h>
#include <stdint.h>

static_assert(KICKOS_HAVE_ASPACE, "this gate's posture states a translating board");

namespace
{
    constexpr size_t G = 4096u;
    constexpr arch_phys_addr_t RAM = 0x80000000u;
    constexpr arch_phys_addr_t MMIO = 0x10000000u;
    constexpr size_t MMIO_SIZE = 0x1000u;
    constexpr struct arch_reserved_block APERTURES[] = {{0x10000000u, 0x1000u}};

    bool g_entry_memtype = false;
}

extern "C"
{
    struct arch_reserved_span arch_window_apertures(void) { return APERTURES; }
    size_t arch_aspace_granule(void) { return G; }
    bool arch_aspace_entry_memtype(void) { return g_entry_memtype; }
    bool arch_aspace_memtype_support(enum arch_map_memtype type)
    {
        return type == ARCH_MAP_NORMAL or type == ARCH_MAP_NOCACHE or type == ARCH_MAP_DEVICE;
    }
}

namespace
{
    using kickos::grant_memtype_at;

    class MemtypeAt : public ::testing::Test
    {
    protected:
        void TearDown() override { g_entry_memtype = false; }
    };

    // THE POSITIVE CONTROL: a page entry that carries the type delivers it at any address.
    TEST_F(MemtypeAt, an_entry_carrying_the_type_takes_it_on_ram_and_on_mmio)
    {
        g_entry_memtype = true;
        EXPECT_TRUE(grant_memtype_at(ARCH_MAP_NOCACHE, RAM, G));
        EXPECT_TRUE(grant_memtype_at(ARCH_MAP_DEVICE, RAM, G));
        EXPECT_TRUE(grant_memtype_at(ARCH_MAP_NOCACHE, MMIO, MMIO_SIZE));
        EXPECT_TRUE(grant_memtype_at(ARCH_MAP_DEVICE, MMIO, MMIO_SIZE));
    }

    TEST_F(MemtypeAt, an_entry_carrying_none_refuses_the_uncached_types_on_ram)
    {
        EXPECT_FALSE(grant_memtype_at(ARCH_MAP_NOCACHE, RAM, G));
        EXPECT_FALSE(grant_memtype_at(ARCH_MAP_DEVICE, RAM, G));
        EXPECT_TRUE(grant_memtype_at(ARCH_MAP_NORMAL, RAM, G));
    }

    TEST_F(MemtypeAt, an_entry_carrying_none_takes_both_uncached_types_on_mmio)
    {
        EXPECT_TRUE(grant_memtype_at(ARCH_MAP_NOCACHE, MMIO, MMIO_SIZE));
        EXPECT_TRUE(grant_memtype_at(ARCH_MAP_DEVICE, MMIO, MMIO_SIZE));
    }

    TEST_F(MemtypeAt, a_range_running_past_the_aperture_is_not_mmio)
    {
        EXPECT_FALSE(grant_memtype_at(ARCH_MAP_NOCACHE, MMIO, MMIO_SIZE + G));
        EXPECT_FALSE(grant_memtype_at(ARCH_MAP_DEVICE, MMIO - G, 2u * G));
    }

    TEST_F(MemtypeAt, a_type_the_backend_cannot_map_is_refused_everywhere)
    {
        g_entry_memtype = true;
        enum arch_map_memtype const unnamed = static_cast<enum arch_map_memtype>(3);
        EXPECT_FALSE(grant_memtype_at(unnamed, MMIO, MMIO_SIZE));
    }
}
