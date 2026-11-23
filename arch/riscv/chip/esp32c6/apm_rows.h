// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The register image of the partition gate (kickos/sys/partition_gate.h) on the ESP32-C6's
// APM: each gate's rows, in row order, become its regions 1, 2, ... past the catch-all, at
// the security mode of the row's node. Every word the gate rests on is written, a reset value
// included, and read back.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_APM_ROWS_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_APM_ROWS_H

#include <kickos/sys/partition_gate.h>

#include <stdint.h>

#include "regs/apm.h"

namespace kickos::esp32c6::apm
{
    constexpr uint32_t GATES = 3u;
    constexpr uint32_t MAX_REGIONS = 16u;

    struct Controller
    {
        uintptr_t base;
        uint32_t regions; // the catch-all included (Table 16.3-1, pp.562-563)
        uint32_t first;   // its region 1's slot in Image::region
        uint32_t filter;  // FILTER_EN's bits (Reg 16.1, 16.25, 16.41)
        uint32_t paths;   // FUNC_CTRL's bits, one per access path (Reg 16.5, 16.29, 16.45)
    };

    // In the order of the chip file's `gates`.
    constexpr Controller CONTROLLER[GATES] = {
        {reg::apm::HP_APM_BASE, 16u, 0u, 0xFFFFu, 0xFu},
        {reg::apm::LP_APM_BASE, 4u, 15u, 0xFu, 0x3u},
        {reg::apm::LP_APM0_BASE, 4u, 18u, 0xFu, 0x1u},
    };
    constexpr uint32_t ROWS = 21u;
    static_assert(CONTROLLER[1].first == CONTROLLER[0].first + CONTROLLER[0].regions - 1u
                      and CONTROLLER[2].first == CONTROLLER[1].first + CONTROLLER[1].regions - 1u
                      and ROWS == CONTROLLER[2].first + CONTROLLER[2].regions - 1u,
                  "each gate's rows follow the last gate's, one slot per region past the catch-all");

    // By node index. Node 1, the LP CPU, must leave REE2: that mode is every DMA master's too.
    constexpr uint32_t NODE_MODES = 2u;
    constexpr uint32_t NODE_MODE[NODE_MODES] = {reg::apm::MODE_REE0, reg::apm::MODE_REE1};

    struct Region
    {
        uint32_t start;
        uint32_t end; // inclusive
        uint32_t attr;
    };

    enum class Refusal : uint8_t
    {
        NONE,
        GATE,   // a gate the chip has no controller for
        NODE,   // a node with no security mode
        BUDGET, // more rows than the gate has regions past its catch-all
        RANGE,  // empty, past 4 GiB, or not word-aligned (TRM 16.3.2.2)
        ACCESS, // no access, or a bit past R, W and X
    };

    struct Image
    {
        Region region[ROWS];
        uint32_t used[GATES];
        uint32_t filter[GATES];
        uint16_t row; // the refused row
    };

    inline Refusal image_of(kickos_gate_row const* rows, uint16_t count, uint32_t const* modes,
                            uint32_t mode_count, Image& out)
    {
        for (uint32_t g = 0; g < GATES; g++)
        {
            out.used[g] = 0;
            out.filter[g] = 0;
        }
        out.row = 0;
        for (uint16_t i = 0; i < count; i++)
        {
            kickos_gate_row const& row = rows[i];
            out.row = i;
            if (row.gate >= GATES)
            {
                return Refusal::GATE;
            }
            if (row.node >= mode_count)
            {
                return Refusal::NODE;
            }
            if (row.access == 0u or (row.access & ~reg::apm::ATTR_MODE_MASK) != 0u)
            {
                return Refusal::ACCESS;
            }
            uint64_t const last = row.base + row.size - 1u;
            if (row.size == 0u or last < row.base or last > 0xFFFFFFFFull or (row.base & 3u) != 0u
                or (row.size & 3u) != 0u)
            {
                return Refusal::RANGE;
            }
            Controller const& gate = CONTROLLER[row.gate];
            if (out.used[row.gate] + 1u >= gate.regions)
            {
                return Refusal::BUDGET;
            }
            out.used[row.gate]++;
            uint32_t const n = out.used[row.gate];
            Region& region = out.region[gate.first + n - 1u];
            region.start = static_cast<uint32_t>(row.base);
            region.end = static_cast<uint32_t>(last);
            region.attr = static_cast<uint32_t>(row.access) << reg::apm::attr_shift(modes[row.node]);
            out.filter[row.gate] |= reg::apm::region_en(n);
        }
        return Refusal::NONE;
    }

    inline Refusal image_of(kickos_gate_row const* rows, uint16_t count, Image& out)
    {
        return image_of(rows, count, NODE_MODE, NODE_MODES, out);
    }

    // Every word the gate is programmed with, in order, as emit(at, value, mask), the mask the
    // register's implemented bits. FILTER_EN is written whole: a region the rows do not name stays
    // disabled whatever enabled it before.
    template <typename Emit>
    inline void each_word(Image const& image, Emit emit)
    {
        emit(reg::apm::HP_TEE_M0_MODE_CTRL, reg::apm::MODE_TEE, reg::apm::MODE_MASK);
        emit(reg::apm::LP_TEE_FORCE_ACC_HP, 0u, reg::apm::FORCE_ACC_MASK);
        for (uint32_t g = 0; g < GATES; g++)
        {
            Controller const& gate = CONTROLLER[g];
            emit(reg::apm::region_start(gate.base, 0u), 0u, reg::apm::ADDR_MASK);
            emit(reg::apm::region_end(gate.base, 0u), 0xFFFFFFFFu, reg::apm::ADDR_MASK);
            emit(reg::apm::region_attr_of(gate.base, 0u), 0u, reg::apm::ATTR_MASK);
            for (uint32_t n = 1u; n <= image.used[g]; n++)
            {
                Region const& region = image.region[gate.first + n - 1u];
                emit(reg::apm::region_start(gate.base, n), region.start, reg::apm::ADDR_MASK);
                emit(reg::apm::region_end(gate.base, n), region.end, reg::apm::ADDR_MASK);
                emit(reg::apm::region_attr_of(gate.base, n), region.attr, reg::apm::ATTR_MASK);
            }
            emit(reg::apm::filter_en(gate.base), reg::apm::region_en(0u) | image.filter[g], gate.filter);
            emit(reg::apm::func_ctrl(gate.base), gate.paths, gate.paths);
        }
    }

    // The first word that does not hold what the gate needs there, 0 where every one does. LP_APM0's
    // DATE is read before anything is written; every written word is read back once all are
    // written, so two controllers decoding one address are caught too.
    template <typename Write, typename Read>
    inline uintptr_t program(Image const& image, Write write, Read read)
    {
        if ((read(reg::apm::LP_APM0_DATE) & reg::apm::DATE_MASK) != reg::apm::DATE)
        {
            return reg::apm::LP_APM0_DATE;
        }
        each_word(image, [&](uintptr_t at, uint32_t value, uint32_t) { write(at, value); });
        uintptr_t first = 0;
        each_word(image, [&](uintptr_t at, uint32_t value, uint32_t mask) {
            if (first == 0u and ((read(at) ^ value) & mask) != 0u)
            {
                first = at;
            }
        });
        return first;
    }
}

#endif
