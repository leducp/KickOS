// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ARCH_COMMON_RAM_ARENA_H
#define KICKOS_ARCH_COMMON_RAM_ARENA_H

#include <stddef.h>
#include <stdint.h>

// The bump allocator under arch_ram_alloc, with one free range kept: the alignment run-up a block
// skipped, which the next block that fits it takes. Blocks are never freed. Replayed exactly by
// arch/common/boot_arena.ld.h, tools/compose/kickos_compose/region.py (ArenaModel) and emit.py.
namespace kickos
{
    struct RamArena
    {
        size_t used = 0;
        size_t gap_off = 0;
        size_t gap_len = 0;
    };

    // A block of `rsz` bytes on `ralign` out of [base, base + total), or nullptr, the arena then
    // unchanged. The kept range takes it when it fits; else it is bumped, and the run-up it
    // leaves replaces the kept range only when strictly larger. Taking from the kept range keeps
    // the larger of the two pieces, the upper one on a tie. A bumped block that would run across
    // `line` (0: none) starts at the line instead. Caller serialises.
    //
    // The kept range never runs across the line only because the line sits on every block's
    // alignment, which the composition tool requires of a chip's `bank_line`.
    inline void* ram_arena_alloc(RamArena& a, uintptr_t base, size_t total, size_t rsz,
                                 size_t ralign, uintptr_t line = 0)
    {
        uintptr_t const mask = ~static_cast<uintptr_t>(ralign - 1);
        if (a.gap_len != 0)
        {
            uintptr_t const lo = base + a.gap_off;
            uintptr_t const hi = lo + a.gap_len;
            uintptr_t const at = (lo + (ralign - 1)) & mask;
            if (at >= lo and at <= hi and rsz <= hi - at)
            {
                size_t const before = static_cast<size_t>(at - lo);
                size_t const after = static_cast<size_t>(hi - at - rsz);
                if (after >= before)
                {
                    a.gap_off = static_cast<size_t>(at + rsz - base);
                    a.gap_len = after;
                }
                else
                {
                    a.gap_len = before;
                }
                return reinterpret_cast<void*>(at);
            }
        }
        uintptr_t const cur = base + a.used;
        uintptr_t at = (cur + (ralign - 1)) & mask;
        if (line != 0 and at < line and rsz > line - at)
        {
            at = (line + (ralign - 1)) & mask;
        }
        size_t const off = static_cast<size_t>(at - base);
        if (at < cur or off > total or rsz > total - off)
        {
            return nullptr;
        }
        size_t const run = static_cast<size_t>(at - cur);
        if (run > a.gap_len)
        {
            a.gap_off = a.used;
            a.gap_len = run;
        }
        a.used = off + rsz;
        return reinterpret_cast<void*>(at);
    }
}

#endif
