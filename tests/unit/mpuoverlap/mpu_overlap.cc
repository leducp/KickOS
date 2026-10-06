// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// mpu_overlap_expressible under each backend's stated rule, over the shapes a spawn and a
// self-grant can build, against what each backend's real encoder has its hardware decide, and
// MpuSet's two set-level answers over it.

#include <kickos/mpuset.h>

#include "hw.h"

#include <gtest/gtest.h>

#include <algorithm>

int mpu_overlap_rule_armv6m_pmsav7();
int mpu_overlap_rule_armv7m_pmsav7();
int mpu_overlap_rule_armv7m_pmsav8();
int mpu_overlap_rule_armv7m_sysmpu();
int mpu_overlap_rule_rv32imac();
int mpu_overlap_rule_rxv3();
int mpu_overlap_rule_sim();

// A region based here gets no descriptor from the encoder below.
constexpr uintptr_t UNSEATED = 0x20004100u;

// Every region seated but one based at UNSEATED.
extern "C" uint32_t arch_mpu_encode(struct arch_mpu_region const* regions, size_t n,
                                    struct arch_mpu_encoded* out)
{
    out->seated = 0;
    for (size_t i = 0; i < n; i++)
    {
        if (regions[i].base != UNSEATED)
        {
            out->seated |= static_cast<uint32_t>(1) << i;
        }
    }
    return out->seated;
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

    struct HwBackend
    {
        char const* name;
        int (*rule)();
        HwRun run;
    };

    HwBackend const HW[] = {
        {"armv7m PMSAv7", mpu_overlap_rule_armv7m_pmsav7, hw_armv7m_pmsav7},
        {"armv7m PMSAv8", mpu_overlap_rule_armv7m_pmsav8, hw_armv7m_pmsav8},
        {"armv7m SYSMPU", mpu_overlap_rule_armv7m_sysmpu, hw_armv7m_sysmpu},
        {"rv32imac PMP", mpu_overlap_rule_rv32imac, hw_rv32imac},
        {"rxv3 MPU", mpu_overlap_rule_rxv3, hw_rxv3},
        {"sim mprotect", mpu_overlap_rule_sim, hw_sim},
    };

    // The attributes a thread's region carries.
    constexpr uint32_t ATTRS[] = {
        RO,
        RW,
        ARCH_MPU_R | ARCH_MPU_X,
        RO | ARCH_MPU_NOCACHE,
        RW | ARCH_MPU_NOCACHE,
        RW | ARCH_MPU_DEV,
    };

    // Lower slot first. The last pair's lower region is one no encoder here can name.
    struct Shape
    {
        uintptr_t base[2];
        size_t size[2];
    };
    constexpr Shape SHAPES[] = {
        {{BLOCK, BLOCK}, {SIZE, SIZE}},
        {{BLOCK, BLOCK + SIZE / 2u}, {SIZE, SIZE / 4u}},
        {{BLOCK + SIZE / 4u, BLOCK}, {SIZE / 4u, SIZE}},
        {{BLOCK + 0x40u, BLOCK}, {0x60u, SIZE}},
    };

    // Each backend's real encoder, the image decoded as its hardware reads it, over every
    // attribute pair the predicate admits: each shared byte is granted exactly what the kernel's
    // checks grant it, readable through any region granting R and writable only where every
    // region grants W.
    TEST(MpuOverlap, EachEncoderDecidesAnAdmittedOverlapAsTheKernel)
    {
        for (HwBackend const& b : HW)
        {
            int const rule = b.rule();
            uint32_t admitted = 0;
            uint32_t unseated = 0;
            for (Shape const& shape : SHAPES)
            {
                for (uint32_t lo : ATTRS)
                {
                    for (uint32_t hi : ATTRS)
                    {
                        arch_mpu_region const set[2] = {
                            region(shape.base[0], shape.size[0], lo),
                            region(shape.base[1], shape.size[1], hi)};
                        uintptr_t const first = std::max(shape.base[0], shape.base[1]);
                        uintptr_t const last = std::min(shape.base[0] + shape.size[0],
                                                        shape.base[1] + shape.size[1]) - 1u;
                        uint32_t seated = 0;
                        (void)b.run(set, 2, first, &seated);
                        if (not mpu_overlap_expressible(rule, set[0], (seated & 1u) != 0, set[1],
                                                        (seated & 2u) != 0))
                        {
                            continue;
                        }
                        admitted++;
                        if (seated != 3u)
                        {
                            unseated++;
                        }
                        bool const read = ((lo | hi) & ARCH_MPU_R) != 0;
                        bool const write = (lo & hi & ARCH_MPU_W) != 0;
                        for (uintptr_t const at : {first, (first + last) / 2u, last})
                        {
                            HwAccess const got = b.run(set, 2, at, &seated);
                            EXPECT_EQ(got.read, read)
                                << b.name << " lower " << lo << " higher " << hi << " at 0x"
                                << std::hex << at;
                            EXPECT_EQ(got.write, write)
                                << b.name << " lower " << lo << " higher " << hi << " at 0x"
                                << std::hex << at;
                        }
                    }
                }
            }
            EXPECT_NE(unseated, 0u) << b.name << ": no admitted pair had an unseated region";
            if (rule != ARCH_MPU_OVERLAP_FAULTS)
            {
                EXPECT_GT(admitted, unseated) << b.name << ": no seated pair was admitted";
            }
        }
    }

    // A region with no descriptor decides nothing in the hardware.
    TEST(MpuOverlap, AnUnseatedRegionDecidesNothing)
    {
        arch_mpu_region const data = region(BLOCK, SIZE, RW);
        arch_mpu_region const window_ro = region(BLOCK, SIZE, RO);
        for (int r = ARCH_MPU_OVERLAP_FAULTS; r <= ARCH_MPU_OVERLAP_UNION; r++)
        {
            EXPECT_FALSE(mpu_overlap_expressible(r, data, true, window_ro, false)) << r;
            EXPECT_TRUE(mpu_overlap_expressible(r, window_ro, true, data, false)) << r;
            EXPECT_TRUE(mpu_overlap_expressible(r, data, false, data, true)) << r;
            EXPECT_FALSE(mpu_overlap_expressible(r, data, false, data, false)) << r;
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

    // The set asks the image which regions it seated: a read-only region with no descriptor over
    // writable data leaves the data's descriptor deciding the shared bytes writable.
    TEST(MpuOverlap, TheSetJudgesTheDescriptorsItSeated)
    {
        kickos::MpuSet s;
        s.clear();
        ASSERT_TRUE(s.add(BLOCK, SIZE, RW));
        ASSERT_TRUE(s.add(UNSEATED, SIZE / 4u, RO));
        EXPECT_FALSE(s.overlaps_expressible());
        // Retyped in place, the region is one the add that follows seats or refuses.
        EXPECT_TRUE(s.retyping_expressible(UNSEATED, SIZE / 4u, RW));
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
