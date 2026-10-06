// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The register image of the partition gate (kickos/sys/partition_gate.h) on the ESP32-C6's
// APM: each gate's rows, in row order, become its regions 1, 2, ... past the catch-all, at
// the security mode of the row's node.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_APM_ROWS_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_APM_ROWS_H

#include <kickos/sys/partition_gate.h>

#include <stdint.h>

#include "regs/apm.h"

namespace kickos::esp32c6::apm
{
    constexpr uint32_t GATES = 2u;
    constexpr uint32_t MAX_REGIONS = 16u;

    struct Controller
    {
        uintptr_t base;
        uint32_t regions; // the catch-all included (Table 16.3-1, p.562)
    };

    // In the order of the chip file's `gates`.
    constexpr Controller CONTROLLER[GATES] = {
        {reg::apm::HP_APM_BASE, 16u},
        {reg::apm::LP_APM_BASE, 4u},
    };

    // By node index. Node 1, the LP CPU, must leave REE2: that mode is every DMA master's too.
    constexpr uint32_t NODE_MODES = 2u;
    constexpr uint32_t NODE_MODE[NODE_MODES] = {reg::apm::MODE_REE0, reg::apm::MODE_REE1};

    struct Region
    {
        uint32_t start;
        uint32_t end; // inclusive
        uint32_t attr;
    };

    struct GateImage
    {
        Region region[MAX_REGIONS]; // [1, used]; [0] is the catch-all, never written
        uint32_t used;
        uint32_t filter; // the FILTER_EN bits of the regions written
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
        GateImage gate[GATES];
        uint16_t row; // the refused row
    };

    inline Refusal image_of(kickos_gate_row const* rows, uint16_t count, uint32_t const* modes,
                            uint32_t mode_count, Image& out)
    {
        for (uint32_t g = 0; g < GATES; g++)
        {
            out.gate[g].used = 0;
            out.gate[g].filter = 0;
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
            GateImage& gate = out.gate[row.gate];
            if (gate.used + 1u >= CONTROLLER[row.gate].regions)
            {
                return Refusal::BUDGET;
            }
            gate.used++;
            uint32_t const n = gate.used;
            gate.region[n].start = static_cast<uint32_t>(row.base);
            gate.region[n].end = static_cast<uint32_t>(last);
            gate.region[n].attr = static_cast<uint32_t>(row.access) << reg::apm::attr_shift(modes[row.node]);
            gate.filter |= reg::apm::region_en(n);
        }
        return Refusal::NONE;
    }

    inline Refusal image_of(kickos_gate_row const* rows, uint16_t count, Image& out)
    {
        return image_of(rows, count, NODE_MODE, NODE_MODES, out);
    }

    // c6lpprobe programs HP_APM regions 14 and 15 and every LP_APM region past the catch-all.
    constexpr uint32_t LP_PROBE_FIRST_HP_REGION = 14u;

    inline bool lp_probe_fits(kickos_gate_row const* rows, uint16_t count)
    {
        uint32_t used[GATES] = {0u, 0u};
        for (uint16_t i = 0; i < count; i++)
        {
            if (rows[i].gate < GATES)
            {
                used[rows[i].gate]++;
            }
        }
        return used[0] < LP_PROBE_FIRST_HP_REGION and used[1] == 0u;
    }

    using Write = void (*)(uintptr_t at, uint32_t value);

    // FILTER_EN is written whole: a region the rows do not name stays disabled whatever enabled it
    // before.
    inline void program(Image const& image, Write write)
    {
        for (uint32_t g = 0; g < GATES; g++)
        {
            GateImage const& gate = image.gate[g];
            uintptr_t const base = CONTROLLER[g].base;
            for (uint32_t n = 1u; n <= gate.used; n++)
            {
                write(reg::apm::region_start(base, n), gate.region[n].start);
                write(reg::apm::region_end(base, n), gate.region[n].end);
                write(reg::apm::region_attr_of(base, n), gate.region[n].attr);
            }
            write(reg::apm::filter_en(base), reg::apm::region_en(0u) | gate.filter);
        }
    }
}

#endif
