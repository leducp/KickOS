// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The x86_64 map editor: the arch_aspace_* family of the arch.h seam over 4-level or 5-level
// paging structures (Intel SDM Vol 3 chapter 5).
//
// The regime is ADOPTED: the boot space here is the live one firmware handed over, and its
// tables belong to the FIRMWARE. At boot this file reads the root register, measures which of
// its top-level slots are taken, and adds exactly one entry of its own.
//
// One root register serves both privilege levels, so every space carries the kernel's mappings
// as a copy of the boot root's TOP-LEVEL entries and shares every table below them.
//
// The level count is a RUNTIME figure, so every walk below is written over it and over the
// leaf's level. Acquire walks the boot space for a frame's own address rather than assuming
// the identity map. Nothing here allocates a translation tag; the root write drops the whole
// non-global set instead.

#include <kickos/arch/arch.h>
#include <kickos/arch/aspace.h>
#include <kickos/arch/regs.h>
#include <kickos/chip_com1.h>
#include <kickos/extent.h>

#include <stddef.h>
#include <stdint.h>

extern "C"
{
    arch_phys_addr_t kickos_frame_alloc(void);
    void kickos_frame_free(arch_phys_addr_t frame);
    void kfault_terminate(void) __attribute__((noreturn));
}

namespace
{
    using namespace kickos::q35;

    // 4 KiB is the only GRANULE; the 2 MiB and 1 GiB forms are larger mappings through the
    // same tables.
    constexpr unsigned GRANULE_SHIFT = 12;
    constexpr size_t GRANULE = static_cast<size_t>(1) << GRANULE_SHIFT;
    constexpr size_t PTES = GRANULE / sizeof(uint64_t);
    constexpr unsigned INDEX_BITS = 9;
    constexpr int LEVEL_LEAF = 1;
    // The deepest level whose entry may name memory directly: 2 MiB at level 2, 1 GiB at level
    // 3. No root entry and no level-4 entry under 5-level paging carries the size bit at all, so
    // a walk must not consult it above level 3.
    constexpr int LEVEL_LARGEST_LEAF = 3;
    constexpr int LEVEL_MAX = 5;

    // Acquire spends no window, so nothing here bounds the live holds.
    constexpr size_t ACQUIRE_CAPACITY = SIZE_MAX;

    // Entry bits (Intel SDM Vol 3 chapter 5).
    constexpr uint64_t PTE_P = 1ull << 0;
    constexpr uint64_t PTE_RW = 1ull << 1;
    constexpr uint64_t PTE_US = 1ull << 2;
    constexpr uint64_t PTE_PWT = 1ull << 3;
    constexpr uint64_t PTE_PCD = 1ull << 4;
    constexpr uint64_t PTE_A = 1ull << 5;
    constexpr uint64_t PTE_D = 1ull << 6;
    constexpr uint64_t PTE_PS = 1ull << 7;
    // The same bit as PTE_PS, and the attribute-table index's high bit ONLY in a leaf of one
    // granule: a larger leaf carries that index bit at 12. Nothing here writes a larger leaf.
    constexpr uint64_t PTE_PAT_4K = 1ull << 7;
    constexpr uint64_t PTE_XD = 1ull << 63;
    constexpr uint64_t PTE_ADDR_MASK = 0x000ffffffffff000ull;

    constexpr uint64_t CR0_WP = 1ull << 16;
    constexpr uint64_t CR4_LA57 = 1ull << 12;
    constexpr uint32_t MSR_EFER = 0xc0000080;
    constexpr uint64_t EFER_NXE = 1ull << 11;
    constexpr uint32_t MSR_PAT = 0x277;

    // Memory types an IA32_PAT field can encode (Intel SDM Vol 3 Table 14-10).
    constexpr uint8_t PAT_UC = 0x00;
    constexpr uint8_t PAT_WB = 0x06;
    constexpr uint8_t PAT_UC_MINUS = 0x07;
    // The power-up layout (Table 14-12), which is also what PCD and PWT select on a part
    // reporting no attribute table at all.
    constexpr uint64_t PAT_POWER_UP = 0x0007040600070406ull;
    constexpr unsigned PAT_FIELDS = 8;

    // The identifier width this port RECORDS: nothing here allocates, generates or scopes
    // anything on a tag.
    constexpr unsigned TAG_BITS_RECORDED = 0;

    // The live regime, read once at aspace_init.
    unsigned g_levels = 0;
    uint64_t* g_boot_root = nullptr;
    // The top of the conventional run, which is the widest output address this port programs.
    arch_phys_addr_t g_ram_hi = 0;
    uintptr_t g_user_lo = 0;
    uintptr_t g_user_hi = 0;
    unsigned g_kernel_slots = 0;
    // Present entries in the table under the boot root's first present slot, which is what
    // decides where a kernel range of this port's own can go.
    unsigned g_first_child_entries = 0;

    // The port's own kernel window: one table per level under its top-level entry, indexed by
    // level minus one. Static because aspace_init runs long before a frame pool exists, and zero
    // so the window maps nothing, and holds no table, until asked.
    alignas(4096) uint64_t g_kwin_table[LEVEL_MAX - 1][PTES] = {};
    uintptr_t g_kwin_va = 0;

#if defined(KICKOS_ENABLE_SELFTEST)
    // Page-invalidation sequences the map editor issued, and the ones a not-installed space
    // skipped. Every writer holds the caller's IrqLock.
    uint32_t g_tlbi_issued = 0;
    uint32_t g_tlbi_elided = 0;
#endif

    using kickos::x86_64::read_cr3;
    using kickos::x86_64::read_msr;
    using kickos::x86_64::write_cr3;

    void cpuid_at(uint32_t leaf, uint32_t sub, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d)
    {
        __asm__ volatile("cpuid"
                         : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                         : "a"(leaf), "c"(sub));
    }

    // CPUID 0x80000001, EDX bit 20. The extended leaf need not exist at all, so its own
    // maximum is read first.
    bool nx_supported(void)
    {
        uint32_t a = 0;
        uint32_t b = 0;
        uint32_t c = 0;
        uint32_t d = 0;
        cpuid_at(0x80000000u, 0, &a, &b, &c, &d);
        if (a < 0x80000001u)
        {
            return false;
        }
        cpuid_at(0x80000001u, 0, &a, &b, &c, &d);
        return (d & (1u << 20)) != 0;
    }

    // MAXPHYADDR: CPUID 0x80000008, EAX bits 7:0, capped at the 52 the paging structures hold
    // (Intel SDM Vol 3 section 5.1.4). 36 where that leaf is absent, which is the figure the
    // same section states for a part with PAE, and long mode has PAE by construction.
    unsigned phys_addr_bits(void)
    {
        uint32_t a = 0;
        uint32_t b = 0;
        uint32_t c = 0;
        uint32_t d = 0;
        cpuid_at(0x80000000u, 0, &a, &b, &c, &d);
        unsigned bits = 36;
        if (a >= 0x80000008u)
        {
            cpuid_at(0x80000008u, 0, &a, &b, &c, &d);
            bits = static_cast<unsigned>(a & 0xFFu);
        }
        if (bits > 52)
        {
            bits = 52;
        }
        return bits;
    }

    static_assert(sizeof(uintptr_t) == sizeof(arch_phys_addr_t),
                  "the physical extent below is computed through the pointer-width helper");

    // The WHOLE output extent, granule alignment included. Bits 51:MAXPHYADDR are reserved in
    // every paging-structure entry and the output field stops at bit 51, so a run validated by
    // its start alone can walk off the top of either and come back aliased onto a low frame.
    bool phys_range_ok(arch_phys_addr_t pa, size_t pages)
    {
        if ((pa & ~PTE_ADDR_MASK) != 0)
        {
            return false;
        }
        uintptr_t end = 0;
        if (not kickos::extent_end(static_cast<uintptr_t>(pa), pages, GRANULE, &end))
        {
            return false;
        }
        return static_cast<arch_phys_addr_t>(end)
               <= (static_cast<arch_phys_addr_t>(1) << phys_addr_bits());
    }

    // CPUID leaf 1, EDX bit 16.
    bool pat_supported(void)
    {
        uint32_t a = 0;
        uint32_t b = 0;
        uint32_t c = 0;
        uint32_t d = 0;
        cpuid_at(1, 0, &a, &b, &c, &d);
        return (d & (1u << 16)) != 0;
    }

    // The first field of `pat` encoding `want`, or PAT_FIELDS where none does.
    unsigned pat_index_in(uint64_t pat, uint8_t want)
    {
        for (unsigned i = 0; i < PAT_FIELDS; i++)
        {
            if (static_cast<uint8_t>((pat >> (i * 8)) & 0x7ull) == want)
            {
                return i;
            }
        }
        return PAT_FIELDS;
    }

    [[noreturn]] void refuse(char const* what)
    {
        com1_puts("\nx86_64 aspace: ");
        com1_puts(what);
        com1_puts("\n");
        kfault_terminate();
    }

    // Identity is the adopted map's property; aspace_init proves it of this file's own tables
    // before anything relies on it.
    uint64_t* table_at(arch_phys_addr_t pa)
    {
        return reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(pa));
    }

    arch_phys_addr_t phys_of(void const* p)
    {
        return static_cast<arch_phys_addr_t>(reinterpret_cast<uintptr_t>(p));
    }

    uint64_t* root_of(struct arch_aspace* space)
    {
        return reinterpret_cast<uint64_t*>(space);
    }

    arch_phys_addr_t pte_pa(uint64_t pte)
    {
        return static_cast<arch_phys_addr_t>(pte & PTE_ADDR_MASK);
    }

    unsigned shift_at(int level)
    {
        return GRANULE_SHIFT + static_cast<unsigned>(level - 1) * INDEX_BITS;
    }

    size_t index_at(uintptr_t va, int level)
    {
        return static_cast<size_t>((va >> shift_at(level)) & (PTES - 1));
    }

    uintptr_t span_at(int level)
    {
        return static_cast<uintptr_t>(1) << shift_at(level);
    }

    // A present entry naming memory rather than another table. The size bit exists only at
    // levels 2 and 3, so consulting it at the root would read a bit the architecture reserves.
    bool is_leaf(uint64_t desc, int level)
    {
        if (level == LEVEL_LEAF)
        {
            return true;
        }
        if (level > LEVEL_LARGEST_LEAF)
        {
            return false;
        }
        return (desc & PTE_PS) != 0;
    }

    // Every TLB entry for this page, and every paging-structure-cache entry for the current
    // identifier whatever address it corresponds to (Intel SDM Vol 3 section 5.10.4.1). The
    // second clause is why a table entry this editor installs owes no invalidate of its own.
    void invalidate_page(uintptr_t va)
    {
#if defined(KICKOS_ENABLE_SELFTEST)
        g_tlbi_issued++;
#endif
        __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
    }

    // A cleared table entry can span up to 1 GiB, which no by-address invalidate covers. Writing
    // the root register back drops every TLB entry for identifier 0 except the GLOBAL ones, and
    // every paging-structure-cache entry for it (section 5.10.4.1); nothing this file writes
    // sets the global bit.
    void invalidate_all(void)
    {
        write_cr3(read_cr3());
    }

    // Whether `space` is the root this core is RUNNING on. A space installed nowhere has no
    // cached translation, activate's root write sweeping the whole non-global set. A second core
    // holds a root this one cannot read, so the elision is compiled out above one core.
    bool installed_here(struct arch_aspace* space)
    {
        return (read_cr3() & PTE_ADDR_MASK) == (phys_of(root_of(space)) & PTE_ADDR_MASK);
    }

    void invalidate_page_if(uintptr_t va, bool installed)
    {
#if KICKOS_NUM_CORES == 1
        if (not installed)
        {
#if defined(KICKOS_ENABLE_SELFTEST)
            g_tlbi_elided++;
#endif
            return;
        }
#else
        (void)installed;
#endif
        invalidate_page(va);
    }

    void zero_table(uint64_t* table)
    {
        for (size_t i = 0; i < PTES; i++)
        {
            table[i] = 0;
        }
    }

    bool table_empty(uint64_t const* table)
    {
        for (size_t i = 0; i < PTES; i++)
        {
            if (table[i] != 0)
            {
                return false;
            }
        }
        return true;
    }

    // The memory type of a GRANULE leaf as entry bits. The only place that supplies
    // kickos::x86_64::aspace_memtype_bits with the LIVE attribute table.
    bool memtype_bits(enum arch_map_memtype type, uint64_t* out)
    {
        return kickos::x86_64::aspace_memtype_bits(kickos::x86_64::aspace_attribute_table(), type,
                                                   out);
    }

    // A leaf for the unprivileged level. This architecture has no read-disable and no
    // execute-only form, so a request without read is REFUSED.
    bool leaf_attrs(uint32_t rights, enum arch_map_memtype type, uint64_t* out)
    {
        uint32_t const known = ARCH_MAP_R | ARCH_MAP_W | ARCH_MAP_X;
        if ((rights & ~known) != 0 or (rights & ARCH_MAP_R) == 0)
        {
            return false;
        }
        // Writable and executable at once is expressible here and refused anyway: the entry is
        // the only thing there is to take such a page back with.
        if ((rights & ARCH_MAP_W) != 0 and (rights & ARCH_MAP_X) != 0)
        {
            return false;
        }
        uint64_t bits = 0;
        if (not memtype_bits(type, &bits))
        {
            return false;
        }
        // Accessed and dirty set: nothing here consumes either.
        uint64_t desc = PTE_P | PTE_US | PTE_A | PTE_D | bits;
        if ((rights & ARCH_MAP_W) != 0)
        {
            desc |= PTE_RW;
        }
        if ((rights & ARCH_MAP_X) == 0)
        {
            desc |= PTE_XD;
        }
        *out = desc;
        return true;
    }

    // A table entry on the per-space path. The unprivileged bit is set at every level because
    // the walk ANDs it down; the leaf is what decides.
    uint64_t table_desc_user(arch_phys_addr_t frame)
    {
        return static_cast<uint64_t>(frame) | PTE_P | PTE_RW | PTE_US;
    }

    // The frame backing the page holding `va` in `root`, through a walk that HANDLES a large
    // leaf, the boot space being built out of them. False leaves `*pa` untouched.
    bool resolve(uint64_t const* root, uintptr_t va, arch_phys_addr_t* pa)
    {
        uint64_t const* table = root;
        for (int level = static_cast<int>(g_levels); level >= LEVEL_LEAF; level--)
        {
            uint64_t const desc = table[index_at(va, level)];
            if ((desc & PTE_P) == 0)
            {
                return false;
            }
            if (is_leaf(desc, level))
            {
                // A large leaf's address field has its low bits reserved to zero, so the mask
                // gives the leaf's base and the offset inside it comes from `va`.
                uintptr_t const inside = va & (span_at(level) - 1);
                *pa = pte_pa(desc) + static_cast<arch_phys_addr_t>(inside & ~(GRANULE - 1));
                return true;
            }
            table = table_at(pte_pa(desc));
        }
        return false;
    }

    // Null unless a GRANULE leaf stands at `va`, which is what makes unmap total-or-fail. This
    // editor may not take a large leaf apart, so none is answered.
    uint64_t* leaf_entry(uint64_t* root, uintptr_t va)
    {
        uint64_t* table = root;
        for (int level = static_cast<int>(g_levels); level > LEVEL_LEAF; level--)
        {
            uint64_t const desc = table[index_at(va, level)];
            if ((desc & PTE_P) == 0 or is_leaf(desc, level))
            {
                return nullptr;
            }
            table = table_at(pte_pa(desc));
        }
        uint64_t* const entry = &table[index_at(va, LEVEL_LEAF)];
        if ((*entry & PTE_P) == 0)
        {
            return nullptr;
        }
        return entry;
    }

    // Whether the adopted regime maps `pa` at its own address, MEASURED: the identity map is a
    // claim about the memory map.
    bool identity_maps(arch_phys_addr_t pa)
    {
        if (g_boot_root == nullptr)
        {
            return false;
        }
        arch_phys_addr_t back = 0;
        if (not resolve(g_boot_root, static_cast<uintptr_t>(pa), &back))
        {
            return false;
        }
        return back == (pa & ~static_cast<arch_phys_addr_t>(GRANULE - 1));
    }

    // A top-level slot the boot root HAS is the kernel half, whose tables every space shares, so
    // walking into one would free the tables every other space points at.
    //
    // The slot's PRESENCE is the ownership record. Hardware sets the accessed flag in any entry
    // it walks and the dirty flag in any entry that maps a page (Intel SDM Vol 3 section 5.8), so
    // a copy and its original drift apart on their own and cannot be matched by descriptor.
    bool slot_is_shared(uint64_t const* keep, size_t slot)
    {
        if (keep == nullptr)
        {
            return false;
        }
        return (keep[slot] & PTE_P) != 0;
    }

    // Frees every table under `table` and every leaf output it names. `keep` is the boot root's
    // entry array at the top level and null below it.
    void free_subtree(uint64_t* table, int level, uint64_t const* keep)
    {
        for (size_t i = 0; i < PTES; i++)
        {
            uint64_t const desc = table[i];
            if ((desc & PTE_P) == 0)
            {
                continue;
            }
            if (slot_is_shared(keep, i))
            {
                continue;
            }
            arch_phys_addr_t const out = pte_pa(desc);
            if (not is_leaf(desc, level))
            {
                free_subtree(table_at(out), level - 1, nullptr);
            }
            // A leaf output the pool never handed out is refused inside the free, which is what
            // lets a space hold a device page without destroy reclaiming it.
            kickos_frame_free(out);
            table[i] = 0;
        }
    }

    // Recursion is bounded by the level count.
    enum arch_aspace_result map_into(uint64_t* table, int level, uintptr_t va, size_t pages,
                                    arch_phys_addr_t pa, uint64_t leaf, bool installed)
    {
        while (pages != 0)
        {
            size_t const idx = index_at(va, level);
            if (level == LEVEL_LEAF)
            {
                if ((table[idx] & PTE_P) != 0)
                {
                    // The invalidate belongs BETWEEN the two writes: no access between them
                    // can then take the old frame with the new permissions, invalidation
                    // otherwise being free to be delayed (section 5.10.4.4).
                    table[idx] = 0;
                    invalidate_page_if(va, installed);
                }
                table[idx] = leaf | (static_cast<uint64_t>(pa) & PTE_ADDR_MASK);
                // A fresh slot is issued one anyway: section 5.10.4.3's exemption is
                // conditional on every EARLIER clearing of the same slot having been
                // invalidated, which is a property of the slot's history and not of this call.
                invalidate_page_if(va, installed);
                va += GRANULE;
                pa += GRANULE;
                pages--;
                continue;
            }

            uint64_t desc = table[idx];
            if ((desc & PTE_P) == 0)
            {
                arch_phys_addr_t const frame = kickos_frame_alloc();
                if (frame == 0)
                {
                    return ARCH_ASPACE_ENOMEM;
                }
                zero_table(table_at(frame));
                desc = table_desc_user(frame);
                table[idx] = desc;
            }
            else if (is_leaf(desc, level))
            {
                // A larger leaf already covers this range: replacing it would change the mapping
                // of pages this call was not asked about.
                return ARCH_ASPACE_EINVAL;
            }

            uintptr_t const span = span_at(level);
            uintptr_t const next = (va + span) & ~(span - 1);
            size_t const here_max = static_cast<size_t>((next - va) / GRANULE);
            size_t here = pages;
            if (here > here_max)
            {
                here = here_max;
            }
            enum arch_aspace_result const rc =
                map_into(table_at(pte_pa(desc)), level - 1, va, here, pa, leaf, installed);
            if (rc != ARCH_ASPACE_OK)
            {
                return rc;
            }
            va += static_cast<uintptr_t>(here) * GRANULE;
            pa += static_cast<arch_phys_addr_t>(here) * GRANULE;
            pages -= here;
        }
        return ARCH_ASPACE_OK;
    }

    // True when `table` is empty once its empty children are gone. `keep` is as in free_subtree:
    // a slot the boot root holds is never pruned.
    bool prune_empty(uint64_t* table, int level, uint64_t const* keep)
    {
        if (level == LEVEL_LEAF)
        {
            return table_empty(table);
        }
        for (size_t i = 0; i < PTES; i++)
        {
            uint64_t const desc = table[i];
            if ((desc & PTE_P) == 0)
            {
                continue;
            }
            if (slot_is_shared(keep, i))
            {
                continue;
            }
            if (is_leaf(desc, level))
            {
                continue;
            }
            arch_phys_addr_t const child = pte_pa(desc);
            if (prune_empty(table_at(child), level - 1, nullptr))
            {
                table[i] = 0;
                kickos_frame_free(child);
            }
        }
        return table_empty(table);
    }

    // A top-level slot the boot root left ABSENT is one a space may map; one it has is the
    // kernel half. The boundary is MEASURED off the adopted regime.
    bool slot_is_user(size_t slot)
    {
        if (g_boot_root == nullptr)
        {
            return false;
        }
        return (g_boot_root[slot] & PTE_P) == 0;
    }

    bool range_ok(uintptr_t va, size_t pages)
    {
        if (g_boot_root == nullptr or (va & (GRANULE - 1)) != 0)
        {
            return false;
        }
        uintptr_t end = 0;
        if (not kickos::extent_end(va, pages, GRANULE, &end))
        {
            return false; // 0 pages, a byte count past the pointer width, or a wrapped end
        }
        if (va < g_user_lo or end > g_user_hi)
        {
            return false;
        }
        // Every top-level slot the range touches, and not only its ends: the half a space may
        // map is not one contiguous run of slots in general.
        int const top = static_cast<int>(g_levels);
        uintptr_t at = va;
        while (at < end)
        {
            if (not slot_is_user(index_at(at, top)))
            {
                return false;
            }
            uintptr_t const span = span_at(top);
            at = (at & ~(span - 1)) + span;
        }
        return true;
    }
}

namespace kickos::x86_64
{
    uint64_t aspace_attribute_table(void)
    {
        if (not pat_supported())
        {
            return PAT_POWER_UP;
        }
        return read_msr(MSR_PAT);
    }

    bool aspace_memtype_bits(uint64_t pat, enum arch_map_memtype type, uint64_t* out)
    {
        uint8_t want = PAT_WB;
        if (type == ARCH_MAP_NOCACHE)
        {
            want = PAT_UC_MINUS;
        }
        else if (type == ARCH_MAP_DEVICE)
        {
            want = PAT_UC;
        }
        else if (type != ARCH_MAP_NORMAL)
        {
            return false;
        }
        unsigned const index = pat_index_in(pat, want);
        if (index >= PAT_FIELDS)
        {
            return false;
        }
        uint64_t bits = 0;
        if ((index & 1u) != 0)
        {
            bits |= PTE_PWT;
        }
        if ((index & 2u) != 0)
        {
            bits |= PTE_PCD;
        }
        if ((index & 4u) != 0)
        {
            bits |= PTE_PAT_4K;
        }
        *out = bits;
        return true;
    }

    void aspace_init(uintptr_t ram_base, size_t ram_size)
    {
        uint64_t const cr4 = read_cr4();
        // ring3_init reads the same control-register bit; neither publishes it to the other.
        g_levels = 4;
        if ((cr4 & CR4_LA57) != 0)
        {
            g_levels = 5;
        }

        // A part without execute-disable is REFUSED: leaf_attrs expresses "readable, not
        // executable" with PTE_XD and with nothing else this architecture offers, so the caller
        // would be handed an executable page with a success code. Bit 63 is reserved while
        // EFER.NXE is clear, and setting NXE on a part reporting no bit 20 is a
        // general-protection fault.
        if (not nx_supported())
        {
            refuse("this part reports no execute-disable bit, which every leaf here carries");
        }
        // Setting it where it is clear cannot change how an existing entry behaves, every one
        // of them having to carry a zero there to have been legal at all.
        uint64_t const efer = read_msr(MSR_EFER);
        if ((efer & EFER_NXE) == 0)
        {
            write_msr(MSR_EFER, efer | EFER_NXE);
        }

        g_boot_root = table_at(static_cast<arch_phys_addr_t>(read_cr3() & PTE_ADDR_MASK));
        g_ram_hi = static_cast<arch_phys_addr_t>(ram_base)
                   + static_cast<arch_phys_addr_t>(ram_size);

        // Identity is checked BEFORE it is used: every walk reaches a table through its own
        // physical address.
        if (not identity_maps(phys_of(&g_kwin_table[0][0])))
        {
            refuse("the adopted regime does not map this image at its own physical address");
        }
        // An entry's address field has no low bits, so a window table the link placed off a
        // granule boundary would be silently truncated into a neighbour's frame.
        if ((phys_of(&g_kwin_table[0][0]) & static_cast<arch_phys_addr_t>(GRANULE - 1)) != 0)
        {
            refuse("the window tables are not granule aligned where the loader put this image");
        }

        int const top = static_cast<int>(g_levels);

        // The port's kernel range takes a TOP-LEVEL slot: the table under the boot root's first
        // present slot is full on this firmware. That is sound only BEFORE the first create, a
        // top-level entry added later reaching no space that has already copied the root. The
        // canonical HIGH half, searched from the top down, is where this firmware maps nothing.
        size_t kwin_slot = PTES;
        for (size_t i = PTES; i > PTES / 2; i--)
        {
            if ((g_boot_root[i - 1] & PTE_P) == 0)
            {
                kwin_slot = i - 1;
                break;
            }
        }
        if (kwin_slot == PTES)
        {
            refuse("the adopted root leaves no high-half slot for this port's kernel range");
        }
        // Firmware write-protects its own tables against a privileged store, so the one entry
        // that anchors the range needs the check lifted for it.
        //
        // Privileged the whole way down: no unprivileged bit anywhere in this chain, so no
        // thread at that level can reach a byte of the range whichever root is installed.
        uint64_t const cr0 = read_cr0();
        if ((cr0 & CR0_WP) != 0)
        {
            write_cr0(cr0 & ~CR0_WP);
        }
        g_boot_root[kwin_slot] = phys_of(&g_kwin_table[top - 2][0]) | PTE_P | PTE_RW;
        if ((cr0 & CR0_WP) != 0)
        {
            write_cr0(cr0);
        }
        // Sign-extended, a high-half index needing every bit above the top level's field set for
        // the address to be canonical at all.
        g_kwin_va = static_cast<uintptr_t>(kwin_slot) << shift_at(top);
        if (kwin_slot >= PTES / 2)
        {
            g_kwin_va |= ~((static_cast<uintptr_t>(1) << (shift_at(top) + INDEX_BITS)) - 1);
        }

        // AFTER the install, so the slot this port took counts as the kernel half it now is.
        size_t first_present = PTES;
        g_kernel_slots = 0;
        g_user_lo = 0;
        g_user_hi = 0;
        for (size_t i = 0; i < PTES; i++)
        {
            if ((g_boot_root[i] & PTE_P) == 0)
            {
                continue;
            }
            g_kernel_slots++;
            if (first_present == PTES)
            {
                first_present = i;
            }
        }
        if (first_present == PTES)
        {
            refuse("the adopted root maps nothing, so there is no kernel half to share");
        }
        g_first_child_entries = 0;
        if (not is_leaf(g_boot_root[first_present], top))
        {
            uint64_t const* const child = table_at(pte_pa(g_boot_root[first_present]));
            for (size_t i = 0; i < PTES; i++)
            {
                if ((child[i] & PTE_P) != 0)
                {
                    g_first_child_entries++;
                }
            }
        }
        // The canonical LOW half only. Slot 0 is never offered even where the boot root leaves
        // it absent: a space that could map it could map page zero, and a null pointer would
        // then be a legitimate address in it.
        size_t const low_slots = PTES / 2;
        for (size_t i = 1; i < low_slots; i++)
        {
            if (not slot_is_user(i))
            {
                continue;
            }
            if (g_user_lo == 0)
            {
                g_user_lo = static_cast<uintptr_t>(i) << shift_at(top);
            }
            g_user_hi = (static_cast<uintptr_t>(i) + 1) << shift_at(top);
        }
        if (g_user_hi == 0)
        {
            refuse("the adopted root leaves no top-level slot for a space to map");
        }
        invalidate_all();
    }

    unsigned aspace_levels(void)
    {
        return g_levels;
    }

    uintptr_t aspace_kernel_window(void)
    {
        return g_kwin_va;
    }

    size_t aspace_kernel_window_pages(void)
    {
        if (g_kwin_va == 0)
        {
            return 0;
        }
        return PTES;
    }

    bool aspace_kernel_map(size_t page, arch_phys_addr_t pa)
    {
        if (g_kwin_va == 0 or page >= PTES)
        {
            return false;
        }
        if (not phys_range_ok(pa, 1))
        {
            return false;
        }
        uint64_t memtype = 0;
        if (not memtype_bits(ARCH_MAP_NORMAL, &memtype))
        {
            return false;
        }
        arch_irq_state_t const s = arch_irq_save();
        uintptr_t const va = g_kwin_va + page * GRANULE;
        int const top = static_cast<int>(g_levels);
        // The chain is built on demand, so the first edit lands in the table the copied
        // top-level entry points at, which every space shares.
        for (int level = top - 1; level > LEVEL_LEAF; level--)
        {
            uint64_t* const table = &g_kwin_table[level - 1][0];
            size_t const idx = index_at(va, level);
            if ((table[idx] & PTE_P) == 0)
            {
                table[idx] = phys_of(&g_kwin_table[level - 2][0]) | PTE_P | PTE_RW;
            }
        }
        uint64_t* const leaves = &g_kwin_table[LEVEL_LEAF - 1][0];
        size_t const leaf = index_at(va, LEVEL_LEAF);
        if ((leaves[leaf] & PTE_P) != 0)
        {
            leaves[leaf] = 0;
            invalidate_page(va);
        }
        leaves[leaf] = (static_cast<uint64_t>(pa) & PTE_ADDR_MASK) | PTE_P | PTE_RW | PTE_A
                       | PTE_D | PTE_XD | memtype;
        invalidate_page(va);
        arch_irq_restore(s);
        return true;
    }

    bool aspace_kernel_unmap(size_t page)
    {
        if (g_kwin_va == 0 or page >= PTES)
        {
            return false;
        }
        arch_irq_state_t const s = arch_irq_save();
        uintptr_t const va = g_kwin_va + page * GRANULE;
        uint64_t* const leaves = &g_kwin_table[LEVEL_LEAF - 1][0];
        size_t const leaf = index_at(va, LEVEL_LEAF);
        if ((leaves[leaf] & PTE_P) == 0)
        {
            arch_irq_restore(s);
            return false;
        }
        leaves[leaf] = 0;
        invalidate_page(va);
        arch_irq_restore(s);
        return true;
    }

    uintptr_t aspace_user_lo(void)
    {
        return g_user_lo;
    }

    uintptr_t aspace_user_hi(void)
    {
        return g_user_hi;
    }

    unsigned aspace_kernel_slots(void)
    {
        return g_kernel_slots;
    }

    unsigned aspace_first_child_entries(void)
    {
        return g_first_child_entries;
    }

    arch_phys_addr_t aspace_root_installed(void)
    {
        return static_cast<arch_phys_addr_t>(read_cr3() & PTE_ADDR_MASK);
    }

    unsigned aspace_tag_bits(void)
    {
        uint32_t a = 0;
        uint32_t b = 0;
        uint32_t c = 0;
        uint32_t d = 0;
        // CPUID leaf 1, ECX bit 17 gates the control-register bit that enables the tag at all,
        // and the field is twelve bits wide where it is reported.
        cpuid_at(1, 0, &a, &b, &c, &d);
        if ((c & (1u << 17)) == 0)
        {
            return 0;
        }
        return 12;
    }

    bool aspace_tag_invalidate_present(void)
    {
        uint32_t a = 0;
        uint32_t b = 0;
        uint32_t c = 0;
        uint32_t d = 0;
        cpuid_at(0, 0, &a, &b, &c, &d);
        if (a < 7)
        {
            return false;
        }
        // Leaf 7, EBX bit 10. Read SEPARATELY from the tag above: a machine can report this one
        // and not that one.
        cpuid_at(7, 0, &a, &b, &c, &d);
        return (b & (1u << 10)) != 0;
    }

    // The walk arch_aspace_frame_at is built on, WITHOUT the range test: it reads any address
    // the walk can index. Nothing above the seam may call it.
    arch_phys_addr_t aspace_frame_at_unchecked(struct arch_aspace* space, uintptr_t va)
    {
        if (space == nullptr)
        {
            return 0;
        }
        // Masked for the reason the map unwind masks: that path clears leaves and frees tables
        // under one mask, and a walk interleaved with it would read a table already back in the
        // pool.
        arch_irq_state_t const s = arch_irq_save();
        arch_phys_addr_t frame = 0;
        if (not resolve(root_of(space), va & ~static_cast<uintptr_t>(GRANULE - 1), &frame))
        {
            frame = 0;
        }
        arch_irq_restore(s);
        return frame;
    }

    uint64_t aspace_leaf_desc(struct arch_aspace* space, uintptr_t va)
    {
        if (space == nullptr)
        {
            return 0;
        }
        arch_irq_state_t const s = arch_irq_save();
        uint64_t const* const entry =
            leaf_entry(root_of(space), va & ~static_cast<uintptr_t>(GRANULE - 1));
        uint64_t desc = 0;
        if (entry != nullptr)
        {
            desc = *entry;
        }
        arch_irq_restore(s);
        return desc;
    }

}

extern "C"
{

size_t arch_aspace_granule(void)
{
    return GRANULE;
}

uint64_t arch_aspace_model(void)
{
    // The same answer bounds every leaf the map path installs.
    unsigned const pa_bits = phys_addr_bits();
    // One granule whatever the level count, so bit 0 is the whole answer.
    uint64_t const granules = 1;
    unsigned const tag_bits = kickos::x86_64::aspace_tag_bits();
    uint64_t out = 0;
    if (GRANULE == 4096u)
    {
        out |= ARCH_ASPACE_MODEL_GRANULE;
    }
    if (tag_bits == TAG_BITS_RECORDED)
    {
        out |= ARCH_ASPACE_MODEL_ASID;
    }
    // The widest output address this port programs is the top of the conventional run the frame
    // pool is carved from, so a range covering that covers every leaf.
    if (pa_bits != 0 and (static_cast<arch_phys_addr_t>(1) << pa_bits) >= g_ram_hi)
    {
        out |= ARCH_ASPACE_MODEL_PA;
    }
    out |= static_cast<uint64_t>(tag_bits) << ARCH_ASPACE_MODEL_ASID_SHIFT;
    out |= static_cast<uint64_t>(pa_bits) << ARCH_ASPACE_MODEL_PA_SHIFT;
    out |= granules << ARCH_ASPACE_MODEL_GRAN_SHIFT;
    return out;
}

bool arch_aspace_memtype_support(enum arch_map_memtype type)
{
    uint64_t bits = 0;
    return memtype_bits(type, &bits);
}

struct arch_aspace* arch_aspace_create(void)
{
    if (g_boot_root == nullptr)
    {
        return nullptr;
    }
    arch_phys_addr_t const root = kickos_frame_alloc();
    if (root == 0)
    {
        return nullptr;
    }
    uint64_t* const table = table_at(root);
    // The boot root's TOP-LEVEL entries, so the kernel's fixed range is present and every space
    // shares the tables below them, which keeps a later kernel-half edit in step.
    for (size_t i = 0; i < PTES; i++)
    {
        table[i] = g_boot_root[i];
    }
    return reinterpret_cast<struct arch_aspace*>(table);
}

void arch_aspace_destroy(struct arch_aspace* space)
{
    if (space == nullptr)
    {
        return;
    }
    uint64_t* const table = root_of(space);
    // The boot space's tables are the FIRMWARE's and no frame of them came from the pool, so a
    // walk over them here would hand the allocator addresses it never issued.
    if (table == g_boot_root)
    {
        return;
    }
    // FIRST, not last: a walk caches the address of an intermediate table, so a table freed
    // while such an entry stands would be read as descriptors after the pool hands it out as
    // data.
    invalidate_all();
    free_subtree(table, static_cast<int>(g_levels), g_boot_root);
    kickos_frame_free(phys_of(table));
}

enum arch_aspace_result arch_aspace_map(struct arch_aspace* space, uintptr_t va,
                                        arch_phys_addr_t pa, size_t pages, uint32_t rights,
                                        enum arch_map_memtype type)
{
    if (space == nullptr or not range_ok(va, pages))
    {
        return ARCH_ASPACE_EINVAL;
    }
    // The boot space's top-level entries are what every other space COPIES, so an edit here
    // would silently join the kernel half of every space created afterwards.
    if (root_of(space) == g_boot_root)
    {
        return ARCH_ASPACE_EINVAL;
    }
    // The whole run, and BEFORE the first table is edited: a failed map installs no partial
    // mapping, so an endpoint refused part way through would be a promise broken.
    if (not phys_range_ok(pa, pages))
    {
        return ARCH_ASPACE_EINVAL;
    }
    uint64_t leaf = 0;
    if (not leaf_attrs(rights, type, &leaf))
    {
        return ARCH_ASPACE_EINVAL;
    }
    // A space installed on no core has no cached translation, so its whole seeding costs no
    // maintenance; the running space's own widening still pays.
    bool const installed = installed_here(space);
    enum arch_aspace_result const rc =
        map_into(root_of(space), static_cast<int>(g_levels), va, pages, pa, leaf, installed);
    if (rc != ARCH_ASPACE_OK)
    {
        // Masked across the whole unwind: this space can be the running one, the self-grant
        // widening it mid-syscall, and a walk between the invalidate and the frees would cache a
        // table about to go back to the pool.
        arch_irq_state_t const s = arch_irq_save();
        // Leaves first and tables second: an entry left present over a freed table would be a
        // walk into the pool. Installation runs in address order, so the first page with no leaf
        // ends the rollback.
        for (size_t i = 0; i < pages; i++)
        {
            uint64_t* const entry =
                leaf_entry(root_of(space), va + static_cast<uintptr_t>(i) * GRANULE);
            if (entry == nullptr)
            {
                break;
            }
            *entry = 0;
        }
        // One sweep each side of the frees.
        invalidate_all();
        (void)prune_empty(root_of(space), static_cast<int>(g_levels), g_boot_root);
        invalidate_all();
        arch_irq_restore(s);
    }
    return rc;
}

enum arch_aspace_result arch_aspace_unmap(struct arch_aspace* space, uintptr_t va, size_t pages)
{
    if (space == nullptr or not range_ok(va, pages))
    {
        return ARCH_ASPACE_EINVAL;
    }
    if (root_of(space) == g_boot_root)
    {
        return ARCH_ASPACE_EINVAL;
    }
    for (size_t i = 0; i < pages; i++)
    {
        if (leaf_entry(root_of(space), va + static_cast<uintptr_t>(i) * GRANULE) == nullptr)
        {
            return ARCH_ASPACE_EINVAL; // not wholly mapped, and nothing has been cleared
        }
    }
    bool const installed = installed_here(space);
    for (size_t i = 0; i < pages; i++)
    {
        uintptr_t const at = va + static_cast<uintptr_t>(i) * GRANULE;
        uint64_t* const entry = leaf_entry(root_of(space), at);
        *entry = 0;
        invalidate_page_if(at, installed);
    }
    // Destroy walks the tree and frees every table under the root, so an intermediate table
    // left empty here is reclaimed.
    return ARCH_ASPACE_OK;
}

void arch_aspace_activate(struct arch_aspace* space)
{
    if (space == nullptr)
    {
        return;
    }
    // One root register, so this write moves the running translation KERNEL HALF INCLUDED.
    // Every space carries the same top-level entries for the image, the conventional memory and
    // the kernel window, so the code making this write keeps its own mappings across it.
    //
    // The write IS the sweep, dropping every non-global translation and every
    // paging-structure-cache entry for identifier 0 (section 5.10.4.1). Masked because the two
    // stores cannot be one instruction.
    arch_irq_state_t const s = arch_irq_save();
    write_cr3(phys_of(root_of(space)));
    arch_irq_restore(s);
}

// The space the boot path installed (arch.h, arch_aspace_boot). Its tables are the FIRMWARE's.
struct arch_aspace* arch_aspace_boot(void)
{
    return reinterpret_cast<struct arch_aspace*>(g_boot_root);
}

// An addition, so any number are live at once.
static_assert(ARCH_ASPACE_ACQUIRE_MIN <= ACQUIRE_CAPACITY,
              "this backend cannot hold as many acquires live as arch.h promises");

void* arch_aspace_acquire(struct arch_aspace* space, uintptr_t va)
{
    if (space == nullptr)
    {
        return nullptr;
    }
    uintptr_t const page = va & ~static_cast<uintptr_t>(GRANULE - 1);
    if (not range_ok(page, 1))
    {
        return nullptr; // an address no space may map, which is range_ok's own boundary
    }
    uintptr_t const off = va & static_cast<uintptr_t>(GRANULE - 1);
    arch_irq_state_t const s = arch_irq_save();
    arch_phys_addr_t frame = 0;
    bool const mapped = resolve(root_of(space), va & ~static_cast<uintptr_t>(GRANULE - 1),
                                &frame);
    bool reachable = false;
    if (mapped)
    {
        reachable = identity_maps(frame);
    }
    arch_irq_restore(s);
    if (not reachable)
    {
        return nullptr;
    }
    return reinterpret_cast<void*>(static_cast<uintptr_t>(frame) + off);
}

void arch_aspace_release(struct arch_aspace* space, uintptr_t va)
{
    (void)space;
    (void)va;
}

// The seam member (arch.h). The range test is range_ok's, the one this backend's map and unmap
// ask: the kernel here lives LOW, in the top-level slots the firmware's regime already has
// present.
arch_phys_addr_t arch_aspace_frame_at(struct arch_aspace* space, uintptr_t va)
{
    if (space == nullptr)
    {
        return 0;
    }
    uintptr_t const page = va & ~static_cast<uintptr_t>(GRANULE - 1);
    if (not range_ok(page, 1))
    {
        return 0;
    }
    return kickos::x86_64::aspace_frame_at_unchecked(space, page);
}

#if defined(KICKOS_ENABLE_SELFTEST)
uint64_t arch_aspace_tlbi_counts(void)
{
    uint32_t elided = g_tlbi_elided;
    if (elided > 0xFFFFFFu)
    {
        elided = 0xFFFFFFu; // saturates rather than bleeding into the issued half
    }
    // The low byte is the window-release mispairing count; acquire here is an addition and
    // spends no slot.
    return (static_cast<uint64_t>(g_tlbi_issued) << 32) | (static_cast<uint64_t>(elided) << 8);
}

uint32_t arch_aspace_active_cores(struct arch_aspace* space)
{
    if (space == nullptr)
    {
        return 0;
    }
    if (not installed_here(space))
    {
        return 0;
    }
    return 1u << arch_cpu_id();
}
#endif

}
