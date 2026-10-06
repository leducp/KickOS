// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_ARM_ARMV7M_DCACHE_PLAN_H
#define KICKOS_ARCH_ARM_ARMV7M_DCACHE_PLAN_H

#include <stddef.h>
#include <stdint.h>

namespace kickos::armv7m
{
    // How one maintenance call over [addr, addr + bytes) runs on an L1 data cache whose CCSIDR
    // is `ccsidr`: by line over `lines` lines from `first`, or once over every set and way, which
    // bounds the operations by the cache's lines whatever the range.
    struct DcachePlan
    {
        enum class Kind
        {
            NONE,
            WRAPS,
            LINES,
            SET_WAY
        };
        Kind kind;
        uintptr_t first;
        size_t lines;
        uintptr_t line;
    };

    inline constexpr DcachePlan dcache_plan(uint32_t ccsidr, uintptr_t addr, size_t bytes)
    {
        uintptr_t const line = static_cast<uintptr_t>(1u) << ((ccsidr & 0x7u) + 4u);
        if (bytes == 0)
        {
            return {DcachePlan::Kind::NONE, 0, 0, line};
        }
        if (bytes - 1u > UINTPTR_MAX - addr)
        {
            return {DcachePlan::Kind::WRAPS, 0, 0, line};
        }
        size_t const ways = ((ccsidr >> 3) & 0x3FFu) + 1u;
        size_t const sets = ((ccsidr >> 13) & 0x7FFFu) + 1u;
        if (bytes / line >= sets * ways)
        {
            return {DcachePlan::Kind::SET_WAY, 0, 0, line};
        }
        uintptr_t const first = addr & ~(line - 1u);
        uintptr_t const last = (addr + (bytes - 1u)) & ~(line - 1u);
        return {DcachePlan::Kind::LINES, first, (last - first) / line + 1u, line};
    }

    // Runs `op` on each line of a LINES plan, in address order.
    template <typename Op>
    inline void dcache_each_line(DcachePlan const& plan, Op op)
    {
        uintptr_t a = plan.first;
        for (size_t i = 0; i < plan.lines; i++)
        {
            op(a);
            a += plan.line;
        }
    }
}

#endif
