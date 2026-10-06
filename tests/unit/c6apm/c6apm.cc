// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The ESP32-C6 partition gate's rows, programmed into a fake HP_APM and LP_APM register file
// that starts at the TRM's reset values (Reg 16.1-16.4, 16.25-16.28), and read back.

#include "arch/riscv/chip/esp32c6/apm_rows.h"

#include <gtest/gtest.h>

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

namespace
{
    namespace apm = kickos::esp32c6::apm;
    namespace reg = kickos::esp32c6::reg::apm;

    constexpr uint8_t RW = KICKOS_GATE_R | KICKOS_GATE_W;
    constexpr uint8_t RWX = KICKOS_GATE_R | KICKOS_GATE_W | KICKOS_GATE_X;

    // Emitted by `kickos_compose gate` for esp32c6-wroom-amp2-n0 with no composition.
    kickos_gate_row const NO_COMPOSITION[] = {
        {0x40878000ull, 0x8000ull, 0, 0, 1, RW, 0},
        {0x4083C000ull, 0x3C000ull, 0, 0, 1, RWX, 0},
        {0x600B0000ull, 0x400ull, 1, 0, 1, RW, 0},
        {0x600B0C00ull, 0x400ull, 1, 0, 1, RW, 0},
        {0x70000000ull, 0x400ull, 1, 0, 1, RWX, 0},
    };

    // Emitted for the ampping partition (user/apps/common/ampping/systems/esp32c6-wroom/amp2),
    // node 0 granted /dev/timg0 and node 1 /dev/timg1: <build>/kickos_compose/ampping_system/gate.c.
    kickos_gate_row const AMPPING[] = {
        {0x60008000ull, 0x1000ull, 0, 0, 0, RW, 0},
        {0x40878000ull, 0x8000ull, 0, 0, 1, RW, 0},
        {0x60009000ull, 0x1000ull, 0, 0, 1, RW, 0},
        {0x4083C000ull, 0x3C000ull, 0, 0, 1, RWX, 0},
        {0x600B0000ull, 0x400ull, 1, 0, 1, RW, 0},
        {0x600B0C00ull, 0x400ull, 1, 0, 1, RW, 0},
        {0x70000000ull, 0x400ull, 1, 0, 1, RWX, 0},
    };

    // The ampping rows when REE0 was opened over every HP range the kernel and node 1 did not hold.
    kickos_gate_row const AMPPING_OPEN[] = {
        {0x60000000ull, 0x6000ull, 0, 0, 0, RW, 0},
        {0x60007000ull, 0x2000ull, 0, 0, 0, RW, 0},
        {0x6000A000ull, 0x6000ull, 0, 0, 0, RW, 0},
        {0x60011000ull, 0x7F000ull, 0, 0, 0, RW, 0},
        {0x60091000ull, 0x5000ull, 0, 0, 0, RW, 0},
        {0x60097000ull, 0x1000ull, 0, 0, 0, RW, 0},
        {0x6009A000ull, 0x16000ull, 0, 0, 0, RW, 0},
        {0x40878000ull, 0x8000ull, 0, 0, 1, RW, 0},
        {0x60009000ull, 0x1000ull, 0, 0, 1, RW, 0},
        {0x4083C000ull, 0x3C000ull, 0, 0, 1, RWX, 0},
        {0x600B0000ull, 0x400ull, 1, 0, 1, RW, 0},
        {0x600B0C00ull, 0x400ull, 1, 0, 1, RW, 0},
        {0x70000000ull, 0x400ull, 1, 0, 1, RWX, 0},
    };

    struct Window
    {
        uint32_t base;
        uint32_t size;
    };

    // What node 0's composition grants: ampping's /dev/timg0 (platform/esp32c6/chip.yaml).
    Window const AMPPING_NODE0[] = {{0x60008000u, 0x1000u}};

    template <size_t N>
    constexpr uint16_t count_of(kickos_gate_row const (&)[N])
    {
        return static_cast<uint16_t>(N);
    }

    // FILTER_EN starts at `filter`, the TRM's reset value unless a test stales it.
    struct FakeBus
    {
        std::map<uintptr_t, uint32_t> regs;
        uint32_t filter = 0x1u;

        uint32_t read(uintptr_t a) const
        {
            auto const it = regs.find(a);
            if (it != regs.end())
            {
                return it->second;
            }
            for (apm::Controller const& c : apm::CONTROLLER)
            {
                if (a == reg::filter_en(c.base))
                {
                    return filter;
                }
                if (a == reg::region_end(c.base, 0u))
                {
                    return 0xFFFFFFFFu;
                }
            }
            return 0u;
        }
    };

    FakeBus* g_bus = nullptr;

    void fake_write(uintptr_t a, uint32_t v)
    {
        g_bus->regs[a] = v;
    }

    struct Programmed
    {
        uint32_t gate;
        uint32_t n;
        uint32_t start;
        uint32_t end;
        uint32_t attr;
    };

    // Every enabled region past the catch-all, as the bus holds it.
    std::vector<Programmed> regions_of(FakeBus const& bus)
    {
        std::vector<Programmed> out;
        for (uint32_t g = 0; g < apm::GATES; g++)
        {
            uintptr_t const base = apm::CONTROLLER[g].base;
            uint32_t const filter = bus.read(reg::filter_en(base));
            for (uint32_t n = 1u; n < apm::CONTROLLER[g].regions; n++)
            {
                if ((filter & reg::region_en(n)) != 0u)
                {
                    out.push_back({g, n, bus.read(reg::region_start(base, n)), bus.read(reg::region_end(base, n)),
                                   bus.read(reg::region_attr_of(base, n))});
                }
            }
        }
        return out;
    }

    FakeBus programmed(kickos_gate_row const* rows, uint16_t count, uint32_t const* modes, uint32_t filter = 0x1u)
    {
        apm::Image image;
        EXPECT_EQ(apm::image_of(rows, count, modes, apm::NODE_MODES, image), apm::Refusal::NONE);
        FakeBus bus;
        bus.filter = filter;
        g_bus = &bus;
        apm::program(image, fake_write);
        g_bus = nullptr;
        return bus;
    }

    struct Block
    {
        char const* name;
        uint32_t base;
        uint32_t size;
    };

    // Kept from REE0: the kernel-owned HP blocks of platform/esp32c6/chip.yaml and HP SRAM.
    Block const KERNEL_BLOCKS[] = {
        {"RMT", 0x60006000u, 0x1000u},     {"INTMTX", 0x60010000u, 0x1000u}, {"IO_MUX", 0x60090000u, 0x1000u},
        {"PCR", 0x60096000u, 0x1000u},     {"HP_TEE", 0x60098000u, 0x1000u}, {"HP_APM", 0x60099000u, 0x1000u},
        {"INTPRI", 0x600C5000u, 0x1000u},  {"HP SRAM", 0x40800000u, 0x80000u},
    };

    constexpr uint32_t REE0_BITS = reg::ATTR_MODE_MASK << reg::attr_shift(reg::MODE_REE0);
    constexpr uint32_t REE1_BITS = reg::ATTR_MODE_MASK << reg::attr_shift(reg::MODE_REE1);
    constexpr uint32_t REE2_BITS = reg::ATTR_MODE_MASK << reg::attr_shift(reg::MODE_REE2);

    // What the C6 partition must hold, one line per breach.
    std::vector<std::string> breaches(FakeBus const& bus, kickos_gate_row const* rows, uint16_t count,
                                      Window const* granted, size_t granted_count)
    {
        std::vector<std::string> out;
        std::vector<Programmed> const regions = regions_of(bus);
        uint32_t want_filter[apm::GATES] = {0x1u, 0x1u};
        uint32_t seen[apm::GATES] = {0u, 0u};
        bool vector_row = false;
        for (uint16_t i = 0; i < count; i++)
        {
            uint32_t const g = rows[i].gate;
            seen[g]++;
            want_filter[g] |= reg::region_en(seen[g]);
        }
        for (uint32_t g = 0; g < apm::GATES; g++)
        {
            uint32_t const filter = bus.read(reg::filter_en(apm::CONTROLLER[g].base));
            if (filter != want_filter[g])
            {
                out.push_back("gate " + std::to_string(g) + " FILTER_EN " + std::to_string(filter));
            }
        }
        for (Programmed const& r : regions)
        {
            std::string const at = "gate " + std::to_string(r.gate) + " region " + std::to_string(r.n);
            if ((r.attr & REE2_BITS) != 0u)
            {
                out.push_back(at + " carries a REE2 bit");
            }
            if ((r.attr & REE0_BITS) != 0u)
            {
                bool inside = false;
                for (size_t i = 0; i < granted_count; i++)
                {
                    if (granted[i].base <= r.start and r.end <= granted[i].base + (granted[i].size - 1u))
                    {
                        inside = true;
                    }
                }
                if (not inside)
                {
                    out.push_back(at + " opens REE0 past every device node 0's composition grants");
                }
                for (Block const& b : KERNEL_BLOCKS)
                {
                    if (r.start <= b.base + (b.size - 1u) and b.base <= r.end)
                    {
                        out.push_back(at + " opens " + b.name + " to REE0");
                    }
                }
            }
            if (r.gate == 1u and r.start == 0x70000000u and r.end == 0x700003FFu and r.attr == REE1_BITS)
            {
                vector_row = true;
            }
        }
        // Each row is its gate's next region, at its node's mode.
        uint32_t next[apm::GATES] = {0u, 0u};
        for (uint16_t i = 0; i < count; i++)
        {
            kickos_gate_row const& row = rows[i];
            next[row.gate]++;
            uintptr_t const base = apm::CONTROLLER[row.gate].base;
            uint32_t const attr = bus.read(reg::region_attr_of(base, next[row.gate]));
            uint32_t want = static_cast<uint32_t>(row.access);
            if (row.node == 1u)
            {
                want = want << reg::attr_shift(reg::MODE_REE1);
            }
            if (bus.read(reg::region_start(base, next[row.gate])) != row.base
                or bus.read(reg::region_end(base, next[row.gate])) != row.base + row.size - 1u or attr != want)
            {
                out.push_back("row " + std::to_string(i) + " is not its region");
            }
        }
        if (not vector_row)
        {
            out.push_back("no LP_APM region grants REE1 rwx over 0x7000_0000..0x7000_03FF");
        }
        return out;
    }

    void expect_holds(kickos_gate_row const* rows, uint16_t count, Window const* granted, size_t granted_count)
    {
        FakeBus const bus = programmed(rows, count, apm::NODE_MODE);
        for (std::string const& b : breaches(bus, rows, count, granted, granted_count))
        {
            ADD_FAILURE() << b;
        }
        for (apm::Controller const& c : apm::CONTROLLER)
        {
            EXPECT_EQ(bus.read(reg::region_start(c.base, 0u)), 0u);
            EXPECT_EQ(bus.read(reg::region_end(c.base, 0u)), 0xFFFFFFFFu);
            EXPECT_EQ(bus.read(reg::region_attr_of(c.base, 0u)), 0u);
        }
    }

    apm::Refusal refusal_of(kickos_gate_row const* rows, uint16_t count)
    {
        apm::Image image;
        return apm::image_of(rows, count, image);
    }
}

TEST(C6Apm, NoCompositionRegisterImage)
{
    FakeBus const bus = programmed(NO_COMPOSITION, count_of(NO_COMPOSITION), apm::NODE_MODE);
    struct Want
    {
        uintptr_t gate;
        uint32_t n;
        uint32_t start;
        uint32_t end;
        uint32_t attr;
    };
    Want const want[] = {
        {0x60099000u, 1u, 0x40878000u, 0x4087FFFFu, 0x60u}, {0x60099000u, 2u, 0x4083C000u, 0x40877FFFu, 0x70u},
        {0x600B3800u, 1u, 0x600B0000u, 0x600B03FFu, 0x60u}, {0x600B3800u, 2u, 0x600B0C00u, 0x600B0FFFu, 0x60u},
        {0x600B3800u, 3u, 0x70000000u, 0x700003FFu, 0x70u},
    };
    for (Want const& w : want)
    {
        EXPECT_EQ(bus.read(w.gate + 0x04u + 0x0Cu * w.n), w.start) << w.gate << " region " << w.n;
        EXPECT_EQ(bus.read(w.gate + 0x08u + 0x0Cu * w.n), w.end) << w.gate << " region " << w.n;
        EXPECT_EQ(bus.read(w.gate + 0x0Cu + 0x0Cu * w.n), w.attr) << w.gate << " region " << w.n;
    }
    EXPECT_EQ(bus.read(0x60099000u), 0x7u);
    EXPECT_EQ(bus.read(0x600B3800u), 0xFu);
    // 3 writes per region and one FILTER_EN per gate.
    EXPECT_EQ(bus.regs.size(), 3u * 5u + 2u);
}

TEST(C6Apm, NoCompositionHoldsThePartition)
{
    expect_holds(NO_COMPOSITION, count_of(NO_COMPOSITION), nullptr, 0u);
}

TEST(C6Apm, AmppingHoldsThePartition)
{
    expect_holds(AMPPING, count_of(AMPPING), AMPPING_NODE0, 1u);
}

TEST(C6Apm, NoRowsLeaveEveryRegionOff)
{
    FakeBus const bus = programmed(nullptr, 0u, apm::NODE_MODE, 0xFFFFu);
    EXPECT_EQ(bus.read(0x60099000u), 0x1u);
    EXPECT_EQ(bus.read(0x600B3800u), 0x1u);
    EXPECT_EQ(bus.regs.size(), 2u);
}

TEST(C6Apm, AStaleRegionEnableIsCleared)
{
    FakeBus const bus = programmed(AMPPING, count_of(AMPPING), apm::NODE_MODE, 0x1u | reg::region_en(9u));
    EXPECT_EQ(bus.read(0x60099000u), 0x1Fu);
    EXPECT_EQ(bus.read(0x600B3800u), 0xFu);
}

TEST(C6Apm, Ree0PastNode0sGrantsIsCaught)
{
    FakeBus const bus = programmed(AMPPING_OPEN, count_of(AMPPING_OPEN), apm::NODE_MODE);
    size_t past = 0;
    for (std::string const& b : breaches(bus, AMPPING_OPEN, count_of(AMPPING_OPEN), AMPPING_NODE0, 1u))
    {
        if (b.find("past every device") != std::string::npos)
        {
            past++;
        }
    }
    EXPECT_EQ(past, 7u);
}

TEST(C6Apm, TheLpProbeKeepsItsRegions)
{
    EXPECT_TRUE(apm::lp_probe_fits(NO_COMPOSITION, 2u));
    EXPECT_FALSE(apm::lp_probe_fits(NO_COMPOSITION, count_of(NO_COMPOSITION)));
    kickos_gate_row hp[14];
    for (uint16_t i = 0; i < 14u; i++)
    {
        hp[i] = {0x60000000ull + 0x1000ull * i, 0x1000ull, 0, 0, 0, RW, 0};
    }
    EXPECT_TRUE(apm::lp_probe_fits(hp, 13u));
    EXPECT_FALSE(apm::lp_probe_fits(hp, 14u));
}

TEST(C6Apm, AmppingGivesTimg1ToTheLpOnly)
{
    FakeBus const bus = programmed(AMPPING, count_of(AMPPING), apm::NODE_MODE);
    uint32_t timg0 = 0;
    uint32_t timg1 = 0;
    for (Programmed const& r : regions_of(bus))
    {
        if (r.start <= 0x60008000u and 0x60008000u <= r.end)
        {
            timg0 |= r.attr;
        }
        if (r.start <= 0x60009000u and 0x60009000u <= r.end)
        {
            timg1 |= r.attr;
        }
    }
    EXPECT_EQ(timg0, REE0_BITS & ~reg::ATTR_X);
    EXPECT_EQ(timg1, REE1_BITS & ~(reg::ATTR_X << reg::attr_shift(reg::MODE_REE1)));
}

TEST(C6Apm, LpAtRee2IsCaught)
{
    uint32_t const ree2[apm::NODE_MODES] = {reg::MODE_REE0, reg::MODE_REE2};
    FakeBus const bus = programmed(AMPPING, count_of(AMPPING), ree2);
    std::vector<std::string> const found = breaches(bus, AMPPING, count_of(AMPPING), AMPPING_NODE0, 1u);
    bool ree2_bit = false;
    for (std::string const& b : found)
    {
        if (b.find("REE2") != std::string::npos)
        {
            ree2_bit = true;
        }
    }
    EXPECT_TRUE(ree2_bit);
}

TEST(C6Apm, RefusesWhatTheApmCannotHold)
{
    kickos_gate_row hp[16];
    for (uint16_t i = 0; i < 16u; i++)
    {
        hp[i] = {0x60000000ull + 0x1000ull * i, 0x1000ull, 0, 0, 0, RW, 0};
    }
    EXPECT_EQ(refusal_of(hp, 15u), apm::Refusal::NONE);
    EXPECT_EQ(refusal_of(hp, 16u), apm::Refusal::BUDGET);

    kickos_gate_row lp[4];
    for (uint16_t i = 0; i < 4u; i++)
    {
        lp[i] = {0x600B0000ull + 0x400ull * i, 0x400ull, 1, 0, 1, RW, 0};
    }
    EXPECT_EQ(refusal_of(lp, 3u), apm::Refusal::NONE);
    EXPECT_EQ(refusal_of(lp, 4u), apm::Refusal::BUDGET);

    kickos_gate_row bad = {0x60000000ull, 0x1000ull, 2, 0, 0, RW, 0};
    EXPECT_EQ(refusal_of(&bad, 1u), apm::Refusal::GATE);
    bad = {0x60000000ull, 0x1000ull, 0, 0, 2, RW, 0};
    EXPECT_EQ(refusal_of(&bad, 1u), apm::Refusal::NODE);
    bad = {0x60000002ull, 0x1000ull, 0, 0, 0, RW, 0};
    EXPECT_EQ(refusal_of(&bad, 1u), apm::Refusal::RANGE);
    bad = {0xFFFFF000ull, 0x2000ull, 0, 0, 0, RW, 0};
    EXPECT_EQ(refusal_of(&bad, 1u), apm::Refusal::RANGE);
    bad = {0x60000000ull, 0ull, 0, 0, 0, RW, 0};
    EXPECT_EQ(refusal_of(&bad, 1u), apm::Refusal::RANGE);
    bad = {0x60000000ull, 0x1000ull, 0, 0, 0, 0, 0};
    EXPECT_EQ(refusal_of(&bad, 1u), apm::Refusal::ACCESS);
    bad = {0x60000000ull, 0x1000ull, 0, 0, 0, 8, 0};
    EXPECT_EQ(refusal_of(&bad, 1u), apm::Refusal::ACCESS);
}

TEST(C6Apm, TheModeRegistersLieInTheChipFilesTees)
{
    namespace mmap = kickos::esp32c6::mmap;
    EXPECT_LT(reg::HP_TEE_M0_MODE_CTRL - mmap::HP_TEE_BASE, mmap::HP_TEE_SIZE);
    EXPECT_LT(reg::LP_TEE_M0_MODE_CTRL - mmap::LP_TEE_BASE, mmap::LP_TEE_SIZE);
}

// Every register a gate programs, the filter and each region's last word, at the TRM's bases
// (Table 5.3-2, p.176) and inside the chip file's windows.
TEST(C6Apm, TheRegionControllersLieInTheChipFilesApms)
{
    namespace mmap = kickos::esp32c6::mmap;
    EXPECT_EQ(reg::HP_APM_BASE, 0x60099000u);
    EXPECT_EQ(reg::LP_APM_BASE, 0x600B3800u);
    for (uintptr_t const gate : {reg::HP_APM_BASE, reg::LP_APM_BASE})
    {
        uintptr_t base = mmap::HP_APM_BASE;
        uintptr_t size = mmap::HP_APM_SIZE;
        if (gate == reg::LP_APM_BASE)
        {
            base = mmap::LP_APM_BASE;
            size = mmap::LP_APM_SIZE;
        }
        EXPECT_LT(reg::filter_en(gate) - base, size);
        EXPECT_LT(reg::region_attr_of(gate, apm::MAX_REGIONS - 1u) + 3u - base, size);
    }
}
