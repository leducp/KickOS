// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The register image of the partition gate (kickos/sys/partition_gate.h) on the RP2350's
// ACCESSCTRL: each row's register given to its node's core alone.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2350_ACCESSCTRL_ROWS_H
#define KICKOS_ARCH_ARM_CHIP_RP2350_ACCESSCTRL_ROWS_H

#include <kickos/sys/partition_gate.h>

#include <stdint.h>

#include "regs/accessctrl.h"

namespace kickos::rp2350::accessctrl
{
    namespace gate = reg::accessctrl;

    enum class Refusal : uint8_t
    {
        NONE,
        NODE,     // a node that runs on no core
        REGISTER, // no bus access register
        NEVER,    // a register no node is assigned
        TWICE,    // a register an earlier row names
    };

    // Every row is checked before any is written. `at` is the refused row.
    inline Refusal refusal_of(kickos_gate_row const* rows, uint16_t count, uint32_t const* node_core,
                              uint32_t node_count, uint16_t& at)
    {
        at = 0;
        for (uint16_t i = 0; i < count; i++)
        {
            kickos_gate_row const& row = rows[i];
            at = i;
            if (row.node >= node_count or node_core[row.node] >= gate::CORES)
            {
                return Refusal::NODE;
            }
            if (row.reg < gate::FIRST_BUS_REG or row.reg > gate::LAST_BUS_REG or (row.reg & 3u) != 0u)
            {
                return Refusal::REGISTER;
            }
            for (uint32_t never : gate::NEVER_ASSIGNED)
            {
                if (row.reg == never)
                {
                    return Refusal::NEVER;
                }
            }
            for (uint16_t j = 0; j < i; j++)
            {
                if (rows[j].reg == row.reg)
                {
                    return Refusal::TWICE;
                }
            }
        }
        return Refusal::NONE;
    }

    // `was` is the register's value before the write, whose DMA bit is kept.
    inline uint32_t image_of(uint32_t was, uint32_t core)
    {
        return gate::PASSWORD | gate::DBG | (was & gate::DMA) | gate::core_bit(core) | gate::SP | gate::SU;
    }
}

#endif
