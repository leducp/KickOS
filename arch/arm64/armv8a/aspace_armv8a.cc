// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// AArch64 stage-1 page tables (DDI 0487 M.b, chapter D8).
// TTBR0 maps userspace; TTBR1 maps the kernel and physical RAM.
// T0SZ=25 selects a 39-bit address space with levels 1 through 3.

#include <kickos/arch/arch.h>
#include <kickos/arch/aspace_residency.h>
#include <kickos/diag.h>
#include <kickos/extent.h>

#include "sysops_armv8a.h"

#include <stddef.h>
#include <stdint.h>

extern "C"
{
    // VA - PA of the kernel half.
    extern unsigned char __kickos_arm64_va_base[];

#if KICKOS_KERNEL_CORES > 1
    void kickos_arm64_instruction_side_rendezvous(uint32_t peers);
#endif
}

namespace
{
    // Table sizes and index widths use the 4 KiB granule of vmsa.h.
    constexpr unsigned GRANULE_SHIFT = 12;
    constexpr size_t GRANULE = static_cast<size_t>(1) << GRANULE_SHIFT;
    constexpr size_t PTES = GRANULE / sizeof(uint64_t);
    constexpr unsigned INDEX_BITS = 9;
    constexpr int LEVEL_ROOT = 1;
    constexpr int LEVEL_LEAF = 3;
    constexpr int HEIGHT_ROOT = LEVEL_LEAF - LEVEL_ROOT;
    static_assert(LEVEL_LEAF - LEVEL_ROOT + 1 == 3,
                  "tests/static/trap_redzone_roots.txt bounds each table walk at 3 activations");

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

    // MAIR_EL1 attribute slots (MAIR_VALUE in vmsa.h).
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
}

#include "aspace_walk.h"

namespace kickos
{
    void kpanic(char const* msg) __attribute__((noreturn));

    namespace armv8a
    {
        // Why the MMU these ID and control registers describe cannot run this port, or null.
        char const* mmu_refusal(uint64_t mmfr0, uint64_t tcr)
        {
            if (((mmfr0 >> 28) & 0xFu) == 0xFu)
            {
                return diag::kMmuNoGranule;
            }
            unsigned const pa = pa_bits_of(static_cast<unsigned>(mmfr0 & 0xFu));
            unsigned const ips = pa_bits_of(static_cast<unsigned>((tcr >> 32) & 0x7u));
            if (pa == 0 or ips == 0 or pa < ips)
            {
                return diag::kMmuPaNarrow;
            }
            return nullptr;
        }
    }
}

namespace
{
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

    // Reserve ASID 0 for untagged roots and the top one for the empty root. Residency also
    // limits IDs to its row count.
    uint32_t asid_capacity()
    {
        unsigned const width = asid_width();
        if (width == 0)
        {
            return 0;
        }
        return (1u << width) - 2u;
    }

    uint64_t empty_asid()
    {
        unsigned const width = asid_width();
        if (width == 0)
        {
            return 0;
        }
        return (1ull << width) - 1u;
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

    // The descriptor write must reach the walker before the invalidate, and the invalidate
    // must complete before the next translated access (DDI 0487 M.b section D8.17).
    void invalidate_page(uintptr_t va)
    {
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

    // Root-keyed ASIDs and the cores that may retain translations; write_ttbr0 records them.
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

    // Not the boot root, whose EL1-only low half PAN cannot police. Read-only: nothing maps here.
    alignas(GRANULE) constexpr uint64_t g_empty_root[PTES] = {};

    uint64_t* empty_root()
    {
        return const_cast<uint64_t*>(g_empty_root);
    }

    // Under an ASID no space is given: a walk of it caches nothing, and leaving it owes no sweep.
    uint64_t empty_ttbr()
    {
        uint64_t const root = static_cast<uint64_t>(phys_of(g_empty_root)) & DESC_OA_MASK;
        return root | (empty_asid() << ASID_SHIFT);
    }

    // Call with interrupts masked. Flush locally when leaving ASID 0: its
    // translations are shared, and the boot root also has global low-half entries.
    // Switching between tagged roots needs no flush. Update the residency record after
    // writing TTBR0. Under the reserved-root form the root being left is the cell's, never the
    // register's, which at EL1 is always the empty root.
    void write_ttbr0(uint64_t ttbr)
    {
        uint64_t leaving = 0;
        if (kickos_armv8a_root_deferred())
        {
            uint64_t* const cell = kickos_armv8a_user_ttbr0_cell();
            leaving = *cell;
            *cell = ttbr;
        }
        else
        {
            leaving = kickos_armv8a_read_ttbr0_el1();
            kickos_armv8a_write_ttbr0_el1(ttbr);
            kickos_armv8a_isb();
        }
        if (asid_of(leaving) == 0)
        {
            kickos_armv8a_tlbi_all_local();
            kickos_armv8a_dsb_ish();
            kickos_armv8a_isb();
        }
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
    bool removal_owes(uint64_t desc)
    {
        return (desc & DESC_VALID) != 0 and (desc & DESC_UXN) == 0;
    }
#else
    bool removal_owes(uint64_t)
    {
        return false;
    }
#endif

    int walk_top()
    {
        return HEIGHT_ROOT;
    }

    arch_phys_addr_t entry_pa(uint64_t desc)
    {
        return static_cast<arch_phys_addr_t>(desc & DESC_OA_MASK);
    }

    // DESC_BIT1 clear above level 3 is a block.
    bool entry_table(uint64_t desc, int)
    {
        return (desc & DESC_VALID) != 0 and (desc & DESC_BIT1) != 0;
    }

    bool root_slot_kept(uint64_t const*, size_t)
    {
        return false;
    }

    uint64_t leaf_desc(uint64_t attrs, arch_phys_addr_t pa)
    {
        return attrs | (static_cast<uint64_t>(pa) & DESC_OA_MASK);
    }

    void link_table(uint64_t* entry, arch_phys_addr_t table, Edit const&)
    {
        kickos_armv8a_dsb_ishst();
        *entry = static_cast<uint64_t>(table) | DESC_VALID | DESC_BIT1;
    }

    // A detached table needs the whole sweep: a by-address invalidate drops the walks of that
    // address only.
    void edits_done(Edit const& e)
    {
        publish_edits();
        if (e.detached != 0 and e.resident)
        {
            invalidate_all();
        }
        if (e.owed)
        {
            instruction_side_rendezvous(e.peers);
        }
    }

    Edit edit_of(struct arch_aspace* space)
    {
        Edit const e = {resident_anywhere(space), resident_peers(space), false, 0};
        return e;
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

void kickos_armv8a_mmu_check(void)
{
    char const* const why = kickos::armv8a::mmu_refusal(kickos_armv8a_read_mmfr0_el1(),
                                                        kickos_armv8a_read_tcr_el1());
    if (why != nullptr)
    {
        kickos::kpanic(why);
    }
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
    // The empty root is no frame of the pool.
    if (table == empty_root())
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
    free_subtree(table, HEIGHT_ROOT, true);
    kickos_frame_free(phys_of(table));
    instruction_side_rendezvous(peers);
}

enum arch_aspace_result arch_aspace_map(struct arch_aspace* space, uintptr_t va,
                                        arch_phys_addr_t pa, size_t pages,
                                        uint32_t rights, enum arch_map_memtype type)
{
    if (space == nullptr or root_of(space) == empty_root() or not range_ok(va, pages))
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
    return walk_map(root_of(space), va, pa, pages, leaf, edit_of(space));
}

enum arch_aspace_result arch_aspace_unmap(struct arch_aspace* space, uintptr_t va, size_t pages)
{
    if (space == nullptr or root_of(space) == empty_root() or not range_ok(va, pages))
    {
        return ARCH_ASPACE_EINVAL;
    }
    return walk_unmap(root_of(space), va, pages, edit_of(space));
}

void arch_aspace_activate(struct arch_aspace* space)
{
    uint64_t ttbr = empty_ttbr();
    if (root_of(space) != empty_root())
    {
        uint64_t const root = static_cast<uint64_t>(phys_of(root_of(space)));
        ttbr = root | (static_cast<uint64_t>(g_residency.identifier(root)) << ASID_SHIFT);
    }
    // Mask interrupts across the root write and any required invalidation.
    arch_irq_state_t const s = arch_irq_save();
    write_ttbr0(ttbr);
    arch_irq_restore(s);
}

// The register itself, whatever the form: the reporter never returns to EL0.
void kickos_armv8a_ttbr0_to_empty(void)
{
    arch_irq_state_t const s = arch_irq_save();
    kickos_armv8a_write_ttbr0_el1(empty_ttbr());
    kickos_armv8a_isb();
    arch_irq_restore(s);
}

uint64_t kickos_armv8a_empty_ttbr(void)
{
    return empty_ttbr();
}

// TTBR0 holds the whole user half, so a frame is reached at its physical address.
uintptr_t arch_aspace_user_offset(void)
{
    return 0;
}

// Spawn windows sit halfway up the user half, far above every frame's identity address and the
// app window, and 1 GiB of it is theirs.
void arch_aspace_window_area(uintptr_t* base, size_t* size)
{
    *base = static_cast<uintptr_t>(1) << (va_bits() - 1u);
    *size = static_cast<size_t>(1) << 30;
}

struct arch_aspace* arch_aspace_boot(void)
{
    return reinterpret_cast<struct arch_aspace*>(empty_root());
}

void* arch_aspace_acquire(struct arch_aspace* space, uintptr_t va, bool* uncached,
                          arch_phys_addr_t* frame)
{
    if (frame != nullptr)
    {
        *frame = 0;
    }
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
    uint64_t desc = 0;
    if (entry != nullptr)
    {
        desc = *entry;
    }
    arch_irq_restore(s);
    if (entry == nullptr)
    {
        return nullptr;
    }
    arch_phys_addr_t const out = static_cast<arch_phys_addr_t>(desc & DESC_OA_MASK);
    if (frame != nullptr)
    {
        *frame = out;
    }
    if (uncached != nullptr)
    {
        *uncached = ((desc >> 2) & 7u) == ATTR_NOCACHE;
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

}
