// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The ESP32-C6 partition gate's rows, programmed into a fake HP_APM, LP_APM and LP_APM0 register
// file that starts at the TRM's reset values (Reg 16.1-16.5, 16.25-16.29, 16.41-16.45), and read
// back.

#include "arch/riscv/chip/esp32c6/apm_rows.h"
#include "arch/riscv/chip/esp32c6/regs/pmu.h"

#include <gtest/gtest.h>

#include <stdint.h>

#include <map>
#include <string>
#include <vector>

namespace
{
    namespace apm = kickos::esp32c6::apm;
    namespace reg = kickos::esp32c6::reg::apm;
    namespace mmap = kickos::esp32c6::mmap;
    namespace pmu = kickos::esp32c6::reg::pmu;

    constexpr uint8_t RW = KICKOS_GATE_R | KICKOS_GATE_W;
    constexpr uint8_t RWX = KICKOS_GATE_R | KICKOS_GATE_W | KICKOS_GATE_X;

    // Emitted by `kickos_compose gate` for esp32c6-wroom-amp2-n0 with no composition.
    kickos_gate_row const NO_COMPOSITION[] = {
        {0x40878000ull, 0x8000ull, 0, 0, 1, RW, 0},
        {0x4083C000ull, 0x3C000ull, 0, 0, 1, RWX, 0},
        {0x600B0170ull, 0x18ull, 1, 0, 1, RW, 0},
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
        {0x600B0170ull, 0x18ull, 1, 0, 1, RW, 0},
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
        {0x600B0170ull, 0x18ull, 1, 0, 1, RW, 0},
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

    // Each controller's block as the TRM describes it: its base, regions, the bits FILTER_EN
    // and FUNC_CTRL implement, and where DATE sits (Reg 16.1-16.5 and 16.24, 16.25-16.29 and
    // 16.40, 16.41-16.45 and 16.52).
    struct Apm
    {
        uintptr_t base;
        uint32_t regions;
        uint32_t filter;
        uint32_t paths;
        uintptr_t date;
    };
    Apm const APMS[] = {
        {0x60099000u, 16u, 0xFFFFu, 0xFu, 0x7FCu},
        {0x600B3800u, 4u, 0xFu, 0x3u, 0xFCu},
        {0x60099800u, 4u, 0xFu, 0x1u, 0x7FCu},
    };
    constexpr uint32_t DATE = 0x2205240u; // Reg 16.24, 16.40, 16.52

    // A register file that keeps only the bits each register implements: an address's low two
    // bits are 0 (16.3.2.2), ATTR is bits 10:0 less 3 and 7 (Reg 16.4, 16.28, 16.44), a mode is
    // bits 1:0 (Reg 16.53, 16.56) and the bypass bit 0 (Reg 16.58). FILTER_EN starts at
    // `filter`, the reset value unless a test stales it, and a write to `stuck` is dropped.
    struct FakeBus
    {
        std::map<uintptr_t, uint32_t> regs;
        uint32_t filter = 0x1u;
        uintptr_t stuck = 0u;

        static uint32_t writable(uintptr_t a)
        {
            if (a == 0x60098000u or a == 0x600B3400u)
            {
                return 0x3u;
            }
            if (a == 0x600B3490u)
            {
                return 0x1u;
            }
            for (Apm const& b : APMS)
            {
                if (a == b.base)
                {
                    return b.filter;
                }
                if (a == b.base + 0xC4u)
                {
                    return b.paths;
                }
                for (uint32_t n = 0; n < b.regions; n++)
                {
                    if (a == b.base + 0x04u + 0x0Cu * n or a == b.base + 0x08u + 0x0Cu * n)
                    {
                        return ~3u;
                    }
                    if (a == b.base + 0x0Cu + 0x0Cu * n)
                    {
                        return 0x777u;
                    }
                }
            }
            return 0xFFFFFFFFu;
        }

        uint32_t read(uintptr_t a) const
        {
            auto const it = regs.find(a);
            if (it != regs.end())
            {
                return it->second;
            }
            for (Apm const& b : APMS)
            {
                if (a == b.base)
                {
                    return filter;
                }
                if (a == b.base + 0x08u)
                {
                    return 0xFFFFFFFFu;
                }
                if (a == b.base + b.date)
                {
                    return DATE;
                }
            }
            return 0u;
        }
    };

    FakeBus* g_bus = nullptr;

    void fake_write(uintptr_t a, uint32_t v)
    {
        if (a != g_bus->stuck)
        {
            g_bus->regs[a] = v & FakeBus::writable(a);
        }
    }

    uint32_t fake_read(uintptr_t a)
    {
        return g_bus->read(a);
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
        EXPECT_EQ(apm::program(image, fake_write, fake_read), 0u);
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
        {"INTMTX", mmap::INTMTX_BASE, mmap::INTMTX_SIZE},
        {"IO_MUX", mmap::IO_MUX_BASE, mmap::IO_MUX_SIZE},
        {"GPIO_MATRIX", mmap::GPIO_MATRIX_BASE, mmap::GPIO_MATRIX_SIZE},
        {"PCR", mmap::PCR_BASE, mmap::PCR_SIZE},
        {"HP_TEE", mmap::HP_TEE_BASE, mmap::HP_TEE_SIZE},
        {"HP_APM", mmap::HP_APM_BASE, mmap::HP_APM_SIZE},
        {"LP_APM0", mmap::LP_APM0_BASE, mmap::LP_APM0_SIZE},
        {"INTPRI", mmap::INTPRI_BASE, mmap::INTPRI_SIZE},
        {"HP SRAM", mmap::HP_SRAM_BASE, 0x80000u},
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
        uint32_t want_filter[apm::GATES];
        uint32_t seen[apm::GATES];
        for (uint32_t g = 0; g < apm::GATES; g++)
        {
            want_filter[g] = 0x1u;
            seen[g] = 0u;
        }
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
            if ((r.attr & REE1_BITS) != 0u and r.start <= mmap::PMU_BASE + (mmap::PMU_SIZE - 1u)
                and mmap::PMU_BASE <= r.end and (r.start < pmu::LP_KERNEL_FIRST or r.end > pmu::LP_KERNEL_LAST))
            {
                out.push_back(at + " opens PMU words past the LP kernel's to REE1");
            }
            if (r.gate == 1u and r.start == 0x70000000u and r.end == 0x700003FCu and r.attr == REE1_BITS)
            {
                vector_row = true;
            }
        }
        // Each row is its gate's next region, at its node's mode.
        uint32_t next[apm::GATES] = {};
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
                or bus.read(reg::region_end(base, next[row.gate])) != ((row.base + row.size - 1u) & ~3u) or attr != want)
            {
                out.push_back("row " + std::to_string(i) + " is not its region");
            }
        }
        if (not vector_row)
        {
            out.push_back("no LP_APM region grants REE1 rwx over 0x7000_0000..0x7000_03FF");
        }
        constexpr uint32_t REE1_RW = (reg::ATTR_R | reg::ATTR_W) << reg::attr_shift(reg::MODE_REE1);
        for (uintptr_t const word : {pmu::LP_INT_ST, pmu::LP_INT_CLR, pmu::HP_LP_CPU_COMM})
        {
            bool covered = false;
            for (Programmed const& r : regions)
            {
                if (r.gate == 1u and (r.attr & REE1_RW) == REE1_RW and r.start <= word and word + 3u <= (r.end | 3u))
                {
                    covered = true;
                }
            }
            if (not covered)
            {
                out.push_back("no LP_APM region opens the LP kernel's PMU word " + std::to_string(word) + " to REE1");
            }
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
            EXPECT_EQ(bus.read(reg::region_end(c.base, 0u)), 0xFFFFFFFCu);
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
        {0x600B3800u, 1u, 0x600B0170u, 0x600B0187u, 0x60u}, {0x600B3800u, 2u, 0x600B0C00u, 0x600B0FFFu, 0x60u},
        {0x600B3800u, 3u, 0x70000000u, 0x700003FFu, 0x70u},
    };
    for (Want const& w : want)
    {
        EXPECT_EQ(bus.read(w.gate + 0x04u + 0x0Cu * w.n), w.start) << w.gate << " region " << w.n;
        EXPECT_EQ(bus.read(w.gate + 0x08u + 0x0Cu * w.n), w.end & ~3u) << w.gate << " region " << w.n;
        EXPECT_EQ(bus.read(w.gate + 0x0Cu + 0x0Cu * w.n), w.attr) << w.gate << " region " << w.n;
    }
    EXPECT_EQ(bus.read(0x60099000u), 0x7u);
    EXPECT_EQ(bus.read(0x600B3800u), 0xFu);
    EXPECT_EQ(bus.read(0x60099800u), 0x1u);
    EXPECT_EQ(bus.regs.size(), 2u + 3u * 5u + 3u * 5u);
}

// Nothing the partition rests on is left at its reset value: every controller's catch-all, filter
// and path enables, the HP CPU's security mode and the LP CPU's bypass of HP SRAM's gate.
TEST(C6Apm, ProgramWritesTheResetPostureItRestsOn)
{
    FakeBus const bus = programmed(NO_COMPOSITION, count_of(NO_COMPOSITION), apm::NODE_MODE);
    std::vector<uintptr_t> want = {reg::HP_TEE_M0_MODE_CTRL, mmap::LP_TEE_BASE + 0x90u};
    for (uintptr_t const base : {uintptr_t{0x60099000u}, uintptr_t{0x600B3800u}, uintptr_t{0x60099800u}})
    {
        for (uintptr_t const at : {base + 0x00u, base + 0x04u, base + 0x08u, base + 0x0Cu, base + 0xC4u})
        {
            want.push_back(at);
        }
    }
    for (uintptr_t const at : want)
    {
        EXPECT_EQ(bus.regs.count(at), 1u) << std::hex << at << " is never written";
    }
    EXPECT_EQ(bus.read(reg::HP_TEE_M0_MODE_CTRL), reg::MODE_TEE);
    EXPECT_EQ(bus.read(reg::LP_TEE_FORCE_ACC_HP), 0u);
    EXPECT_EQ(bus.read(0x600990C4u), 0xFu);
    EXPECT_EQ(bus.read(0x600B38C4u), 0x3u);
    EXPECT_EQ(bus.read(0x600998C4u), 0x1u);
}

// A word that does not hold what was written refuses the program, the first such word named: the
// HP CPU's mode, HP_APM's catch-all end, LP_APM's catch-all start and LP_APM0's path enable.
TEST(C6Apm, AWordThatDoesNotHoldIsNamed)
{
    struct Stuck
    {
        uintptr_t at;
        uint32_t held;
    };
    Stuck const stuck[] = {{reg::HP_TEE_M0_MODE_CTRL, 0x2u},
                           {0x60099008u, 0xFFFFFFF0u},
                           {0x600B3804u, 0x600B0000u},
                           {0x600998C4u, 0x0u}};
    apm::Image image;
    ASSERT_EQ(apm::image_of(AMPPING, count_of(AMPPING), image), apm::Refusal::NONE);
    for (Stuck const& word : stuck)
    {
        FakeBus bus;
        bus.stuck = word.at;
        bus.regs[word.at] = word.held;
        g_bus = &bus;
        EXPECT_EQ(apm::program(image, fake_write, fake_read), word.at) << std::hex << word.at;
        g_bus = nullptr;
    }
}

// TRM 16.6.3 puts LP_APM0 at HP_APM + 0x1000, where Table 5.3-2 has a reserved block; the tree
// takes +0x800. A DATE that does not read Reg 16.52's value there refuses the program before it
// writes anything to LP_APM0.
TEST(C6Apm, NoLp_Apm0WhereItsDateShouldBeIsRefused)
{
    apm::Image image;
    ASSERT_EQ(apm::image_of(AMPPING, count_of(AMPPING), image), apm::Refusal::NONE);
    FakeBus bus;
    bus.regs[0x60099FFCu] = 0u;
    g_bus = &bus;
    EXPECT_EQ(apm::program(image, fake_write, fake_read), 0x60099FFCu);
    g_bus = nullptr;
    for (auto const& [at, value] : bus.regs)
    {
        EXPECT_FALSE(at >= 0x60099800u and at < 0x60099FFCu) << std::hex << at << " written";
    }
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
    EXPECT_EQ(bus.read(0x60099800u), 0x1u);
    EXPECT_EQ(bus.regs.size(), 2u + 3u * 5u);
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
    std::vector<std::string> kernel;
    for (std::string const& b : breaches(bus, AMPPING_OPEN, count_of(AMPPING_OPEN), AMPPING_NODE0, 1u))
    {
        if (b.find("past every device") != std::string::npos)
        {
            past++;
        }
        else if (b.find(" to REE0") != std::string::npos)
        {
            kernel.push_back(b);
        }
    }
    EXPECT_EQ(past, 7u);
    // Each open row stops at a kernel block's edge but the GPIO row, which runs over the matrix.
    EXPECT_EQ(kernel, std::vector<std::string>{"gate 0 region 5 opens GPIO_MATRIX to REE0"});
}

TEST(C6Apm, ALpKernelPmuWordLeftOutIsCaught)
{
    kickos_gate_row rows[count_of(NO_COMPOSITION)];
    for (uint16_t i = 0; i < count_of(NO_COMPOSITION); i++)
    {
        rows[i] = NO_COMPOSITION[i];
        if (rows[i].base == pmu::LP_KERNEL_FIRST)
        {
            rows[i].size = pmu::HP_LP_CPU_COMM - pmu::LP_KERNEL_FIRST;
        }
    }
    FakeBus const bus = programmed(rows, count_of(NO_COMPOSITION), apm::NODE_MODE);
    std::vector<std::string> const found = breaches(bus, rows, count_of(NO_COMPOSITION), nullptr, 0u);
    EXPECT_EQ(found, std::vector<std::string>{"no LP_APM region opens the LP kernel's PMU word "
                                              + std::to_string(pmu::HP_LP_CPU_COMM) + " to REE1"});
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

    kickos_gate_row bad = {0x60000000ull, 0x1000ull, 3, 0, 0, RW, 0};
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
    EXPECT_LT(reg::HP_TEE_M0_MODE_CTRL - mmap::HP_TEE_BASE, mmap::HP_TEE_SIZE);
    EXPECT_LT(reg::LP_TEE_M0_MODE_CTRL - mmap::LP_TEE_BASE, mmap::LP_TEE_SIZE);
    EXPECT_LT(reg::LP_TEE_FORCE_ACC_HP - mmap::LP_TEE_BASE, mmap::LP_TEE_SIZE);
}

// Every register a gate programs, the filter and each region's last word, at the TRM's bases
// (Table 5.3-2, p.176) and inside the chip file's windows.
TEST(C6Apm, TheRegionControllersLieInTheChipFilesApms)
{
    EXPECT_EQ(reg::HP_APM_BASE, 0x60099000u);
    EXPECT_EQ(reg::LP_APM_BASE, 0x600B3800u);
    // Past HP_APM's last register, DATE at 0x7FC (Reg 16.24).
    EXPECT_EQ(reg::LP_APM0_BASE, 0x60099800u);
    for (uintptr_t const gate : {reg::HP_APM_BASE, reg::LP_APM_BASE, reg::LP_APM0_BASE})
    {
        uintptr_t base = mmap::HP_APM_BASE;
        uintptr_t size = mmap::HP_APM_SIZE;
        if (gate == reg::LP_APM_BASE)
        {
            base = mmap::LP_APM_BASE;
            size = mmap::LP_APM_SIZE;
        }
        else if (gate == reg::LP_APM0_BASE)
        {
            base = mmap::LP_APM0_BASE;
            size = mmap::LP_APM0_SIZE;
        }
        EXPECT_LT(reg::filter_en(gate) - base, size);
        EXPECT_LT(reg::region_attr_of(gate, apm::MAX_REGIONS - 1u) + 3u - base, size);
        EXPECT_LT(reg::func_ctrl(gate) + 3u - base, size);
    }
}
