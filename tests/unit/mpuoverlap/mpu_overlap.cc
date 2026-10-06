// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// mpu_overlap_expressible under each backend's stated rule, over the shapes a spawn and a
// self-grant can build, and MpuSet's two set-level answers over it.

#include <kickos/mpuset.h>

#include <gtest/gtest.h>

int mpu_overlap_rule_armv6m_pmsav7();
int mpu_overlap_rule_armv7m_pmsav7();
int mpu_overlap_rule_armv7m_pmsav8();
int mpu_overlap_rule_armv7m_sysmpu();
int mpu_overlap_rule_rv32imac();
int mpu_overlap_rule_rxv3();
int mpu_overlap_rule_sim();

// Every region seated: the set-level answers are asked of the regions, never of the image.
extern "C" uint32_t arch_mpu_encode(struct arch_mpu_region const* regions, size_t n,
                                    struct arch_mpu_encoded* out)
{
    (void)regions;
    (void)out;
    return (static_cast<uint32_t>(1) << n) - 1u;
}

namespace
{
    using kickos::mpu_overlap_expressible;

    constexpr uint32_t RW = ARCH_MPU_R | ARCH_MPU_W;
    constexpr uint32_t RO = ARCH_MPU_R;
    constexpr uintptr_t BLOCK = 0x20004000u;
    constexpr size_t SIZE = 0x800u;

    arch_mpu_region region(uintptr_t base, size_t size, uint32_t attr)
    {
        arch_mpu_region r = {};
        r.base = base;
        r.size = size;
        r.attr = attr;
        return r;
    }

    struct Backend
    {
        char const* name;
        int (*rule)();
        int want;
    };

    Backend const BACKENDS[] = {
        {"armv6m PMSAv7", mpu_overlap_rule_armv6m_pmsav7, ARCH_MPU_OVERLAP_HIGHER},
        {"armv7m PMSAv7", mpu_overlap_rule_armv7m_pmsav7, ARCH_MPU_OVERLAP_HIGHER},
        {"armv7m PMSAv8", mpu_overlap_rule_armv7m_pmsav8, ARCH_MPU_OVERLAP_FAULTS},
        {"armv7m SYSMPU", mpu_overlap_rule_armv7m_sysmpu, ARCH_MPU_OVERLAP_UNION},
        {"rv32imac PMP", mpu_overlap_rule_rv32imac, ARCH_MPU_OVERLAP_LOWER},
        {"rxv3 MPU", mpu_overlap_rule_rxv3, ARCH_MPU_OVERLAP_UNION},
        {"sim mprotect", mpu_overlap_rule_sim, ARCH_MPU_OVERLAP_HIGHER},
    };

    TEST(MpuOverlap, EachBackendStatesItsHardwareRule)
    {
        for (Backend const& b : BACKENDS)
        {
            EXPECT_EQ(b.rule(), b.want) << b.name;
        }
    }

    // The shapes thread_create orders as data, windows, stack, so the first named is the lower.
    TEST(MpuOverlap, SpawnShapesOnEachBackend)
    {
        arch_mpu_region const data = region(BLOCK, SIZE, RW);
        arch_mpu_region const stack = region(BLOCK, SIZE, RW);
        arch_mpu_region const window_ro = region(BLOCK, SIZE, RO);
        arch_mpu_region const window_rw = region(BLOCK, SIZE, RW);
        for (Backend const& b : BACKENDS)
        {
            int const r = b.rule();
            bool const faults = (r == ARCH_MPU_OVERLAP_FAULTS);
            // Equal rights decide the same everywhere but where any overlap faults.
            EXPECT_EQ(mpu_overlap_expressible(r, data, stack), not faults) << b.name;
            EXPECT_EQ(mpu_overlap_expressible(r, window_rw, stack), not faults) << b.name;
            // A read-only window over the data: only a higher-numbered window decides it.
            EXPECT_EQ(mpu_overlap_expressible(r, data, window_ro), r == ARCH_MPU_OVERLAP_HIGHER)
                << b.name;
            // A read-only window under the stack: only a lower-numbered window decides it.
            EXPECT_EQ(mpu_overlap_expressible(r, window_ro, stack), r == ARCH_MPU_OVERLAP_LOWER)
                << b.name;
        }
    }

    TEST(MpuOverlap, DisjointAndAdjacentRegionsAreExpressibleEverywhere)
    {
        arch_mpu_region const below = region(BLOCK, SIZE, RW);
        arch_mpu_region const flush = region(BLOCK + SIZE, SIZE, RO);
        arch_mpu_region const apart = region(BLOCK + 4u * SIZE, SIZE, ARCH_MPU_R | ARCH_MPU_X);
        for (int r = ARCH_MPU_OVERLAP_FAULTS; r <= ARCH_MPU_OVERLAP_UNION; r++)
        {
            EXPECT_TRUE(mpu_overlap_expressible(r, below, flush)) << r;
            EXPECT_TRUE(mpu_overlap_expressible(r, flush, below)) << r;
            EXPECT_TRUE(mpu_overlap_expressible(r, below, apart)) << r;
            EXPECT_TRUE(mpu_overlap_expressible(r, region(BLOCK, 0, RO), below)) << r;
        }
    }

    TEST(MpuOverlap, OneSharedByteIsAnOverlap)
    {
        arch_mpu_region const low = region(BLOCK, SIZE, RW);
        arch_mpu_region const high = region(BLOCK + SIZE - 1u, SIZE, RW);
        EXPECT_FALSE(mpu_overlap_expressible(ARCH_MPU_OVERLAP_FAULTS, low, high));
        EXPECT_FALSE(mpu_overlap_expressible(ARCH_MPU_OVERLAP_FAULTS, high, low));
    }

    TEST(MpuOverlap, TheTopOfTheAddressSpaceDoesNotWrap)
    {
        uintptr_t const top = ~static_cast<uintptr_t>(0) - (SIZE - 1u);
        arch_mpu_region const last = region(top, SIZE, RW);
        arch_mpu_region const low = region(BLOCK, SIZE, RW);
        EXPECT_TRUE(mpu_overlap_expressible(ARCH_MPU_OVERLAP_FAULTS, low, last));
        EXPECT_FALSE(mpu_overlap_expressible(ARCH_MPU_OVERLAP_FAULTS, last, last));
    }

    TEST(MpuOverlap, AnUnknownRuleRefusesEveryOverlap)
    {
        arch_mpu_region const r = region(BLOCK, SIZE, RW);
        EXPECT_FALSE(mpu_overlap_expressible(ARCH_MPU_OVERLAP_UNION + 1, r, r));
    }

    // One thread's set holds one memory type per byte, under every rule.
    TEST(MpuOverlap, TwoMemoryTypesNeverShareAByte)
    {
        arch_mpu_region const cached = region(BLOCK, SIZE, RW);
        arch_mpu_region const uncached = region(BLOCK, SIZE, RW | ARCH_MPU_NOCACHE);
        arch_mpu_region const device = region(BLOCK, SIZE, RW | ARCH_MPU_DEV);
        for (int r = ARCH_MPU_OVERLAP_FAULTS; r <= ARCH_MPU_OVERLAP_UNION; r++)
        {
            EXPECT_FALSE(mpu_overlap_expressible(r, cached, uncached)) << r;
            EXPECT_FALSE(mpu_overlap_expressible(r, uncached, cached)) << r;
            EXPECT_FALSE(mpu_overlap_expressible(r, cached, device)) << r;
        }
    }

    struct Access
    {
        bool read;
        bool write;
    };

    // The kernel's checks on a byte both regions cover: user_range_ok reads through any region
    // granting R, and read_only_overlaps refuses a write wherever a region lacks W.
    Access kernel_decides(uint32_t lo, uint32_t hi)
    {
        return {((lo | hi) & ARCH_MPU_R) != 0, (lo & hi & ARCH_MPU_W) != 0};
    }

    // What each rule's hardware grants on that byte; FAULTS grants nothing.
    Access hardware_decides(int rule, uint32_t lo, uint32_t hi)
    {
        uint32_t granted = 0;
        if (rule == ARCH_MPU_OVERLAP_HIGHER)
        {
            granted = hi;
        }
        if (rule == ARCH_MPU_OVERLAP_LOWER)
        {
            granted = lo;
        }
        if (rule == ARCH_MPU_OVERLAP_UNION)
        {
            granted = lo | hi;
        }
        return {(granted & ARCH_MPU_R) != 0, (granted & ARCH_MPU_W) != 0};
    }

    // Exhaustive over every attribute pair: admitted exactly where the hardware decides the
    // shared byte as the kernel does and both regions name one memory type.
    TEST(MpuOverlap, AdmittedExactlyWhereTheHardwareDecidesAsTheKernel)
    {
        uint32_t const all = ARCH_MPU_R | ARCH_MPU_W | ARCH_MPU_X | ARCH_MPU_DEV | ARCH_MPU_NOCACHE;
        for (int rule = ARCH_MPU_OVERLAP_FAULTS; rule <= ARCH_MPU_OVERLAP_UNION; rule++)
        {
            for (uint32_t lo = 0; lo <= all; lo++)
            {
                for (uint32_t hi = 0; hi <= all; hi++)
                {
                    Access const k = kernel_decides(lo, hi);
                    Access const h = hardware_decides(rule, lo, hi);
                    bool const one_type =
                        ((lo ^ hi) & (ARCH_MPU_NOCACHE | ARCH_MPU_DEV)) == 0;
                    bool const same = rule != ARCH_MPU_OVERLAP_FAULTS and h.read == k.read
                                      and h.write == k.write;
                    EXPECT_EQ(mpu_overlap_expressible(rule, region(BLOCK, SIZE, lo),
                                                      region(BLOCK, SIZE, hi)),
                              one_type and same)
                        << "rule " << rule << " lower " << lo << " higher " << hi;
                }
            }
        }
    }

    // On a union MPU that is exactly equal W bits.
    TEST(MpuOverlap, AUnionMpuAdmitsExactlyEqualWriteRights)
    {
        for (uint32_t lo = 0; lo < 8u; lo++)
        {
            for (uint32_t hi = 0; hi < 8u; hi++)
            {
                EXPECT_EQ(mpu_overlap_expressible(ARCH_MPU_OVERLAP_UNION, region(BLOCK, SIZE, lo),
                                                  region(BLOCK, SIZE, hi)),
                          ((lo ^ hi) & ARCH_MPU_W) == 0)
                    << "lower " << lo << " higher " << hi;
            }
        }
    }

    // This build's own rule, which the set answers by.
    TEST(MpuOverlap, TheSetAnswersEveryPairInSlotOrder)
    {
        ASSERT_EQ(ARCH_MPU_OVERLAP, ARCH_MPU_OVERLAP_HIGHER);
        kickos::MpuSet s;
        s.clear();
        ASSERT_TRUE(s.add(BLOCK, SIZE, RW));
        ASSERT_TRUE(s.add(BLOCK + 4u * SIZE, SIZE, RO));
        EXPECT_TRUE(s.overlaps_expressible());
        // A read-only window above the data decides it as the kernel does.
        ASSERT_TRUE(s.add(BLOCK, SIZE, RO));
        EXPECT_TRUE(s.overlaps_expressible());
        // A writable region above the read-only one does not.
        ASSERT_TRUE(s.add(BLOCK + 4u * SIZE, SIZE / 2u, RW));
        EXPECT_FALSE(s.overlaps_expressible());
    }

    TEST(MpuOverlap, ARetypeIsAskedAtTheSlotItWouldTake)
    {
        kickos::MpuSet s;
        s.clear();
        ASSERT_TRUE(s.add(BLOCK, SIZE, RW));
        ASSERT_TRUE(s.add(BLOCK, SIZE / 2u, RO));
        ASSERT_TRUE(s.overlaps_expressible());
        // Appended above the read-only region, a writable superset would decide it writable.
        EXPECT_FALSE(s.retyping_expressible(BLOCK, 2u * SIZE, RW));
        // Retyped in place, a region is never a second region over itself.
        EXPECT_TRUE(s.retyping_expressible(BLOCK, SIZE / 2u, RW));
        EXPECT_TRUE(s.retyping_expressible(BLOCK, SIZE, RO));
        // Retyped in place BELOW a writable region over it, a read-only one would be decided
        // writable by the region above.
        kickos::MpuSet t;
        t.clear();
        ASSERT_TRUE(t.add(BLOCK, SIZE, RW));
        ASSERT_TRUE(t.add(BLOCK, SIZE / 2u, RW));
        EXPECT_FALSE(t.retyping_expressible(BLOCK, SIZE, RO));
        EXPECT_TRUE(t.retyping_expressible(BLOCK, SIZE / 2u, RO));
    }
}
