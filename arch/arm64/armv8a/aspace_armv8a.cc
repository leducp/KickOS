// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// AArch64 stage-1 page tables (DDI 0487 M.b, chapter D8).
// TTBR0 maps userspace; TTBR1 maps the kernel and physical RAM.
// T0SZ=25 selects a 39-bit address space with levels 1 through 3.

#include <kickos/arch/arch.h>
#include <kickos/arch/aspace_residency.h>
#include <kickos/arch/aspace_table.h>
#include <kickos/extent.h>

#include "sysops_armv8a.h"

#include <stddef.h>
#include <stdint.h>

extern "C"
{
    // Required linker symbol for the physical RAM mapping.
    extern unsigned char __kickos_arm64_va_base[];

    arch_phys_addr_t kickos_frame_alloc(void);
    void kickos_frame_free(arch_phys_addr_t frame);

#if KICKOS_KERNEL_CORES > 1
    void kickos_arm64_instruction_side_rendezvous(uint32_t peers);
#endif
}

namespace
{
    // Table sizes and index widths use the 4 KiB granule configured in startup.S.
    constexpr unsigned GRANULE_SHIFT = 12;
    constexpr size_t GRANULE = static_cast<size_t>(1) << GRANULE_SHIFT;
    constexpr size_t PTES = GRANULE / sizeof(uint64_t);
    constexpr int LEVEL_ROOT = 1;
    constexpr int LEVEL_LEAF = 3;

    // Acquire uses the direct RAM mapping and needs no temporary slots.
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

    unsigned va_bits()
    {
        uint64_t const tcr = kickos_armv8a_read_tcr_el1();
        return 64u - static_cast<unsigned>(tcr & 0x3Fu);
    }

    uintptr_t va_base()
    {
        return reinterpret_cast<uintptr_t>(__kickos_arm64_va_base);
    }

    // PARange and TCR.IPS use the same encoding. Return zero for reserved values
    // (DDI 0487 M.b, ID_AA64MMFR0_EL1.PARange).
    unsigned pa_bits_of(unsigned field)
    {
        constexpr unsigned char BITS[] = {32, 36, 40, 42, 44, 48, 52, 56};
        if (field >= sizeof(BITS) / sizeof(BITS[0]))
        {
            return 0;
        }
        return BITS[field];
    }

    // Limit outputs to the hardware, TCR.IPS and descriptor widths.
    // Return zero for reserved encodings.
    unsigned oa_bits()
    {
        uint64_t const mmfr0 = kickos_armv8a_read_mmfr0_el1();
        uint64_t const tcr = kickos_armv8a_read_tcr_el1();
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

    constexpr unsigned ASID_SHIFT = 48;
    constexpr uint64_t ASID_MASK = 0xFFFFull << ASID_SHIFT;
    constexpr uint64_t TCR_AS = 1ull << 36;
    constexpr unsigned ASID_BITS_RECORDED = 16;

    // Read the effective ASID width from ASIDBits and TCR.AS.
    // ASIDBits encodes 8 bits as 0 and 16 bits as 2; other values disable tagging.
    // TCR.AS is RES0 on 8-bit parts. TTBR0 readback does not indicate this limit.
    unsigned asid_width()
    {
        unsigned const field = static_cast<unsigned>((kickos_armv8a_read_mmfr0_el1() >> 4) & 0xFu);
        if (field == 0u)
        {
            return 8;
        }
        if (field != 2u)
        {
            return 0;
        }
        if ((kickos_armv8a_read_tcr_el1() & TCR_AS) == 0)
        {
            return 8;
        }
        return ASID_BITS_RECORDED;
    }

    // Reserve ASID 0 for untagged roots. Residency also limits IDs to its row count.
    uint32_t asid_capacity()
    {
        unsigned const width = asid_width();
        if (width == 0)
        {
            return 0;
        }
        return (1u << width) - 1u;
    }

    uint32_t asid_of(uint64_t ttbr)
    {
        return static_cast<uint32_t>((ttbr & ASID_MASK) >> ASID_SHIFT);
    }

    // Physical RAM is directly mapped in the kernel half.
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
    // Invalidation counters, protected by the caller's IrqLock.
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
        kickos_armv8a_dsb_ishst();
        // The IS form reaches all PEs in the Inner Shareable domain; the local form
        // reaches this PE. DSB ISH completes either (DDI 0487 M.b, D8.17.5 and B2.6.9.1).
#if KICKOS_KERNEL_CORES > 1
        kickos_armv8a_tlbi_page_is(va >> GRANULE_SHIFT);
#else
        kickos_armv8a_tlbi_page_local(va >> GRANULE_SHIFT);
#endif
        kickos_armv8a_dsb_ish();
        kickos_armv8a_isb();
    }

#if KICKOS_KERNEL_CORES > 1
    // Last installed root per core. Updated only by write_ttbr0.
    uint64_t g_installed_root[KICKOS_NUM_CORES] = {};
#endif

    // Root-keyed ASIDs and cores that may retain translations.
    // write_ttbr0 records residency when it installs a root.
    kickos::aspace::Residency<KICKOS_NUM_CORES> g_residency;

    uint64_t root_key(struct arch_aspace* space)
    {
        return static_cast<uint64_t>(phys_of(root_of(space)));
    }

    uint32_t resident_cores(struct arch_aspace* space)
    {
        return g_residency.cores(root_key(space));
    }

    // Residency persists after switching away: tagged translations may remain cached.
    bool resident_anywhere(struct arch_aspace* space)
    {
        return resident_cores(space) != 0;
    }

#if KICKOS_KERNEL_CORES > 1
    uint32_t resident_peers(struct arch_aspace* space)
    {
        return resident_cores(space) & ~(1u << arch_cpu_id());
    }
#else
    uint32_t resident_peers(struct arch_aspace*)
    {
        return 0;
    }
#endif

#if defined(KICKOS_ENABLE_SELFTEST)
    // Compare root addresses without the ASID bits.
    bool installed_here(struct arch_aspace* space)
    {
        uint64_t const ttbr = kickos_armv8a_read_ttbr0_el1();
        return (ttbr & DESC_OA_MASK) == static_cast<uint64_t>(phys_of(root_of(space)));
    }

    // Cores currently using this root, for self-tests only.
    // Maintenance uses residency, which includes cores that switched away.
    uint32_t active_cores(struct arch_aspace* space)
    {
        uint32_t set = 0;
        if (installed_here(space))
        {
            set |= 1u << arch_cpu_id();
        }
#if KICKOS_KERNEL_CORES > 1
        uint64_t const oa = static_cast<uint64_t>(phys_of(root_of(space)));
        for (uint32_t c = 0; c < KICKOS_NUM_CORES; c++)
        {
            if (g_installed_root[c] == oa)
            {
                set |= 1u << c;
            }
        }
#endif
        return set;
    }
#endif

    // After removing executable mappings, each resident peer must execute an ISB
    // to discard previously fetched instructions. TLBI alone does not do this
    // (DDI 0487 M.b, B2.7.4.2). The doorbell handler supplies the peer ISB.
#if KICKOS_KERNEL_CORES > 1
    void instruction_side_rendezvous(uint32_t peers)
    {
        kickos_arm64_instruction_side_rendezvous(peers);
    }
#else
    void instruction_side_rendezvous(uint32_t)
    {
    }
#endif

    void invalidate_page_if(uintptr_t va, bool resident)
    {
        if (not resident)
        {
#if defined(KICKOS_ENABLE_SELFTEST)
            g_tlbi_elided++;
#endif
            return;
        }
        invalidate_page(va);
    }

    // Complete descriptor stores before a table walk, including on one core.
    // ISB only synchronizes context; it does not complete stores
    // (DDI 0487 M.b, D8.17.1).
    void publish_edits()
    {
        kickos_armv8a_dsb_ishst();
    }

    // Invalidate all addresses after removing table entries.
    void invalidate_all()
    {
        kickos_armv8a_dsb_ishst();
#if KICKOS_KERNEL_CORES > 1
        kickos_armv8a_tlbi_all_is();
#else
        kickos_armv8a_tlbi_all_local();
#endif
        kickos_armv8a_dsb_ish();
        kickos_armv8a_isb();
    }

    // The fault reporter needs the boot root's low-address device mappings.
    uint64_t g_boot_ttbr0 = 0;
    // Track capture separately because a boot TTBR0 value of zero is valid.
    bool g_boot_captured = false;

    void capture_boot()
    {
        if (not g_boot_captured)
        {
            g_boot_ttbr0 = kickos_armv8a_read_ttbr0_el1();
            g_boot_captured = true;
        }
    }

    uint64_t* boot_root()
    {
        return table_at(static_cast<arch_phys_addr_t>(g_boot_ttbr0 & DESC_OA_MASK));
    }

    // Call with interrupts masked. Flush locally when leaving ASID 0: its
    // translations are shared, and the boot root also has global low-half entries.
    // Switching between tagged roots needs no flush. Update both records after
    // writing TTBR0.
    void write_ttbr0(uint64_t ttbr)
    {
        uint64_t const leaving = kickos_armv8a_read_ttbr0_el1();
        kickos_armv8a_write_ttbr0_el1(ttbr);
        kickos_armv8a_isb();
        if (asid_of(leaving) == 0)
        {
            kickos_armv8a_tlbi_all_local();
            kickos_armv8a_dsb_ish();
            kickos_armv8a_isb();
        }
#if KICKOS_KERNEL_CORES > 1
        g_installed_root[arch_cpu_id()] = ttbr & DESC_OA_MASK;
#endif
        g_residency.note(ttbr & DESC_OA_MASK, arch_cpu_id());
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
        // Enforce W^X for userspace.
        if ((rights & ARCH_MAP_W) != 0 and (rights & ARCH_MAP_X) != 0)
        {
            return false;
        }
        uint64_t attr = 0;
        if (not memtype_attr(type, &attr))
        {
            return false;
        }
        // Tag user entries by ASID; TTBR1 kernel entries remain global.
        uint64_t desc = DESC_VALID | DESC_BIT1 | DESC_AF | DESC_NG | DESC_AP_EL0 | attr;
        if ((rights & ARCH_MAP_W) == 0)
        {
            desc |= DESC_AP_RO;
        }
        if ((rights & ARCH_MAP_X) == 0)
        {
            desc |= DESC_UXN;
        }
        // Never allow privileged execution of user pages.
        desc |= DESC_PXN;
        *out = desc;
        return true;
    }

    // leaf_attrs sets PXN on every entry it builds, so UXN alone carries the execute permission
    // (DDI 0487 M.b section D8.3, Table D8-52).
#if KICKOS_KERNEL_CORES > 1
    bool removal_owes_rendezvous(uint64_t desc)
    {
        return (desc & DESC_VALID) != 0 and (desc & DESC_UXN) == 0;
    }
#else
    bool removal_owes_rendezvous(uint64_t)
    {
        return false;
    }
#endif

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
            // The allocator ignores outputs it does not own, such as device pages.
            kickos_frame_free(out);
            table[i] = 0;
        }
    }

    // Recursion is bounded by the level count.
    enum arch_aspace_result map_into(uint64_t* table, int level, uintptr_t va, size_t pages,
                                     arch_phys_addr_t pa, uint64_t leaf, bool resident,
                                     bool* broke_executable)
    {
        while (pages != 0)
        {
            size_t const idx = index_at(va, level);
            if (level == LEVEL_LEAF)
            {
                if ((table[idx] & DESC_VALID) != 0)
                {
                    // Read execute permission before clearing the descriptor.
                    // Peers using executable mappings also need an ISB.
                    if (removal_owes_rendezvous(table[idx]))
                    {
                        *broke_executable = true;
                    }
                    // Break before make: invalidate between clearing and replacing the entry.
                    table[idx] = 0;
                    invalidate_page_if(va, resident);
                }
                table[idx] = leaf | (static_cast<uint64_t>(pa) & DESC_OA_MASK);
                // ARM64 does not cache faulting entries. Invalidate for replacements
                // (DDI 0487 M.b, D8.17, IWZCBG).
                invalidate_page_if(va, resident);
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
                kickos::aspace::zero_table(table_at(frame), PTES);
                desc = static_cast<uint64_t>(frame) | DESC_VALID | DESC_BIT1;
                kickos_armv8a_dsb_ishst();
                table[idx] = desc;
                // The leaf invalidations also cover intermediate entries for each address.
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
                map_into(table_at(child_pa), level + 1, va, here, pa, leaf, resident,
                         broke_executable);
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

    // Free empty child tables after leaf rollback; return whether this table is empty.
    bool prune_empty(uint64_t* table, int level)
    {
        if (level == LEVEL_LEAF)
        {
            return kickos::aspace::table_empty(table, PTES);
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
        return kickos::aspace::table_empty(table, PTES);
    }

    // Return null for an unmapped leaf.
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

    // Reject the kernel half before walking: its index bits can alias user entries.
    bool low_half_page(uintptr_t page)
    {
        return page < (static_cast<uintptr_t>(1) << va_bits());
    }

    // Check the whole physical range before editing; masking an oversized
    // output address would alias low memory.
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
        uintptr_t last = 0;
        if (not kickos::extent_last_aligned(va, pages, GRANULE, &last))
        {
            return false; // misaligned, 0 pages, past the pointer width, or a wrapped end
        }
        return low_half_page(last);
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
    uint64_t const mmfr0 = kickos_armv8a_read_mmfr0_el1();
    uint64_t const tcr = kickos_armv8a_read_tcr_el1();
    // TGran4: bits 31:28; TGran64: 27:24; TGran16: 23:20; ASIDBits: 7:4; PARange: 3:0.
    // TGran16 uses the opposite support encoding from TGran4 and TGran64.
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
    unsigned const asid_bits = asid_width();
    unsigned const pa_bits = pa_bits_of(static_cast<unsigned>(mmfr0 & 0xFu));
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
    if (asid_capacity() != 0)
    {
        out |= ARCH_ASPACE_MODEL_TAGGED;
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
    kickos::aspace::zero_table(table, PTES);
    kickos_armv8a_dsb_ishst();
    // Destroy invalidates the old root before its frame or ASID can be reused.
    g_residency.open(static_cast<uint64_t>(root), asid_capacity());
    // The kernel mappings stay in TTBR1. The handle is the user root table.
    return reinterpret_cast<struct arch_aspace*>(table);
}

void arch_aspace_destroy(struct arch_aspace* space)
{
    if (space == nullptr)
    {
        return;
    }
    uint64_t* const table = root_of(space);
    // The boot root and its mappings are not owned by the frame pool.
    if (table == boot_root())
    {
        return;
    }
    // Save the root identity and peer set before freeing the tables.
    uint32_t const peers = resident_peers(space);
    uint64_t const key = root_key(space);
    // Invalidate cached walks before freeing any table.
    invalidate_all();
    // Release the ASID only after all resident cores have invalidated it.
    g_residency.close(key);
    free_subtree(table, LEVEL_ROOT);
    kickos_frame_free(phys_of(table));
    instruction_side_rendezvous(peers);
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
    // Reject partially mapped ranges: rollback would remove existing leaves.
    // Wholly mapped ranges can be remapped without allocating tables.
    size_t mapped = 0;
    for (size_t i = 0; i < pages; i++)
    {
        if (leaf_entry(root_of(space), va + static_cast<uintptr_t>(i) * GRANULE) != nullptr)
        {
            mapped++;
        }
    }
    if (mapped != 0 and mapped != pages)
    {
        return ARCH_ASPACE_EINVAL; // partially mapped, and nothing has been edited
    }
    // Never-run spaces need publication but no invalidation.
    bool const resident = resident_anywhere(space);
    uint32_t const peers = resident_peers(space);
    bool broke_executable = false;
    enum arch_aspace_result const rc =
        map_into(root_of(space), LEVEL_ROOT, va, pages, pa, leaf, resident, &broke_executable);
    publish_edits();
    if (rc != ARCH_ASPACE_OK)
    {
        // Mask interrupts so no walk can occur between invalidation and table free.
        arch_irq_state_t const s = arch_irq_save();
        // Clear leaves before freeing tables. Mapping proceeds in address order,
        // so the first missing leaf ends rollback.
        for (size_t i = 0; i < pages; i++)
        {
            uint64_t* const entry =
                leaf_entry(root_of(space), va + static_cast<uintptr_t>(i) * GRANULE);
            if (entry == nullptr)
            {
                break;
            }
            // Rollback of executable leaves also requires peer instruction synchronization.
            if (removal_owes_rendezvous(*entry))
            {
                broke_executable = true;
            }
            *entry = 0;
        }
        // One sweep each side of the frees.
        invalidate_all();
        (void)prune_empty(root_of(space), LEVEL_ROOT);
        invalidate_all();
        arch_irq_restore(s);
    }
    // Synchronize peers after both replacement and rollback of executable mappings.
    if (broke_executable)
    {
        instruction_side_rendezvous(peers);
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
    bool const resident = resident_anywhere(space);
    uint32_t const peers = resident_peers(space);
    bool executable = false;
    for (size_t i = 0; i < pages; i++)
    {
        uintptr_t const at = va + static_cast<uintptr_t>(i) * GRANULE;
        uint64_t* const entry = leaf_entry(root_of(space), at);
        if (removal_owes_rendezvous(*entry))
        {
            executable = true;
        }
        *entry = 0;
        invalidate_page_if(at, resident);
    }
    publish_edits();
    if (executable)
    {
        instruction_side_rendezvous(peers);
    }
    // Empty tables are reclaimed by destroy.
    return ARCH_ASPACE_OK;
}

void arch_aspace_activate(struct arch_aspace* space)
{
    uint64_t const root = static_cast<uint64_t>(phys_of(root_of(space)));
    uint64_t const ttbr =
        root | (static_cast<uint64_t>(g_residency.identifier(root)) << ASID_SHIFT);
    // Mask interrupts across the root write and any required invalidation.
    arch_irq_state_t const s = arch_irq_save();
    capture_boot();
    write_ttbr0(ttbr);
    arch_irq_restore(s);
}

void kickos_armv8a_ttbr0_to_boot(void)
{
    arch_irq_state_t const s = arch_irq_save();
    if (g_boot_ttbr0 != 0)
    {
        write_ttbr0(g_boot_ttbr0);
    }
    arch_irq_restore(s);
}

struct arch_aspace* arch_aspace_boot(void)
{
    arch_irq_state_t const s = arch_irq_save();
    capture_boot();
    arch_irq_restore(s);
    return reinterpret_cast<struct arch_aspace*>(boot_root());
}

void* arch_aspace_acquire(struct arch_aspace* space, uintptr_t va)
{
    if (space == nullptr)
    {
        return nullptr;
    }
    uintptr_t const page = va & ~(GRANULE - 1);
    // Reject the kernel half before returning a writable pointer.
    if (not low_half_page(page))
    {
        return nullptr;
    }
    // Protect the table walk from concurrent rollback and table reclamation.
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
    // Reject addresses whose index bits could alias a user mapping.
    if (not low_half_page(page))
    {
        return 0;
    }
    // Protect the table walk from concurrent rollback and table reclamation.
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
    // The low byte is zero: direct acquisition has no window holds to mispair.
    return (static_cast<uint64_t>(g_tlbi_issued) << 32) | (static_cast<uint64_t>(elided) << 8);
}

uint32_t arch_aspace_active_cores(struct arch_aspace* space)
{
    if (space == nullptr)
    {
        return 0;
    }
    return active_cores(space);
}
#endif

}
