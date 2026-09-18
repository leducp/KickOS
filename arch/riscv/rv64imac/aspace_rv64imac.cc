// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RV64 page tables for the configured paging mode.
// Roots copy the boot root's kernel entries and share the tables below them.
// Page-table levels are defined by rv64_paging.h.
// Frames inside the kernel RAM window use a fixed address offset. Other frames
// use reference-counted (space, page) slots, with ARCH_ASPACE_ACQUIRE_MIN
// distinct pages per core.

#include <kickos/arch/arch.h>
#include <kickos/arch/aspace_residency.h>
#include <kickos/arch/aspace_table.h>
#include <kickos/arch/rv64_paging.h>
#include <kickos/extent.h>

#include "sysops_rv64imac.h"

#include <stddef.h>
#include <stdint.h>

#if KICKOS_KERNEL_CORES > 1
// Request and wait for a translation fence on each peer.
extern "C" void kickos_rv64_translation_rendezvous(uint32_t peers);
#endif

extern "C"
{
    arch_phys_addr_t kickos_frame_alloc(void);
    void kickos_frame_free(arch_phys_addr_t frame);
}

namespace
{
    // satp programs 4 KiB, the only granule these modes define.
    constexpr unsigned GRANULE_SHIFT = KICKOS_RV64_GRANULE_SHIFT;
    constexpr size_t GRANULE = static_cast<size_t>(1) << GRANULE_SHIFT;
    constexpr size_t PTES = GRANULE / sizeof(uint64_t);
    constexpr int LEVEL_ROOT = KICKOS_RV64_LEVEL_ROOT;
    constexpr int LEVEL_LEAF = KICKOS_RV64_LEVEL_LEAF;
    constexpr unsigned INDEX_BITS = KICKOS_RV64_INDEX_BITS;
    // A page table must fit exactly one index range.
    static_assert(PTES == (static_cast<size_t>(1) << INDEX_BITS),
                  "an entry index does not span exactly one table page");
    static_assert(LEVEL_ROOT > LEVEL_LEAF, "a mode with no non-leaf level has no walk");

    // Sign extension separates the user half from the kernel half.
    constexpr uintptr_t LOW_HALF_END = static_cast<uintptr_t>(1) << (KICKOS_RV64_VA_BITS - 1);

    // satp fields at XLEN 64: MODE 63:60, ASID 59:44, PPN 43:0.
    constexpr unsigned SATP_MODE_SHIFT = KICKOS_RV64_SATP_MODE_SHIFT;
    constexpr unsigned SATP_ASID_SHIFT = 44;
    constexpr uint64_t SATP_ASID_MASK = 0xFFFFull << SATP_ASID_SHIFT;
    constexpr uint64_t SATP_PPN_MASK = (1ull << SATP_ASID_SHIFT) - 1u;
    // Each paging mode adds a level; satp.MODE is LEVEL_ROOT + 6.
    constexpr uint64_t SATP_MODE = static_cast<uint64_t>(LEVEL_ROOT) + 6u;
    constexpr uint64_t SATP_MODE_MASK = 0xFull;
    // The editor and boot code must use the same page-table depth.
    static_assert(SATP_MODE == KICKOS_RV64_SATP_MODE,
                  "the depth this backend walks is not the depth the configured paging mode has");

    // Entry bits. R=W=X=0 with V set is a pointer to the next level, so there is no
    // zero-rights leaf and a guard page is the absence of a mapping.
    constexpr uint64_t PTE_V = 1ull << 0;
    constexpr uint64_t PTE_R = 1ull << 1;
    constexpr uint64_t PTE_W = 1ull << 2;
    constexpr uint64_t PTE_X = 1ull << 3;
    constexpr uint64_t PTE_U = 1ull << 4;
    constexpr uint64_t PTE_G = 1ull << 5;
    constexpr uint64_t PTE_A = 1ull << 6;
    constexpr uint64_t PTE_D = 1ull << 7;
    constexpr uint64_t PTE_RWX = PTE_R | PTE_W | PTE_X;
    // PPN sits at 53:10 and carries the output address shifted right by the granule.
    constexpr uint64_t PTE_PPN_MASK = 0x003FFFFFFFFFFC00ull;

    // The identifier width recorded for this class of part, compared against the WARL
    // measurement below.
    constexpr unsigned ASID_BITS_RECORDED = 16;

    uint32_t asid_of(uint64_t satp)
    {
        return static_cast<uint32_t>((satp & SATP_ASID_MASK) >> SATP_ASID_SHIFT);
    }

    // The window's slots per core, one per DISTINCT (space, page) held.
    constexpr size_t ACQUIRE_CAPACITY = ARCH_ASPACE_ACQUIRE_MIN;
    static_assert(ACQUIRE_CAPACITY * KICKOS_NUM_CORES <= PTES,
                  "the transient window outgrows the one level-0 table the chip provides");

    // Boot tables and physical-window bounds supplied by arch_init before use.
    uint64_t* g_boot_root = nullptr;
    // Cache the boot root PPN for the switch path.
    uint64_t g_boot_ppn = 0;
    uint64_t* g_window_leaves = nullptr;
    uintptr_t g_window_va = 0;
    uintptr_t g_window_delta = 0;
    arch_phys_addr_t g_window_pa_lo = 0;
    arch_phys_addr_t g_window_pa_hi = 0;
    // Platform physical address width. Zero rejects every range.
    unsigned g_phys_bits = 0;

    struct WindowSlot
    {
        struct arch_aspace* space;
        uintptr_t page;
        uint32_t holds; // 0 marks the slot free; space is null then too
    };
    WindowSlot g_slots[KICKOS_NUM_CORES][ACQUIRE_CAPACITY];

    // Reject hold-count overflow to avoid unmapping a page still in use.
    constexpr uint32_t HOLDS_MAX = 0xFFFFFFFFu;

#if defined(KICKOS_ENABLE_SELFTEST)
    // Issued and skipped invalidations, protected by the caller's IrqLock.
    uint32_t g_tlbi_issued = 0;
    uint32_t g_tlbi_elided = 0;
#endif

    // Saturating count of unmatched releases outside the direct RAM window.
    // Updates run with interrupts masked; reporting is self-test-only.
    uint8_t g_release_mispaired = 0;

    uint64_t read_satp()
    {
        return kickos_rv64_read_satp();
    }

    arch_phys_addr_t pte_pa(uint64_t pte)
    {
        return static_cast<arch_phys_addr_t>((pte & PTE_PPN_MASK) >> 10) << GRANULE_SHIFT;
    }

    uint64_t pa_ppn(arch_phys_addr_t pa)
    {
        return (static_cast<uint64_t>(pa >> GRANULE_SHIFT) << 10) & PTE_PPN_MASK;
    }

    // All table frames are inside the directly mapped RAM window.
    uint64_t* table_at(arch_phys_addr_t pa)
    {
        return reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(pa) + g_window_delta);
    }

    arch_phys_addr_t phys_of(void const* p)
    {
        return static_cast<arch_phys_addr_t>(reinterpret_cast<uintptr_t>(p) - g_window_delta);
    }

    // Only DRAM uses direct acquisition; device frames use temporary slots.
    bool in_kernel_window(arch_phys_addr_t pa)
    {
        return pa >= g_window_pa_lo and pa < g_window_pa_hi;
    }

    uint64_t* root_of(struct arch_aspace* space)
    {
        return reinterpret_cast<uint64_t*>(space);
    }

    uint64_t satp_of(uint64_t const* root)
    {
        return (SATP_MODE << SATP_MODE_SHIFT)
               | (static_cast<uint64_t>(phys_of(root)) >> GRANULE_SHIFT);
    }

    size_t index_at(uintptr_t va, int level)
    {
        unsigned const shift = GRANULE_SHIFT + static_cast<unsigned>(level) * INDEX_BITS;
        return static_cast<size_t>((va >> shift) & (PTES - 1));
    }

    // Bytes one entry at `level` spans: 4 KiB at the leaf and 512 times that per level up.
    uintptr_t span_at(int level)
    {
        unsigned const shift = GRANULE_SHIFT + static_cast<unsigned>(level) * INDEX_BITS;
        return static_cast<uintptr_t>(1) << shift;
    }

    // The store must reach the walker before the invalidate, and the invalidate must complete
    // before the next translated access.
    void invalidate_page(uintptr_t va)
    {
#if defined(KICKOS_ENABLE_SELFTEST)
        g_tlbi_issued++;
#endif
        kickos_rv64_fence_w_w();
        kickos_rv64_sfence_page(va);
    }

    void invalidate_all()
    {
        kickos_rv64_fence_w_w();
        kickos_rv64_sfence_all();
    }

#if KICKOS_KERNEL_CORES > 1
    // Last installed PPN per core, initialized to the common boot root.
    // Updated only by write_satp.
    uint64_t g_installed_root[KICKOS_NUM_CORES] = {};
#endif

    // Root-keyed residency, including harts that switched away.
    kickos::aspace::Residency<KICKOS_NUM_CORES> g_residency;

    uint64_t root_key(uint64_t const* root)
    {
        return satp_of(root) & SATP_PPN_MASK;
    }

    // Call with interrupts masked. SFENCE.VMA is required when leaving ASID 0
    // or first activating this root on this hart. FENCE w,w and satp writes do
    // not order implicit translation reads (Privileged ISA 11.1.1.11, 12.2.1).
    // Untracked roots fence on every entry. The boot root is already published
    // on every hart, and later edits to it synchronize every hart.
    // Keep the installed-root and residency records consistent with satp.
    void write_satp(uint64_t satp)
    {
        uint64_t const leaving = kickos_rv64_read_satp();
        uint64_t const entering = satp & SATP_PPN_MASK;
        uint32_t const core = arch_cpu_id();
        // Update residency and read its previous value in one lookup.
        bool const ran_here = g_residency.note_and_was_resident(entering, core);
        bool const first_use_here = not ran_here and entering != g_boot_ppn;
        kickos_rv64_write_satp(satp);
        if (asid_of(leaving) == 0 or first_use_here)
        {
            kickos_rv64_sfence_all();
        }
#if KICKOS_KERNEL_CORES > 1
        g_installed_root[core] = entering;
#endif
    }

    // A new non-leaf PTE needs SFENCE.VMA with rs1=x0: by-address fences only
    // cover leaf entries, and invalid PTEs may be cached (Privileged ISA 12.2.1).
    // Use rs2=x0 to cover every ASID.
    void invalidate_nonleaf(bool resident)
    {
        if (not resident)
        {
#if defined(KICKOS_ENABLE_SELFTEST)
            g_tlbi_elided++;
#endif
            return;
        }
#if defined(KICKOS_ENABLE_SELFTEST)
        g_tlbi_issued++;
#endif
        invalidate_all();
    }

    uint32_t resident_cores(struct arch_aspace* space)
    {
        return g_residency.cores(root_key(root_of(space)));
    }

    // Never-run spaces skip invalidation and are published on each hart's first
    // activation. Keep residency bits after switching away.
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
    bool installed_here(struct arch_aspace* space)
    {
        return (read_satp() & SATP_PPN_MASK) == (satp_of(root_of(space)) & SATP_PPN_MASK);
    }

    // Cores currently using this root, for self-tests only.
    // Maintenance uses residency, which includes harts that switched away.
    uint32_t active_cores(struct arch_aspace* space)
    {
        uint32_t set = 0;
        if (installed_here(space))
        {
            set |= 1u << arch_cpu_id();
        }
#if KICKOS_KERNEL_CORES > 1
        uint64_t const ppn = satp_of(root_of(space)) & SATP_PPN_MASK;
        for (uint32_t c = 0; c < KICKOS_NUM_CORES; c++)
        {
            if (g_installed_root[c] == ppn)
            {
                set |= 1u << c;
            }
        }
#endif
        return set;
    }
#endif

    // SFENCE.VMA is local, so every resident peer must run its own fence.
    // This applies to data and executable mappings. FENCE.I is unavailable
    // in this board's ISA baseline.
#if KICKOS_KERNEL_CORES > 1
    void translation_rendezvous(uint32_t peers)
    {
        kickos_rv64_translation_rendezvous(peers);
    }
#else
    void translation_rendezvous(uint32_t)
    {
    }
#endif

    // Invalidate on this hart and all resident peers before freeing tables.
    void invalidate_all_everywhere(uint32_t peers)
    {
        invalidate_all();
        translation_rendezvous(peers);
    }

    // Order descriptor stores before notifying peers. FENCE w,w does not order
    // implicit translation reads; write_satp supplies SFENCE.VMA on first use.
    void publish_edits()
    {
#if KICKOS_KERNEL_CORES > 1
        kickos_rv64_fence_w_w();
#endif
    }

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

    // Memory types are fixed by physical address; this part has no Svpbmt.
    bool memtype_known(enum arch_map_memtype type)
    {
        return type == ARCH_MAP_NORMAL or type == ARCH_MAP_NOCACHE or type == ARCH_MAP_DEVICE;
    }

    // RISC-V reserves W without R; this backend requires read permission.
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
        if (not memtype_known(type))
        {
            return false;
        }
        // Set A and D because this port does not enable hardware updates.
        uint64_t desc = PTE_V | PTE_U | PTE_A | PTE_D | PTE_R;
        if ((rights & ARCH_MAP_W) != 0)
        {
            desc |= PTE_W;
        }
        if ((rights & ARCH_MAP_X) != 0)
        {
            desc |= PTE_X;
        }
        *out = desc;
        return true;
    }

    bool is_table(uint64_t desc)
    {
        return (desc & PTE_V) != 0 and (desc & PTE_RWX) == 0;
    }

    // Free tables and leaf outputs, excluding shared entries copied from the
    // boot root. `keep` is the boot root at LEVEL_ROOT and null below it.
    void free_subtree(uint64_t* table, int level, uint64_t const* keep)
    {
        for (size_t i = 0; i < PTES; i++)
        {
            uint64_t const desc = table[i];
            if ((desc & PTE_V) == 0)
            {
                continue;
            }
            if (keep != nullptr and desc == keep[i])
            {
                continue;
            }
            arch_phys_addr_t const out = pte_pa(desc);
            if (level > LEVEL_LEAF and is_table(desc))
            {
                free_subtree(table_at(out), level - 1, nullptr);
            }
            // The allocator ignores outputs it does not own, such as device pages.
            kickos_frame_free(out);
            table[i] = 0;
        }
    }

    enum arch_aspace_result map_into(uint64_t* table, int level, uintptr_t va, size_t pages,
                                     arch_phys_addr_t pa, uint64_t leaf, bool resident)
    {
        while (pages != 0)
        {
            size_t const idx = index_at(va, level);
            if (level == LEVEL_LEAF)
            {
                if ((table[idx] & PTE_V) != 0)
                {
                    // Invalidate between clearing and replacing the descriptor.
                    table[idx] = 0;
                    invalidate_page_if(va, resident);
                }
                table[idx] = leaf | pa_ppn(pa);
                // Invalid PTEs may also be cached.
                invalidate_page_if(va, resident);
                va += GRANULE;
                pa += GRANULE;
                pages--;
                continue;
            }

            uint64_t desc = table[idx];
            if ((desc & PTE_V) == 0)
            {
                arch_phys_addr_t const frame = kickos_frame_alloc();
                if (frame == 0)
                {
                    return ARCH_ASPACE_ENOMEM;
                }
                kickos::aspace::zero_table(table_at(frame), PTES);
                desc = pa_ppn(frame) | PTE_V;
                kickos_rv64_fence_w_w();
                table[idx] = desc;
                // The per-leaf fence below names leaves only, so it does not reach this entry.
                invalidate_nonleaf(resident);
            }
            else if (not is_table(desc))
            {
                // A larger leaf already covers this range: replacing it would change the
                // mapping of pages this call was not asked about.
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
                map_into(table_at(pte_pa(desc)), level - 1, va, here, pa, leaf, resident);
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

    // Free empty child tables, excluding boot entries; return whether this table is empty.
    bool prune_empty(uint64_t* table, int level, uint64_t const* keep)
    {
        if (level == LEVEL_LEAF)
        {
            return kickos::aspace::table_empty(table, PTES);
        }
        for (size_t i = 0; i < PTES; i++)
        {
            uint64_t const desc = table[i];
            if ((desc & PTE_V) == 0)
            {
                continue;
            }
            if (keep != nullptr and desc == keep[i])
            {
                continue;
            }
            if (not is_table(desc))
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
        return kickos::aspace::table_empty(table, PTES);
    }

    // Return null for an unmapped leaf.
    uint64_t* leaf_entry(uint64_t* table, uintptr_t va)
    {
        for (int level = LEVEL_ROOT; level > LEVEL_LEAF; level--)
        {
            uint64_t const desc = table[index_at(va, level)];
            if (not is_table(desc))
            {
                return nullptr;
            }
            table = table_at(pte_pa(desc));
        }
        uint64_t* const entry = &table[index_at(va, LEVEL_LEAF)];
        if ((*entry & PTE_V) == 0)
        {
            return nullptr;
        }
        return entry;
    }

    // Only the user half may be edited; kernel tables are shared.
    bool low_half_page(uintptr_t page)
    {
        return page < LOW_HALF_END;
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

    // Validate the whole physical range against both hardware and PPN limits
    // before editing. An oversized PPN would alias low memory.
    bool phys_range_ok(arch_phys_addr_t pa, size_t pages)
    {
        if ((pa & static_cast<arch_phys_addr_t>(GRANULE - 1)) != 0)
        {
            return false;
        }
        if (pages == 0)
        {
            return false;
        }
        uint64_t const bytes = static_cast<uint64_t>(pages) * static_cast<uint64_t>(GRANULE);
        if (bytes / GRANULE != pages)
        {
            return false;
        }
        uint64_t const end = static_cast<uint64_t>(pa) + bytes;
        if (end < static_cast<uint64_t>(pa))
        {
            return false;
        }
        // end is exclusive and pages is not zero, so end - 1 is the last byte the range covers.
        arch_phys_addr_t const last = static_cast<arch_phys_addr_t>(end - 1u);
        if (pa_ppn(last) != (static_cast<uint64_t>(last >> GRANULE_SHIFT) << 10))
        {
            return false;
        }
        if (g_phys_bits == 0)
        {
            return false;
        }
        // Avoid shifting by 64 when the platform reports the full pointer width.
        if (g_phys_bits >= 64)
        {
            return true;
        }
        return end <= (static_cast<uint64_t>(1) << g_phys_bits);
    }

    uintptr_t slot_va(size_t core, size_t slot)
    {
        return g_window_va + ((core * ACQUIRE_CAPACITY) + slot) * GRANULE;
    }

    uint64_t* slot_entry(size_t core, size_t slot)
    {
        return &g_window_leaves[(core * ACQUIRE_CAPACITY) + slot];
    }

    // Acquire a per-core slot for a frame outside the RAM window. Repeated
    // acquires of (space, page) share a reference-counted slot and stable pointer.
    // Return null if every slot holds a different page.
    void* window_take(struct arch_aspace* space, uintptr_t page, arch_phys_addr_t pa)
    {
        size_t const core = static_cast<size_t>(arch_cpu_id());
        for (size_t i = 0; i < ACQUIRE_CAPACITY; i++)
        {
            if (g_slots[core][i].holds == 0)
            {
                continue;
            }
            if (g_slots[core][i].space != space or g_slots[core][i].page != page)
            {
                continue;
            }
            if (g_slots[core][i].holds == HOLDS_MAX)
            {
                return nullptr;
            }
            g_slots[core][i].holds++;
            return reinterpret_cast<void*>(slot_va(core, i));
        }
        for (size_t i = 0; i < ACQUIRE_CAPACITY; i++)
        {
            if (g_slots[core][i].holds != 0)
            {
                continue;
            }
            // Kernel data mapping: global, non-user and non-executable.
            // The parent entries do not set G (Privileged ISA 12.3.1).
            *slot_entry(core, i) =
                pa_ppn(pa) | PTE_V | PTE_G | PTE_R | PTE_W | PTE_A | PTE_D;
            kickos_rv64_fence_w_w();
            kickos_rv64_sfence_page(slot_va(core, i));
            g_slots[core][i].space = space;
            g_slots[core][i].page = page;
            g_slots[core][i].holds = 1;
            return reinterpret_cast<void*>(slot_va(core, i));
        }
        return nullptr;
    }

    // Release one hold of (space, page); unmap only after the last hold.
    // Return whether a matching hold existed.
    bool window_drop(struct arch_aspace* space, uintptr_t page)
    {
        size_t const core = static_cast<size_t>(arch_cpu_id());
        for (size_t i = 0; i < ACQUIRE_CAPACITY; i++)
        {
            if (g_slots[core][i].holds == 0)
            {
                continue;
            }
            if (g_slots[core][i].space != space or g_slots[core][i].page != page)
            {
                continue;
            }
            g_slots[core][i].holds--;
            if (g_slots[core][i].holds != 0)
            {
                return true;
            }
            *slot_entry(core, i) = 0;
            kickos_rv64_fence_w_w();
            kickos_rv64_sfence_page(slot_va(core, i));
            g_slots[core][i].space = nullptr;
            g_slots[core][i].page = 0;
            return true;
        }
        return false;
    }

    struct AsidField
    {
        unsigned bits;   // the width the field spans: the highest bit that stuck, plus one
        bool contiguous; // and every bit below that one stuck too
    };

    // Probe the ASID field while preserving MODE and PPN, fencing both sides.
    // Report the highest accepted bit plus one and whether the field is contiguous.
    AsidField measure_asid_field()
    {
        arch_irq_state_t const s = arch_irq_save();
        uint64_t const satp = read_satp();
        kickos_rv64_sfence_all();
        kickos_rv64_write_satp(satp | SATP_ASID_MASK);
        uint64_t const back = read_satp();
        kickos_rv64_write_satp(satp);
        kickos_rv64_sfence_all();
        arch_irq_restore(s);
        uint64_t const field = (back & SATP_ASID_MASK) >> SATP_ASID_SHIFT;
        AsidField out;
        out.bits = 0;
        while ((field >> out.bits) != 0)
        {
            out.bits++;
        }
        out.contiguous = field == ((static_cast<uint64_t>(1) << out.bits) - 1u);
        return out;
    }

    // Probe once at boot: repeating it flushes cached translations.
    // All harts must support the ASID width measured on the boot hart.
    AsidField g_asid_field = {0, false};

    // Reserve ID 0. Non-contiguous or absent ASID fields disable tagging.
    // Residency also limits allocation to its row count.
    uint32_t g_asid_capacity = 0;

    uint32_t asid_capacity_of(AsidField field)
    {
        if (field.bits == 0 or not field.contiguous)
        {
            return 0;
        }
        return (1u << field.bits) - 1u;
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
    // satp.MODE is WARL, so the mode read back here is one this hart implements.
    unsigned const mode = static_cast<unsigned>((read_satp() >> SATP_MODE_SHIFT) & SATP_MODE_MASK);
    uint64_t granules = 0;
    unsigned pa_bits = 0;
    if (mode == SATP_MODE)
    {
        granules = 1; // the architecture defines one page size for this mode
        // Use the platform limit; the PPN field can encode unsupported physical addresses.
        pa_bits = g_phys_bits;
    }
    AsidField const asid = g_asid_field;
    uint64_t out = 0;
    if (granules != 0 and GRANULE == 4096u)
    {
        out |= ARCH_ASPACE_MODEL_GRANULE;
    }
    // Contiguity gates the verdict bit and not the reported width.
    if (asid.contiguous and asid.bits == ASID_BITS_RECORDED)
    {
        out |= ARCH_ASPACE_MODEL_ASID;
    }
    // Tagging can remain enabled even when the ASID width is below the expected width.
    if (g_asid_capacity != 0)
    {
        out |= ARCH_ASPACE_MODEL_TAGGED;
    }
    // The supported physical range must cover the entire kernel RAM window.
    if (pa_bits != 0
        and (static_cast<arch_phys_addr_t>(1) << pa_bits) >= g_window_pa_hi)
    {
        out |= ARCH_ASPACE_MODEL_PA;
    }
    out |= static_cast<uint64_t>(asid.bits) << ARCH_ASPACE_MODEL_ASID_SHIFT;
    out |= static_cast<uint64_t>(pa_bits) << ARCH_ASPACE_MODEL_PA_SHIFT;
    out |= granules << ARCH_ASPACE_MODEL_GRAN_SHIFT;
    return out;
}

bool arch_aspace_memtype_support(enum arch_map_memtype type)
{
    return memtype_known(type);
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
    // Copy the boot root entries; their child tables remain shared.
    for (size_t i = 0; i < PTES; i++)
    {
        table[i] = g_boot_root[i];
    }
    kickos_rv64_fence_w_w();
    // Destroy invalidates the old root before its frame or ASID can be reused.
    g_residency.open(root_key(table), g_asid_capacity);
    return reinterpret_cast<struct arch_aspace*>(table);
}

void arch_aspace_destroy(struct arch_aspace* space)
{
    if (space == nullptr)
    {
        return;
    }
    uint64_t* const table = root_of(space);
    // The boot root is not owned by the frame pool.
    if (table == g_boot_root)
    {
        return;
    }
    // Save the root identity and peer set before freeing the tables.
    uint32_t const peers = resident_peers(space);
    uint64_t const key = root_key(table);
    // Invalidate cached walks on all resident harts before freeing tables.
    invalidate_all_everywhere(peers);
    // Release the ASID only after all resident harts have invalidated it.
    g_residency.close(key);
    free_subtree(table, LEVEL_ROOT, g_boot_root);
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
        return ARCH_ASPACE_EINVAL; // misaligned, or an extent reaching past the PPN field
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
    // Never-run spaces need publication on first activation, but no invalidation here.
    bool const resident = resident_anywhere(space);
    uint32_t const peers = resident_peers(space);
    enum arch_aspace_result const rc =
        map_into(root_of(space), LEVEL_ROOT, va, pages, pa, leaf, resident);
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
            *entry = 0;
        }
        // Invalidate on all resident harts before and after pruning. Pruning clears
        // and frees tables in one pass; the kernel lock prevents frame reuse until
        // the second invalidation. Separate clear and free passes are required
        // if that lock no longer covers the operation.
        invalidate_all_everywhere(peers);
        (void)prune_empty(root_of(space), LEVEL_ROOT, g_boot_root);
        invalidate_all_everywhere(peers);
        arch_irq_restore(s);
    }
    // Invalidate peers' cached missing table entries once per call.
    if (resident)
    {
        translation_rendezvous(peers);
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
    for (size_t i = 0; i < pages; i++)
    {
        uintptr_t const at = va + static_cast<uintptr_t>(i) * GRANULE;
        uint64_t* const entry = leaf_entry(root_of(space), at);
        *entry = 0;
        invalidate_page_if(at, resident);
    }
    publish_edits();
    // Synchronize peers for all removals, including data mappings.
    translation_rendezvous(peers);
    // Empty tables are reclaimed by destroy.
    return ARCH_ASPACE_OK;
}

void arch_aspace_activate(struct arch_aspace* space)
{
    if (space == nullptr)
    {
        return;
    }
    uint64_t const satp = satp_of(root_of(space));
    uint64_t const id = g_residency.identifier(satp & SATP_PPN_MASK);
    // Kernel and temporary-window mappings are shared across roots.
    // Mask interrupts across the root write and any required translation fence.
    arch_irq_state_t const s = arch_irq_save();
    write_satp(satp | (id << SATP_ASID_SHIFT));
    arch_irq_restore(s);
}

struct arch_aspace* arch_aspace_boot(void)
{
    return reinterpret_cast<struct arch_aspace*>(g_boot_root);
}

void* arch_aspace_acquire(struct arch_aspace* space, uintptr_t va)
{
    if (space == nullptr or g_window_leaves == nullptr)
    {
        return nullptr;
    }
    uintptr_t const page = va & ~static_cast<uintptr_t>(GRANULE - 1);
    // Reject the kernel half before returning a writable pointer.
    if (not low_half_page(page))
    {
        return nullptr;
    }
    uintptr_t const off = va & static_cast<uintptr_t>(GRANULE - 1);
    arch_irq_state_t const s = arch_irq_save();
    uint64_t const* const entry = leaf_entry(root_of(space), page);
    if (entry == nullptr)
    {
        arch_irq_restore(s);
        return nullptr;
    }
    arch_phys_addr_t const out = pte_pa(*entry);
    if (in_kernel_window(out))
    {
        arch_irq_restore(s);
        return reinterpret_cast<void*>(static_cast<uintptr_t>(out) + g_window_delta + off);
    }
    void* const slot = window_take(space, page, out);
    arch_irq_restore(s);
    if (slot == nullptr)
    {
        return nullptr;
    }
    return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(slot) + off);
}

// Count unmatched window releases without panicking: the fault reporter
// uses this path. Direct RAM acquisitions have no window hold to release.
void arch_aspace_release(struct arch_aspace* space, uintptr_t va)
{
    if (space == nullptr or g_window_leaves == nullptr)
    {
        return;
    }
    uintptr_t const page = va & ~static_cast<uintptr_t>(GRANULE - 1);
    // Out-of-range addresses cannot hold a window slot.
    if (not low_half_page(page))
    {
        return;
    }
    arch_irq_state_t const s = arch_irq_save();
    bool defect = false;
    if (not window_drop(space, page))
    {
        // Only frames outside the direct RAM window require a matching slot hold.
        uint64_t const* const entry = leaf_entry(root_of(space), page);
        defect = entry != nullptr and not in_kernel_window(pte_pa(*entry));
    }
    arch_irq_restore(s);
    if (defect and g_release_mispaired != 0xFFu)
    {
        g_release_mispaired++;
    }
}

arch_phys_addr_t arch_aspace_frame_at(struct arch_aspace* space, uintptr_t va)
{
    // Do not walk tables before the chip supplies the physical-window mapping.
    if (space == nullptr or g_window_leaves == nullptr)
    {
        return 0;
    }
    uintptr_t const page = va & ~static_cast<uintptr_t>(GRANULE - 1);
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
        out = pte_pa(*entry);
    }
    arch_irq_restore(s);
    return out;
}

// Supply boot tables and physical-window bounds before creating any space.
void kickos_rv64_aspace_boot(uint64_t* user_root, uint64_t* window_leaves, uintptr_t window_va,
                             uintptr_t window_delta, arch_phys_addr_t pa_lo,
                             arch_phys_addr_t pa_hi, unsigned phys_bits)
{
    g_boot_root = user_root;
    g_window_leaves = window_leaves;
    g_window_va = window_va;
    g_window_delta = window_delta;
    g_window_pa_lo = pa_lo;
    g_window_pa_hi = pa_hi;
    g_phys_bits = phys_bits;
    // Every hart starts on this boot root.
    g_boot_ppn = satp_of(user_root) & SATP_PPN_MASK;
    g_asid_field = measure_asid_field();
    g_asid_capacity = asid_capacity_of(g_asid_field);
#if KICKOS_KERNEL_CORES > 1
    for (size_t c = 0; c < KICKOS_NUM_CORES; c++)
    {
        g_installed_root[c] = g_boot_ppn;
    }
#endif
    for (size_t c = 0; c < KICKOS_NUM_CORES; c++)
    {
        for (size_t i = 0; i < ACQUIRE_CAPACITY; i++)
        {
            g_slots[c][i].space = nullptr;
            g_slots[c][i].page = 0;
            g_slots[c][i].holds = 0;
        }
    }
}

#if defined(KICKOS_ENABLE_SELFTEST)
uint64_t arch_aspace_tlbi_counts(void)
{
    uint32_t elided = g_tlbi_elided;
    if (elided > 0xFFFFFFu)
    {
        elided = 0xFFFFFFu; // saturates rather than bleeding into the issued half
    }
    return (static_cast<uint64_t>(g_tlbi_issued) << 32) | (static_cast<uint64_t>(elided) << 8)
           | static_cast<uint64_t>(g_release_mispaired);
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
