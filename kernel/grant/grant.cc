// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/grant.h>

#if KICKOS_MEMORY_ENFORCED

#include <kickos/arch/arch.h>
#include <kickos/domain.h> // arch_domain_static_regions
#include <kickos/kernel.h> // KICKOS_ASSERT

#include <span>

namespace kickos
{
    namespace
    {
        // Cortex-M bit-band: a word in the 1 MB peripheral region [0x40000000,
        // 0x40100000) is mirrored bit-per-word in the alias [0x42000000,0x44000000)
        // at  alias = 0x42000000 + (addr - 0x40000000) * 32. The SRAM twin aliases
        // [0x20000000,0x20100000) into [0x22000000,0x24000000). Present only where
        // arch_bitband_present() reports it (M3/M4).
        constexpr uintptr_t BB_PERI_BASE = 0x40000000u;
        constexpr uintptr_t BB_PERI_LAST = 0x400FFFFFu; // last byte that has an alias
        constexpr uintptr_t BB_PERI_ALIAS_BASE = 0x42000000u;
        constexpr uintptr_t BB_PERI_ALIAS_LAST = 0x43FFFFFFu; // [0x42000000,0x44000000)
        constexpr uintptr_t BB_SRAM_ALIAS_BASE = 0x22000000u;
        constexpr uintptr_t BB_SRAM_ALIAS_LAST = 0x23FFFFFFu; // [0x22000000,0x24000000)
    }

    bool grant_hits_reserved(uintptr_t base, size_t size)
    {
        if (size == 0)
        {
            return false; // an empty window touches nothing
        }
        uintptr_t const last = base + size - 1u;
        if (last < base)
        {
            return true; // wraps 2^32; inadmissible, fail closed
        }
        struct arch_reserved_block blocks[KICKOS_MAX_RESERVED];
        std::span const reserved{blocks, arch_reserved_blocks(blocks, KICKOS_MAX_RESERVED)};
        int const bitband = arch_bitband_present();
        for (struct arch_reserved_block const& b : reserved)
        {
            uintptr_t const b_base = b.base;
            uintptr_t const b_last = b.base + b.size - 1u;
            if (grant_ranges_overlap(base, last, b_base, b_last))
            {
                return true;
            }
            // R9: a reserved peripheral block is ALSO reachable through its bit-band
            // alias image, so a grant touching that image would poke the reserved
            // registers one bit at a time. Gate the x32 multiply behind the fully-
            // in-region test so neither the (base-0x40000000)*32 offset nor the
            // size*32 span can overflow (both bounded by the 1 MB region).
            if (bitband != 0 and b_base >= BB_PERI_BASE and b_last <= BB_PERI_LAST)
            {
                uintptr_t const alias_base =
                    BB_PERI_ALIAS_BASE + (b_base - BB_PERI_BASE) * 32u;
                uintptr_t const alias_last = alias_base + b.size * 32u - 1u;
                if (grant_ranges_overlap(base, last, alias_base, alias_last))
                {
                    return true;
                }
            }
        }
        return false;
    }

    bool grant_region_admissible(uintptr_t base, size_t size, uint32_t attr,
                                 bool caller_authorized)
    {
        // R6/minor: the size-0 and wrap refusals live HERE, not only in the overlap
        // helper (which treats size 0 as "touches nothing").
        if (size == 0)
        {
            return false;
        }
        if (base + size - 1u < base)
        {
            return false; // wraps 2^32
        }
        // Rule 7 core: a grant touching ANY reserved block is refused
        // UNCONDITIONALLY; privileged callers bind too.
        if (grant_hits_reserved(base, size))
        {
            return false;
        }
        // Memory TYPE, ahead of either geometry arm: a backend that cannot encode the type
        // drops the descriptor silently at commit, so the refusal happens here or nowhere.
        if (not grant_nocache_admissible(attr))
        {
            return false;
        }
        if ((attr & ARCH_MPU_DEV) != 0)
        {
            // Choice 5A: an MMIO/device grant needs the caller's AUTH_MEMORY, and must
            // map to exactly one MPU descriptor with no rounding (a rounded window
            // over-grants the neighbouring registers).
            if (not caller_authorized)
            {
                return false;
            }
            if (not arch_mpu_region_encodable(base, size))
            {
                return false;
            }
            // R5: on a bit-band chip refuse ANY DEV window intersecting either alias
            // region; nothing in-tree grants an alias, so a blanket refusal is the
            // simplest sound rule. DEV-only, hence here and not in hits_reserved.
            if (arch_bitband_present() != 0)
            {
                uintptr_t const last = base + size - 1u;
                if (grant_ranges_overlap(base, last, BB_PERI_ALIAS_BASE, BB_PERI_ALIAS_LAST)
                    or grant_ranges_overlap(base, last, BB_SRAM_ALIAS_BASE, BB_SRAM_ALIAS_LAST))
                {
                    return false;
                }
            }
            return true;
        }
        // RAM data grant. R1 (CRITICAL): the block must be nameable by ONE descriptor,
        // else the programmed window covers a span the caller did not ask for (a PMSA/
        // NAPOT descriptor snaps an unaligned base downwards). The mask this replaced
        // was only an alignment test, sound while every size was a power of two.
        // Geometry only: arena confinement is the check below.
        if (not arch_ram_region_admissible(base, size))
        {
            return false;
        }
        // Choice 10C: confine RAM to the user arena for EVERY caller (no privileged
        // waiver). Guard an absent arena (arch_ram_size() == 0 -> nothing admissible).
        (void)caller_authorized;
        size_t const ram_size = arch_ram_size();
        if (ram_size == 0)
        {
            return false;
        }
        uintptr_t const ram_base = arch_ram_base();
        uintptr_t const ram_last = ram_base + ram_size - 1u;
        return base >= ram_base and (base + size - 1u) <= ram_last;
    }

    void grant_reserved_validate(void)
    {
        struct arch_reserved_block blocks[KICKOS_MAX_RESERVED];
        size_t const n = arch_reserved_blocks(blocks, KICKOS_MAX_RESERVED);
        // A miswritten backend that returns more than it filled would make
        // grant_hits_reserved read past the buffer; catch it at boot. A zero count
        // (KICKOS_RESERVED_NONE, the sim) is legal, so there is NO count > 0 assert.
        KICKOS_ASSERT(n <= KICKOS_MAX_RESERVED);
        std::span const reserved{blocks, n};
        // arch_ram_region_size and arch_ram_region_admissible mask with min - 1.
        size_t const min = arch_mpu_min_region();
        KICKOS_ASSERT(min == 0 or (min & (min - 1u)) == 0);
        // Each declared block must be well-formed.
        for (struct arch_reserved_block const& b : reserved)
        {
            KICKOS_ASSERT(b.size != 0);
            KICKOS_ASSERT(b.base + b.size - 1u >= b.base);
        }
        // The static grantable extents must be reserved-disjoint: a legitimate grant
        // of app memory must never be refused, and the kernel must never have reserved
        // a block that overlaps the arena / app image it hands out.
        size_t const ram_size = arch_ram_size();
        if (ram_size != 0)
        {
            KICKOS_ASSERT(not grant_hits_reserved(arch_ram_base(), ram_size));
        }
        struct arch_mpu_region statics[KICKOS_MPU_MAX_REGIONS];
        std::span const region_set{statics,
                                   arch_domain_static_regions(statics, KICKOS_MPU_MAX_REGIONS)};
        for (struct arch_mpu_region const& r : region_set)
        {
            KICKOS_ASSERT(not grant_hits_reserved(r.base, r.size));
        }
    }
}

#endif // KICKOS_MEMORY_ENFORCED
