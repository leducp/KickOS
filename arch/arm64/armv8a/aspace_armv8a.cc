// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The AArch64 stage-1 map editor: the arch_aspace_* family of arch.h over VMSAv8-64
// translation tables (DDI 0487 M.b chapter D8). TTBR0_EL1 alone is edited here; TTBR1_EL1
// holds the kernel half and the map of physical RAM that startup.S built.
//
// TCR_EL1.T0SZ is 25 (startup.S), so a 39-bit low half whose walk starts at level 1: the
// root is the level-1 table and the leaves sit at level 3.

#include <kickos/arch/arch.h>
#include <kickos/extent.h>

#include <stddef.h>
#include <stdint.h>

extern "C"
{
    // Chip<->arch contract (arch/arm64/chip/virt_arm64/virt_arm64.ld). NOT weak: a chip
    // whose script omits it must fail the link rather than translate against a zero base.
    extern unsigned char __kickos_arm64_va_base[];

    arch_phys_addr_t kickos_frame_alloc(void);
    void kickos_frame_free(arch_phys_addr_t frame);
}

namespace
{
    // TCR_EL1.TG0 selects the granule and startup.S programs 4 KiB. Every table size and
    // index width below follows from it.
    constexpr unsigned GRANULE_SHIFT = 12;
    constexpr size_t GRANULE = static_cast<size_t>(1) << GRANULE_SHIFT;
    constexpr size_t PTES = GRANULE / sizeof(uint64_t);
    constexpr int LEVEL_ROOT = 1;
    constexpr int LEVEL_LEAF = 3;

    // Acquire is an addition and spends no window, so nothing here bounds the live holds.
    constexpr size_t ACQUIRE_CAPACITY = SIZE_MAX;

    // Descriptor fields, Table D8-50 and Table D8-52 (DDI 0487 M.b section D8.3).
    constexpr uint64_t DESC_VALID = 1ull << 0;
    constexpr uint64_t DESC_BIT1 = 1ull << 1; // table below level 3, page at level 3
    constexpr uint64_t DESC_AP_EL0 = 1ull << 6;
    constexpr uint64_t DESC_AP_RO = 1ull << 7;
    constexpr uint64_t DESC_SH_INNER = 3ull << 8;
    constexpr uint64_t DESC_AF = 1ull << 10;
    constexpr uint64_t DESC_NG = 1ull << 11;
    constexpr uint64_t DESC_PXN = 1ull << 53;
    constexpr uint64_t DESC_UXN = 1ull << 54;
    // Output address bits [47:12]; the A53 outputs 40 bits, so the top eight are always 0.
    constexpr uint64_t DESC_OA_MASK = 0x0000FFFFFFFFF000ull;
    // The widest output a descriptor of this format can carry, whatever the machine implements.
    constexpr unsigned DESC_OA_BITS = 48;

    // MAIR_EL1 attribute slots, in the order startup.S programs them.
    constexpr uint64_t ATTR_NORMAL = 0;
    constexpr uint64_t ATTR_DEVICE = 1;
    constexpr uint64_t ATTR_NOCACHE = 2;

    // T0SZ is read back from TCR_EL1; startup.S is what programmed it.
    unsigned va_bits()
    {
        uint64_t tcr = 0;
        __asm volatile("mrs %0, tcr_el1" : "=r"(tcr));
        return 64u - static_cast<unsigned>(tcr & 0x3Fu);
    }

    uintptr_t va_base()
    {
        return reinterpret_cast<uintptr_t>(__kickos_arm64_va_base);
    }

    // ID_AA64MMFR0_EL1 and TCR_EL1.IPS share one encoding of the physical address range
    // (DDI 0487 M.b, ID_AA64MMFR0_EL1.PARange). Anything past the table reads as 0, never
    // as a guess.
    unsigned pa_bits_of(unsigned field)
    {
        constexpr unsigned char BITS[] = {32, 36, 40, 42, 44, 48, 52, 56};
        if (field >= sizeof(BITS) / sizeof(BITS[0]))
        {
            return 0;
        }
        return BITS[field];
    }

    // The output width a leaf may carry: the narrowest of what the machine implements (PARange),
    // what this regime outputs (TCR_EL1.IPS, startup.S) and what the descriptor format encodes.
    // An output above the IPS size takes an Address Size Fault, and one above the field is
    // truncated by map_into's mask. Zero where either encoding is reserved.
    unsigned oa_bits()
    {
        uint64_t mmfr0 = 0;
        __asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
        uint64_t tcr = 0;
        __asm volatile("mrs %0, tcr_el1" : "=r"(tcr));
        unsigned bits = pa_bits_of(static_cast<unsigned>(mmfr0 & 0xFu));
        unsigned const ips = pa_bits_of(static_cast<unsigned>((tcr >> 32) & 0x7u));
        if (ips < bits)
        {
            bits = ips;
        }
        if (bits > DESC_OA_BITS)
        {
            bits = DESC_OA_BITS;
        }
        return bits;
    }

    // TCR_EL1.AS is left at 0, an 8-bit identifier, nothing here tagging a translation; 16
    // is the figure the record carries, not one the port programs (DDI 0500J section
    // 4.3.21, Table 4-56).
    constexpr unsigned ASID_BITS_RECORDED = 16;

    // The high-half map of all physical RAM, which is what makes acquire an addition rather
    // than a window (arch.h, arch_aspace_acquire).
    uint64_t* table_at(arch_phys_addr_t pa)
    {
        return reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(pa) + va_base());
    }

    arch_phys_addr_t phys_of(void const* p)
    {
        return static_cast<arch_phys_addr_t>(reinterpret_cast<uintptr_t>(p) - va_base());
    }

    uint64_t* root_of(struct arch_aspace* space)
    {
        return reinterpret_cast<uint64_t*>(space);
    }

    size_t index_at(uintptr_t va, int level)
    {
        unsigned const shift = GRANULE_SHIFT + static_cast<unsigned>(LEVEL_LEAF - level) * 9u;
        return static_cast<size_t>((va >> shift) & (PTES - 1));
    }

    // Bytes one entry at `level` spans: 1 GiB at level 1, 2 MiB at level 2, 4 KiB at 3.
    uintptr_t span_at(int level)
    {
        unsigned const shift = GRANULE_SHIFT + static_cast<unsigned>(LEVEL_LEAF - level) * 9u;
        return static_cast<uintptr_t>(1) << shift;
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // Page-invalidation sequences issued, and the ones a not-installed space skipped. Every
    // writer holds the caller's IrqLock, so these need no ordering of their own.
    uint32_t g_tlbi_issued = 0;
    uint32_t g_tlbi_elided = 0;
#endif

    // The descriptor write must reach the walker before the invalidate, and the invalidate
    // must complete before the next translated access (DDI 0487 M.b section D8.17).
    void invalidate_page(uintptr_t va)
    {
#if defined(KICKOS_ENABLE_SELFTEST)
        g_tlbi_issued++;
#endif
        __asm volatile("dsb ishst" ::: "memory");
        // VAAE1 and not VAE1: nothing here tags a translation, so an entry must be dropped
        // whatever ASID it was cached under.
        __asm volatile("tlbi vaae1, %0" ::"r"(va >> GRANULE_SHIFT) : "memory");
        __asm volatile("dsb ish" ::: "memory");
        __asm volatile("isb" ::: "memory");
    }

    // Whether this core's TTBR0 names `space`. The register holds nothing but the root's
    // output address here: TCR_EL1.A1 is 0 and no identifier is assigned, so the mask below
    // covers every field write_ttbr0 sets.
    bool installed_here(struct arch_aspace* space)
    {
        uint64_t ttbr = 0;
        __asm volatile("mrs %0, ttbr0_el1" : "=r"(ttbr));
        return (ttbr & DESC_OA_MASK) == static_cast<uint64_t>(phys_of(root_of(space)));
    }

    // Drop the entry for `va` only where the walker can hold one. Nothing tags a translation, so
    // write_ttbr0 drops the whole low half on every root change, and no entry for a space this
    // core does not have installed can survive that. A second core holds a TTBR0 this one cannot
    // read, so the elision is compiled out above one core.
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

    // A range sweep: a cleared table entry spans up to 1 GiB, which no by-address
    // invalidate covers.
    void invalidate_all()
    {
        __asm volatile("dsb ishst" ::: "memory");
        __asm volatile("tlbi vmalle1" ::: "memory");
        __asm volatile("dsb ish" ::: "memory");
        __asm volatile("isb" ::: "memory");
    }

    // The root the boot path installed, put back by the fault reporter: every device this
    // chip's reporter touches is a low address, so a fault taken under a user space would
    // otherwise print nothing at all.
    uint64_t g_boot_ttbr0 = 0;

    void capture_boot()
    {
        if (g_boot_ttbr0 == 0)
        {
            __asm volatile("mrs %0, ttbr0_el1" : "=r"(g_boot_ttbr0));
        }
    }

    void write_ttbr0(uint64_t ttbr)
    {
        __asm volatile("msr ttbr0_el1, %0" ::"r"(ttbr) : "memory");
        __asm volatile("isb" ::: "memory");
        __asm volatile("tlbi vmalle1" ::: "memory");
        __asm volatile("dsb ish" ::: "memory");
        __asm volatile("isb" ::: "memory");
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

    bool memtype_attr(enum arch_map_memtype type, uint64_t* out)
    {
        if (type == ARCH_MAP_NORMAL)
        {
            *out = (ATTR_NORMAL << 2) | DESC_SH_INNER;
            return true;
        }
        if (type == ARCH_MAP_NOCACHE)
        {
            // Normal non-cacheable behaves as outer shareable whatever SH says, so the
            // field is left at 0.
            *out = (ATTR_NOCACHE << 2);
            return true;
        }
        if (type == ARCH_MAP_DEVICE)
        {
            *out = (ATTR_DEVICE << 2);
            return true;
        }
        return false;
    }

    // A leaf for the unprivileged level. AP has no read-disable and no execute-only form
    // (Table D8-63), so a request without ARCH_MAP_R is refused.
    bool leaf_attrs(uint32_t rights, enum arch_map_memtype type, uint64_t* out)
    {
        uint32_t const known = ARCH_MAP_R | ARCH_MAP_W | ARCH_MAP_X;
        if ((rights & ~known) != 0 or (rights & ARCH_MAP_R) == 0)
        {
            return false;
        }
        // Writable and executable at EL0 is expressible here and refused: a page granted both
        // is one an unprivileged thread can turn into code.
        if ((rights & ARCH_MAP_W) != 0 and (rights & ARCH_MAP_X) != 0)
        {
            return false;
        }
        uint64_t attr = 0;
        if (not memtype_attr(type, &attr))
        {
            return false;
        }
        // nG set: the entry belongs to one space, though nothing here assigns a tag yet.
        uint64_t desc = DESC_VALID | DESC_BIT1 | DESC_AF | DESC_NG | DESC_AP_EL0 | attr;
        if ((rights & ARCH_MAP_W) == 0)
        {
            desc |= DESC_AP_RO;
        }
        if ((rights & ARCH_MAP_X) == 0)
        {
            desc |= DESC_UXN;
        }
        // A page reachable at EL0 is never privileged-executable: the kernel executes out
        // of the high half alone.
        desc |= DESC_PXN;
        *out = desc;
        return true;
    }

    void free_subtree(uint64_t* table, int level)
    {
        for (size_t i = 0; i < PTES; i++)
        {
            uint64_t const desc = table[i];
            if ((desc & DESC_VALID) == 0)
            {
                continue;
            }
            arch_phys_addr_t const out = static_cast<arch_phys_addr_t>(desc & DESC_OA_MASK);
            // A block descriptor at a non-leaf level is an output, not a table, and
            // recursing into one would read its frame as descriptors.
            if (level < LEVEL_LEAF and (desc & DESC_BIT1) != 0)
            {
                free_subtree(table_at(out), level + 1);
            }
            // A leaf output the pool never handed out is refused inside the free, which is
            // what lets a space hold a device page without destroy reclaiming it.
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
                if ((table[idx] & DESC_VALID) != 0)
                {
                    // Break-before-make: the invalidate belongs BETWEEN the two writes, or
                    // both descriptors are live at once and a walk may take fields from
                    // each.
                    table[idx] = 0;
                    invalidate_page_if(va, installed);
                }
                table[idx] = leaf | (static_cast<uint64_t>(pa) & DESC_OA_MASK);
                // A64 caches no faulting entry, so an invalid-to-valid change needs no
                // invalidation (DDI 0487 M.b, D8.17, IWZCBG). This one covers the slot that was
                // not in fact empty, which nothing above proves.
                invalidate_page_if(va, installed);
                va += GRANULE;
                pa += GRANULE;
                pages--;
                continue;
            }

            uint64_t desc = table[idx];
            if ((desc & DESC_VALID) == 0)
            {
                arch_phys_addr_t const frame = kickos_frame_alloc();
                if (frame == 0)
                {
                    return ARCH_ASPACE_ENOMEM;
                }
                zero_table(table_at(frame));
                desc = static_cast<uint64_t>(frame) | DESC_VALID | DESC_BIT1;
                __asm volatile("dsb ishst" ::: "memory");
                table[idx] = desc;
                // No invalidate for the table entry itself: an invalidate by address drops the
                // cached intermediate entries for that address too, so the per-leaf one below
                // covers every page this call makes reachable.
            }

            uintptr_t const span = span_at(level);
            uintptr_t const next = (va + span) & ~(span - 1);
            size_t const here_max = static_cast<size_t>((next - va) / GRANULE);
            size_t here = pages;
            if (here > here_max)
            {
                here = here_max;
            }
            arch_phys_addr_t const child_pa = static_cast<arch_phys_addr_t>(desc & DESC_OA_MASK);
            enum arch_aspace_result const rc =
                map_into(table_at(child_pa), level + 1, va, here, pa, leaf, installed);
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

    // Returns true when `table` is empty once its empty children are gone. Every table a failed
    // map allocated is empty by then, the leaf rollback having run first.
    bool prune_empty(uint64_t* table, int level)
    {
        if (level == LEVEL_LEAF)
        {
            return table_empty(table);
        }
        for (size_t i = 0; i < PTES; i++)
        {
            uint64_t const desc = table[i];
            if ((desc & DESC_VALID) == 0)
            {
                continue;
            }
            arch_phys_addr_t const child = static_cast<arch_phys_addr_t>(desc & DESC_OA_MASK);
            if (prune_empty(table_at(child), level + 1))
            {
                table[i] = 0;
                kickos_frame_free(child);
            }
        }
        return table_empty(table);
    }

    // Null when the range is not wholly mapped, which is what makes unmap total-or-fail.
    uint64_t* leaf_entry(uint64_t* table, uintptr_t va)
    {
        for (int level = LEVEL_ROOT; level < LEVEL_LEAF; level++)
        {
            uint64_t const desc = table[index_at(va, level)];
            if ((desc & DESC_VALID) == 0 or (desc & DESC_BIT1) == 0)
            {
                return nullptr;
            }
            table = table_at(static_cast<arch_phys_addr_t>(desc & DESC_OA_MASK));
        }
        uint64_t* const entry = &table[index_at(va, LEVEL_LEAF)];
        if ((*entry & DESC_VALID) == 0)
        {
            return nullptr;
        }
        return entry;
    }

    // TTBR0's half only, and the single statement of that boundary every guard below asks. The
    // walk reads the index bits of the address alone, so a high-half address ALIASES onto a
    // low-half one and would otherwise be answered with that page's frame.
    bool low_half_page(uintptr_t page)
    {
        return page < (static_cast<uintptr_t>(1) << va_bits());
    }

    // The whole output extent, alignment included, asked before the first table is edited:
    // map_into increments pa per page and masks each leaf with DESC_OA_MASK, so a run validated
    // by its start alone runs off the implemented output width and comes back aliased onto a low
    // frame.
    bool phys_range_ok(arch_phys_addr_t pa, size_t pages)
    {
        if ((pa & ~DESC_OA_MASK) != 0)
        {
            return false;
        }
        unsigned const bits = oa_bits();
        if (bits == 0)
        {
            return false;
        }
        uintptr_t end = 0;
        if (not kickos::extent_end(static_cast<uintptr_t>(pa), pages, GRANULE, &end))
        {
            return false; // 0 pages, a byte count past the pointer width, or a wrapped end
        }
        return static_cast<uint64_t>(end) <= (static_cast<uint64_t>(1) << bits);
    }

    bool range_ok(uintptr_t va, size_t pages)
    {
        if ((va & (GRANULE - 1)) != 0)
        {
            return false;
        }
        uintptr_t end = 0;
        if (not kickos::extent_end(va, pages, GRANULE, &end))
        {
            return false; // 0 pages, a byte count past the pointer width, or a wrapped end
        }
        // end is exclusive, and extent_end refused a zero-page range, so end - 1 is the last
        // byte the range covers and cannot underflow.
        return low_half_page(end - 1);
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
    uint64_t mmfr0 = 0;
    __asm volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    uint64_t tcr = 0;
    __asm volatile("mrs %0, tcr_el1" : "=r"(tcr));
    // TGran4 at 31:28, TGran64 at 27:24, TGran16 at 23:20, ASIDBits at 7:4, PARange at 3:0.
    //
    // TGran16 states the sense of its answer the opposite way round from the other two: 0b0000
    // means NOT supported there, where for TGran4 and TGran64 it means supported.
    unsigned const tg4 = static_cast<unsigned>((mmfr0 >> 28) & 0xFu);
    unsigned const tg64 = static_cast<unsigned>((mmfr0 >> 24) & 0xFu);
    unsigned const tg16 = static_cast<unsigned>((mmfr0 >> 20) & 0xFu);
    uint64_t granules = 0;
    if (tg4 != 0xFu)
    {
        granules |= 1u; // the architecture's smallest, and the one TCR_EL1.TG0 selects here
    }
    if (tg16 != 0u)
    {
        granules |= 2u;
    }
    if (tg64 != 0xFu)
    {
        granules |= 4u;
    }
    unsigned asid_bits = 8;
    if (((mmfr0 >> 4) & 0xFu) == 2u)
    {
        asid_bits = 16;
    }
    unsigned const pa_bits = pa_bits_of(static_cast<unsigned>(mmfr0 & 0xFu));
    // Read back from TCR_EL1.IPS, which startup.S programmed: this port may only write a
    // range the machine has.
    unsigned const ips_bits = pa_bits_of(static_cast<unsigned>((tcr >> 32) & 0x7u));
    uint64_t out = 0;
    if ((granules & 1u) != 0 and GRANULE == 4096u)
    {
        out |= ARCH_ASPACE_MODEL_GRANULE;
    }
    if (asid_bits == ASID_BITS_RECORDED)
    {
        out |= ARCH_ASPACE_MODEL_ASID;
    }
    if (pa_bits != 0 and ips_bits != 0 and pa_bits >= ips_bits)
    {
        out |= ARCH_ASPACE_MODEL_PA;
    }
    out |= static_cast<uint64_t>(asid_bits) << ARCH_ASPACE_MODEL_ASID_SHIFT;
    out |= static_cast<uint64_t>(pa_bits) << ARCH_ASPACE_MODEL_PA_SHIFT;
    out |= granules << ARCH_ASPACE_MODEL_GRAN_SHIFT;
    return out;
}

bool arch_aspace_memtype_support(enum arch_map_memtype type)
{
    uint64_t attr = 0;
    return memtype_attr(type, &attr);
}

struct arch_aspace* arch_aspace_create(void)
{
    arch_phys_addr_t const root = kickos_frame_alloc();
    if (root == 0)
    {
        return nullptr;
    }
    uint64_t* const table = table_at(root);
    zero_table(table);
    __asm volatile("dsb ishst" ::: "memory");
    // No kernel half is copied in: this architecture selects the table from the top bits of the
    // address, so the kernel window is TTBR1's. The handle is the root table's address.
    return reinterpret_cast<struct arch_aspace*>(table);
}

void arch_aspace_destroy(struct arch_aspace* space)
{
    if (space == nullptr)
    {
        return;
    }
    uint64_t* const table = root_of(space);
    // FIRST, not last: a walk caches the address of an intermediate table, so a table freed
    // while such an entry stands would be read as descriptors after the pool reissues it.
    invalidate_all();
    free_subtree(table, LEVEL_ROOT);
    kickos_frame_free(phys_of(table));
}

enum arch_aspace_result arch_aspace_map(struct arch_aspace* space, uintptr_t va,
                                        arch_phys_addr_t pa, size_t pages,
                                        uint32_t rights, enum arch_map_memtype type)
{
    if (space == nullptr or not range_ok(va, pages))
    {
        return ARCH_ASPACE_EINVAL;
    }
    if (not phys_range_ok(pa, pages))
    {
        return ARCH_ASPACE_EINVAL; // misaligned, or an extent past the implemented output width
    }
    uint64_t leaf = 0;
    if (not leaf_attrs(rights, type, &leaf))
    {
        return ARCH_ASPACE_EINVAL;
    }
    // A space installed on no core has no cached entry and no cached absence, so its whole
    // seeding costs no maintenance; the running space's own widening still pays.
    bool const installed = installed_here(space);
    enum arch_aspace_result const rc =
        map_into(root_of(space), LEVEL_ROOT, va, pages, pa, leaf, installed);
    if (rc != ARCH_ASPACE_OK)
    {
        // Masked across the whole unwind: this space can be the running one, the self-grant
        // widening it mid-syscall, and a walk between the invalidate and the frees would
        // cache a table about to go back to the pool.
        arch_irq_state_t const s = arch_irq_save();
        // Leaves first and tables second: an entry left valid over a freed table would be a
        // walk into the pool. Installation runs in address order, so the first page with no
        // leaf ends the rollback.
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
        (void)prune_empty(root_of(space), LEVEL_ROOT);
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
    // An intermediate table left empty is not a leak: destroy walks the tree and frees
    // every table under the root.
    return ARCH_ASPACE_OK;
}

void arch_aspace_activate(struct arch_aspace* space)
{
    uint64_t const ttbr = static_cast<uint64_t>(phys_of(root_of(space)));
    // No translation tag, so every space is cached under the same identifier and the whole low
    // half has to be dropped on every switch. Masked because the drop cannot be atomic with the
    // base change: between the two, a low-half access would resolve against the outgoing
    // space.
    arch_irq_state_t const s = arch_irq_save();
    capture_boot();
    write_ttbr0(ttbr);
    arch_irq_restore(s);
}

// See g_boot_ttbr0.
void kickos_armv8a_ttbr0_to_boot(void)
{
    if (g_boot_ttbr0 != 0)
    {
        write_ttbr0(g_boot_ttbr0);
    }
}

// The space the boot path installed (arch.h, arch_aspace_boot).
struct arch_aspace* arch_aspace_boot(void)
{
    arch_irq_state_t const s = arch_irq_save();
    capture_boot();
    arch_irq_restore(s);
    return reinterpret_cast<struct arch_aspace*>(
        table_at(static_cast<arch_phys_addr_t>(g_boot_ttbr0 & DESC_OA_MASK)));
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
    uintptr_t const page = va & ~(GRANULE - 1);
    // The half test arch_aspace_frame_at makes: a frame this walk reached through TTBR1's own
    // entries would be handed back as a POINTER the caller may write through.
    if (not low_half_page(page))
    {
        return nullptr;
    }
    // Masked for the reason arch_aspace_frame_at is: the walk reads tables the map unwind can
    // be handing back to the frame pool.
    arch_irq_state_t const s = arch_irq_save();
    uint64_t const* const entry = leaf_entry(root_of(space), page);
    arch_phys_addr_t out = 0;
    if (entry != nullptr)
    {
        out = static_cast<arch_phys_addr_t>(*entry & DESC_OA_MASK);
    }
    arch_irq_restore(s);
    if (entry == nullptr)
    {
        return nullptr;
    }
    return reinterpret_cast<void*>(static_cast<uintptr_t>(out) + va_base() +
                                   (va & (GRANULE - 1)));
}

void arch_aspace_release(struct arch_aspace* space, uintptr_t va)
{
    (void)space;
    (void)va;
}

arch_phys_addr_t arch_aspace_frame_at(struct arch_aspace* space, uintptr_t va)
{
    if (space == nullptr)
    {
        return 0;
    }
    uintptr_t const page = va & ~(GRANULE - 1);
    // MAP AND UNMAP'S OWN HALF TEST, and aligned down first because this member's `va` need
    // not be granule-aligned where range_ok's is: see low_half_page for the aliasing this
    // refuses.
    if (not low_half_page(page))
    {
        return 0;
    }
    // Masked for the same reason the map unwind masks: it clears leaves and frees tables under
    // one mask, and a walk interleaved with it would read a table already back in the pool.
    arch_irq_state_t const s = arch_irq_save();
    uint64_t const* const entry = leaf_entry(root_of(space), page);
    arch_phys_addr_t out = 0;
    if (entry != nullptr)
    {
        out = static_cast<arch_phys_addr_t>(*entry & DESC_OA_MASK);
    }
    arch_irq_restore(s);
    return out;
}

#if defined(KICKOS_ENABLE_SELFTEST)
uint64_t arch_aspace_tlbi_counts(void)
{
    uint32_t elided = g_tlbi_elided;
    if (elided > 0xFFFFFFu)
    {
        elided = 0xFFFFFFu; // saturates rather than bleeding into the issued half
    }
    // The low byte is the window-release mispairing count, which this backend cannot produce:
    // acquire here is an addition and spends no slot, so release holds nothing to mispair.
    return (static_cast<uint64_t>(g_tlbi_issued) << 32) | (static_cast<uint64_t>(elided) << 8);
}
#endif

}
