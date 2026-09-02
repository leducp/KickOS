// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RISC-V RV64IMAC translating backend: the arch_aspace_* family of the arch.h seam over the
// configured paging mode's tables.
//
// ONE ROOT REGISTER. A space carries the kernel's fixed high range as a copy of the boot root's
// own entries and every space shares the tables below them; activate writes satp, which is the
// only place it moves after boot.
//
// The leaf is level 0 and the root is the deepest level. Both are DERIVED from the configured
// paging mode in <kickos/arch/rv64_paging.h>, which the chip's prologue derives its own chain
// from; the handover below refuses a depth stated on one side of the seam only.
//
// TWO ROUTES TO A FRAME'S BYTES, and which one applies is the physical address:
//   - inside the kernel window the boot tables build, the frame is reached by adding the
//     window's fixed delta. Every table frame is here.
//   - outside it, through a TRANSIENT WINDOW of ARCH_ASPACE_ACQUIRE_MIN slots per core. A slot
//     is keyed on (space, page) and REFERENCE COUNTED, so repeated acquires of one page answer
//     one stable pointer and the mapping stands until the last release. Capacity is
//     ARCH_ASPACE_ACQUIRE_MIN DISTINCT pages per core, not that many calls.

#include <kickos/arch/arch.h>
#include <kickos/arch/rv64_paging.h>
#include <kickos/diag.h>
#include <kickos/extent.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    void kpanic(char const* msg) __attribute__((noreturn)); // a mispaired release refuses
}

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
    // The granule, the entry width and the index width are one fact three ways: a table holds
    // exactly one index's worth of entries or an index reaches past the page it names.
    static_assert(PTES == (static_cast<size_t>(1) << INDEX_BITS),
                  "an entry index does not span exactly one table page");
    static_assert(LEVEL_ROOT > LEVEL_LEAF, "a mode with no non-leaf level has no walk");

    // The mode's virtual space is split by sign extension, so the low half a space may map ends
    // here and everything above it is the kernel's window.
    constexpr uintptr_t LOW_HALF_END = static_cast<uintptr_t>(1) << (KICKOS_RV64_VA_BITS - 1);

    // satp fields at XLEN 64: MODE 63:60, ASID 59:44, PPN 43:0.
    constexpr unsigned SATP_MODE_SHIFT = KICKOS_RV64_SATP_MODE_SHIFT;
    constexpr unsigned SATP_ASID_SHIFT = 44;
    constexpr uint64_t SATP_ASID_MASK = 0xFFFFull << SATP_ASID_SHIFT;
    constexpr uint64_t SATP_PPN_MASK = (1ull << SATP_ASID_SHIFT) - 1u;
    // Derived from the depth this file walks: satp.MODE is numbered so each value adds one
    // level, so the mode is LEVEL_ROOT + 6 and the two cannot disagree.
    constexpr uint64_t SATP_MODE = static_cast<uint64_t>(LEVEL_ROOT) + 6u;
    constexpr uint64_t SATP_MODE_MASK = 0xFull;
    // LEVEL_ROOT is the depth every walk below runs at, and the chip's prologue builds its
    // tables from the mode on the right. A level count stated here instead of derived is
    // refused at compile time.
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
    // measurement below. satp.ASID stays 0 in every root this file installs.
    constexpr unsigned ASID_BITS_RECORDED = 16;

    // The window's slots per core, one per DISTINCT (space, page) held.
    constexpr size_t ACQUIRE_CAPACITY = ARCH_ASPACE_ACQUIRE_MIN;
    static_assert(ACQUIRE_CAPACITY * KICKOS_NUM_CORES <= PTES,
                  "the transient window outgrows the one level-0 table the chip provides");
    static_assert(ARCH_ASPACE_ACQUIRE_MIN <= ACQUIRE_CAPACITY,
                  "this backend cannot hold as many acquires live as arch.h promises");

    // Chip<->arch contract, handed over by arch_init before the first space exists
    // (arch/riscv/chip/virt_rv64/chip_virt_rv64.cc). Zero until then, which is what makes a
    // call before the handover answer "not mapped" rather than walk a null table.
    uint64_t* g_boot_root = nullptr;
    uint64_t* g_window_leaves = nullptr;
    uintptr_t g_window_va = 0;
    uintptr_t g_window_delta = 0;
    arch_phys_addr_t g_window_pa_lo = 0;
    arch_phys_addr_t g_window_pa_hi = 0;
    // The physical extent the PLATFORM implements, in bits; the chip is the only layer that
    // can answer. Zero refuses every range.
    unsigned g_phys_bits = 0;

    struct WindowSlot
    {
        struct arch_aspace* space;
        uintptr_t page;
        uint32_t holds; // 0 marks the slot free; space is null then too
    };
    WindowSlot g_slots[KICKOS_NUM_CORES][ACQUIRE_CAPACITY];

    // A hold count that reached this is refused rather than wrapped: a wrap would drop the
    // mapping under every caller still holding the pointer.
    constexpr uint32_t HOLDS_MAX = 0xFFFFFFFFu;

#if defined(KICKOS_ENABLE_SELFTEST)
    // Invalidation sequences the map editor issued, and the ones a not-installed space skipped:
    // the per-page shape and the whole-hart fence a fresh non-leaf entry owes. Every writer
    // holds the caller's IrqLock.
    uint32_t g_tlbi_issued = 0;
    uint32_t g_tlbi_elided = 0;
#endif

    // Releases that named no window hold and whose frame lies outside the kernel window: the
    // mispairing arch_aspace_release describes below. Kept in every build; only the reporting
    // through arch_aspace_tlbi_counts is self-test-only. Saturates. Written under the caller's
    // masked window above.
    uint8_t g_release_mispaired = 0;

    uint64_t read_satp()
    {
        uint64_t satp = 0;
        __asm volatile("csrr %0, satp" : "=r"(satp));
        return satp;
    }

    arch_phys_addr_t pte_pa(uint64_t pte)
    {
        return static_cast<arch_phys_addr_t>((pte & PTE_PPN_MASK) >> 10) << GRANULE_SHIFT;
    }

    uint64_t pa_ppn(arch_phys_addr_t pa)
    {
        return (static_cast<uint64_t>(pa >> GRANULE_SHIFT) << 10) & PTE_PPN_MASK;
    }

    // A table frame is reached by the kernel window's delta: the frame pool is carved inside
    // the image's DRAM share, so every table frame falls in the range the boot tables map.
    uint64_t* table_at(arch_phys_addr_t pa)
    {
        return reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(pa) + g_window_delta);
    }

    arch_phys_addr_t phys_of(void const* p)
    {
        return static_cast<arch_phys_addr_t>(reinterpret_cast<uintptr_t>(p) - g_window_delta);
    }

    // The bound is the DRAM gigabyte the boot tables describe, not every address that delta
    // reaches. The device gigapage is mapped at the same delta and is windowed anyway, which is
    // what keeps the window pool exercised at all.
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
    // before the next translated access. rs2 = zero names every address space, nothing here
    // tagging a translation.
    void invalidate_page(uintptr_t va)
    {
#if defined(KICKOS_ENABLE_SELFTEST)
        g_tlbi_issued++;
#endif
        __asm volatile("fence w, w" ::: "memory");
        __asm volatile("sfence.vma %0, zero" ::"r"(va) : "memory");
    }

    void invalidate_all()
    {
        __asm volatile("fence w, w" ::: "memory");
        __asm volatile("sfence.vma zero, zero" ::: "memory");
    }

#if KICKOS_KERNEL_CORES > 1
    // The satp PPN each core last had written. write_satp is its one writer, and
    // kickos_rv64_aspace_boot seeds every entry: the machine-mode prologue writes the SAME boot
    // root on every hart, so a core that has never activated anything is on it by construction.
    uint64_t g_installed_root[KICKOS_NUM_CORES] = {};
#endif

    // THE ONE WRITER of the installed-root record, placed where the register is written so the
    // two cannot disagree. A record that OVER-reports is not harmless: the switch path skips
    // the activate when the cell already names the incoming space.
    void write_satp(uint64_t satp)
    {
        __asm volatile("csrw satp, %0" ::"r"(satp) : "memory");
        __asm volatile("sfence.vma zero, zero" ::: "memory");
#if KICKOS_KERNEL_CORES > 1
        g_installed_root[arch_cpu_id()] = satp & SATP_PPN_MASK;
#endif
    }

    // A NON-LEAF entry that has just become valid. The per-leaf fence orders leaf entries only,
    // and a hart may cache PTEs whose V bit is clear, so a page-walk cache holding the absence
    // of this table keeps answering absent for every page under it. Privileged ISA 12.2.1 gives
    // one remedy: SFENCE.VMA with rs1 = x0, and rs2 = x0 because no translation here is tagged.
    // There is no narrower form.
    void invalidate_nonleaf(bool installed)
    {
        if (not installed)
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

    // Whether `space` is the root THIS core is running on, asked of satp itself.
    bool installed_here(struct arch_aspace* space)
    {
        return (read_satp() & SATP_PPN_MASK) == (satp_of(root_of(space)) & SATP_PPN_MASK);
    }

    // Bit c set where core c's satp names `space`: the ACTIVE-CORE SET, DERIVED from what the
    // backend last installed rather than held on the space. A peer's satp is unreadable from
    // here, so a peer's half comes from the record above and this core's from the register.
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

    // A space no core has installed has no cached translation and no cached absence, so its
    // seeding costs no maintenance AT EVERY CORE COUNT: the record is what keeps the elision
    // alive above one core, where the register alone could only answer for this one.
    bool installed_anywhere(struct arch_aspace* space)
    {
        return active_cores(space) != 0;
    }

    // A descriptor written without an invalidation still owes the fence that makes the new
    // entry visible to a walker before any later activation reads it. ONE PER CALL covers a
    // whole run of pages, which is what keeps the elision's measurement intact.
    void publish_edits()
    {
#if KICKOS_KERNEL_CORES > 1
        __asm volatile("fence w, w" ::: "memory");
#endif
    }

    void invalidate_page_if(uintptr_t va, bool installed)
    {
        if (not installed)
        {
#if defined(KICKOS_ENABLE_SELFTEST)
            g_tlbi_elided++;
#endif
            return;
        }
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

    // An entry carries permissions and no memory type: the attribute is the physical address's,
    // fixed by the platform, and Svpbmt is absent on this part (menvcfg.PBMTE reads back 0).
    bool memtype_known(enum arch_map_memtype type)
    {
        return type == ARCH_MAP_NORMAL or type == ARCH_MAP_NOCACHE or type == ARCH_MAP_DEVICE;
    }

    // A leaf for the unprivileged level. W without R is reserved by the architecture, so a
    // request with no read is refused.
    bool leaf_attrs(uint32_t rights, enum arch_map_memtype type, uint64_t* out)
    {
        uint32_t const known = ARCH_MAP_R | ARCH_MAP_W | ARCH_MAP_X;
        if ((rights & ~known) != 0 or (rights & ARCH_MAP_R) == 0)
        {
            return false;
        }
        // Writable and executable at once is expressible here and refused: a page granted both
        // is one an unprivileged thread can turn into code.
        if ((rights & ARCH_MAP_W) != 0 and (rights & ARCH_MAP_X) != 0)
        {
            return false;
        }
        if (not memtype_known(type))
        {
            return false;
        }
        // A and D set: this port programs no hardware update of either, so a leaf that left
        // them clear would fault on its first access.
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

    // Frees every table under `table` and every leaf output it names. `keep` is the boot root's
    // entry array at LEVEL_ROOT and null below it: an entry a create copied from the boot root
    // names tables every space shares, so walking into one would free the next space's.
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
            // A leaf output the pool never handed out is refused inside the free, which is what
            // lets a space hold a device page without destroy reclaiming it.
            kickos_frame_free(out);
            table[i] = 0;
        }
    }

    enum arch_aspace_result map_into(uint64_t* table, int level, uintptr_t va, size_t pages,
                                     arch_phys_addr_t pa, uint64_t leaf, bool installed)
    {
        while (pages != 0)
        {
            size_t const idx = index_at(va, level);
            if (level == LEVEL_LEAF)
            {
                if ((table[idx] & PTE_V) != 0)
                {
                    // The invalidate belongs BETWEEN the two writes: a walk that took the old
                    // entry's output and the new entry's permissions would be neither.
                    table[idx] = 0;
                    invalidate_page_if(va, installed);
                }
                table[idx] = leaf | pa_ppn(pa);
                // A fresh slot needs one too: an absence may be cached like a presence.
                invalidate_page_if(va, installed);
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
                zero_table(table_at(frame));
                desc = pa_ppn(frame) | PTE_V;
                __asm volatile("fence w, w" ::: "memory");
                table[idx] = desc;
                // The per-leaf fence below names leaves only, so it does not reach this entry.
                invalidate_nonleaf(installed);
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
    // an entry the boot root owns is never pruned.
    bool prune_empty(uint64_t* table, int level, uint64_t const* keep)
    {
        if (level == LEVEL_LEAF)
        {
            return table_empty(table);
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
        return table_empty(table);
    }

    // Null when the range is not wholly mapped, which is what makes unmap total-or-fail.
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

    // The low half only; above it is the kernel's window, whose entries every space shares.
    // The single statement of that boundary every guard below asks.
    bool low_half_page(uintptr_t page)
    {
        return page < LOW_HALF_END;
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

    // The whole output extent [pa, pa + pages * GRANULE), tested BEFORE any entry is written,
    // against BOTH bounds a leaf has: pa_ppn masks to the PPN field, so a range whose LAST page
    // reaches past it wraps onto low physical memory while every write reports OK, and a range
    // the machine does not implement access-faults on the walk. The first page passing says
    // nothing about the rest.
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
        // A platform naming the whole pointer width bounds nothing, and the shift below would
        // be undefined at 64.
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

    // A frame outside the kernel window, mapped into a slot of this core's own. Null when every
    // slot holds a different page.
    //
    // A page this core already holds answers the SAME slot with its count raised, so the pointer
    // is stable across a second acquire and the mapping outlives a release taken in any order.
    // A release names (space, page) and nothing else.
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
            // Kernel data: no U and no X. PTE_G because the slot IS a mapping every space
            // shares, and G is not inherited from the non-leaf entries above (Privileged ISA
            // 12.3.1), which carry none.
            *slot_entry(core, i) =
                pa_ppn(pa) | PTE_V | PTE_G | PTE_R | PTE_W | PTE_A | PTE_D;
            __asm volatile("fence w, w" ::: "memory");
            __asm volatile("sfence.vma %0, zero" ::"r"(slot_va(core, i)) : "memory");
            g_slots[core][i].space = space;
            g_slots[core][i].page = page;
            g_slots[core][i].holds = 1;
            return reinterpret_cast<void*>(slot_va(core, i));
        }
        return nullptr;
    }

    // Surrenders one hold of (space, page), and says whether one matched. The slot is unmapped
    // on the LAST hold only, so every other holder's pointer stays live whatever order the
    // releases arrive in.
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
            __asm volatile("fence w, w" ::: "memory");
            __asm volatile("sfence.vma %0, zero" ::"r"(slot_va(core, i)) : "memory");
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

    // The identifier field the hart implements, by writing ones across it. Safe under live
    // translation: it preserves MODE and PPN, moves satp.ASID alone, and fences on both sides.
    //
    // WARL legal values need not be a contiguous low run, so the readback's POPCOUNT is not a
    // width. The width is the highest bit that stuck plus one, and whether the bits below it
    // stuck is reported BESIDE the figure rather than folded into it.
    AsidField measure_asid_field()
    {
        arch_irq_state_t const s = arch_irq_save();
        uint64_t const satp = read_satp();
        __asm volatile("sfence.vma zero, zero" ::: "memory");
        __asm volatile("csrw satp, %0" ::"r"(satp | SATP_ASID_MASK) : "memory");
        uint64_t const back = read_satp();
        __asm volatile("csrw satp, %0" ::"r"(satp) : "memory");
        __asm volatile("sfence.vma zero, zero" ::: "memory");
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
        // The PLATFORM's extent: every RV64 mode encodes 56 bits of output in the PPN field,
        // and an output the machine does not implement access-faults on the walk. The same
        // bound phys_range_ok refuses against.
        pa_bits = g_phys_bits;
    }
    AsidField const asid = measure_asid_field();
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
    // The widest output address this port programs is the top of the frame pool's carve, which
    // the kernel window covers, so a range reaching the window's own top covers every leaf.
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
    // The boot root's own entries, so the kernel's fixed high range is present and every space
    // shares the tables below them; a later kernel-half edit stays in step.
    for (size_t i = 0; i < PTES; i++)
    {
        table[i] = g_boot_root[i];
    }
    __asm volatile("fence w, w" ::: "memory");
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
    // A space installed on no core has no cached entry and no cached absence, so its whole
    // seeding costs no maintenance; the running space's own widening still pays.
    bool const installed = installed_anywhere(space);
    enum arch_aspace_result const rc =
        map_into(root_of(space), LEVEL_ROOT, va, pages, pa, leaf, installed);
    publish_edits();
    if (rc != ARCH_ASPACE_OK)
    {
        // Masked across the whole unwind: this space can be the running one, the self-grant
        // widening it mid-syscall, and a walk between the invalidate and the frees would cache
        // a table about to go back to the pool.
        arch_irq_state_t const s = arch_irq_save();
        // Leaves first and tables second: an entry left valid over a freed table would be a
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
        (void)prune_empty(root_of(space), LEVEL_ROOT, g_boot_root);
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
    bool const installed = installed_anywhere(space);
    for (size_t i = 0; i < pages; i++)
    {
        uintptr_t const at = va + static_cast<uintptr_t>(i) * GRANULE;
        uint64_t* const entry = leaf_entry(root_of(space), at);
        *entry = 0;
        invalidate_page_if(at, installed);
    }
    publish_edits();
    // An intermediate table left empty is not a leak: destroy walks the tree and frees every
    // table under the root.
    return ARCH_ASPACE_OK;
}

void arch_aspace_activate(struct arch_aspace* space)
{
    if (space == nullptr)
    {
        return;
    }
    // Every space carries the same root entries for the kernel window, the devices and the
    // transient window, so the code making this write keeps its own mappings across it.
    //
    // No identifier is allocated anywhere, so the fence names every address space.
    arch_irq_state_t const s = arch_irq_save();
    write_satp(satp_of(root_of(space)));
    arch_irq_restore(s);
}

// The space the boot path installed (arch.h, arch_aspace_boot).
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
    // The half test arch_aspace_frame_at makes: a frame this walk reached through the kernel's
    // own entries would be handed back as a POINTER the caller may write through.
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

// A mispairing is COUNTED, not panicked over: this member is on the fault reporter's descent
// (kaccess_to_user through access_copy), so a panic here would fire inside the record it is
// writing. map_tlbi_elided reading g_release_mispaired through arch_aspace_tlbi_counts is what
// refuses it, and a build with no self-test reports it nowhere.
//
// The test sees a second release of a page reached THROUGH a slot, whose frame is outside the
// kernel window. A page acquired by the OFFSET route spends no slot, so a repeat there
// surrenders nothing.
void arch_aspace_release(struct arch_aspace* space, uintptr_t va)
{
    if (space == nullptr or g_window_leaves == nullptr)
    {
        return;
    }
    uintptr_t const page = va & ~static_cast<uintptr_t>(GRANULE - 1);
    // An address outside the half held no slot, acquire having refused it. Falling through
    // would walk the kernel's own entries and count a caller's bad address as the defect
    // below.
    if (not low_half_page(page))
    {
        return;
    }
    arch_irq_state_t const s = arch_irq_save();
    bool defect = false;
    if (not window_drop(space, page))
    {
        // No slot held this page. A hold the OFFSET route took spends none, so nothing to drop
        // is the whole answer. A frame OUTSIDE the kernel window is only ever reached THROUGH a
        // slot, so a release there pairs with no hold and the next release of the same page
        // would surrender a live one. The route is the same question acquire asked.
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
    // acquire's guard: the walk reaches every table through the kernel window's delta, which is
    // 0 until the chip hands it over, so a call before the handover would read a table at its
    // output address.
    if (space == nullptr or g_window_leaves == nullptr)
    {
        return 0;
    }
    uintptr_t const page = va & ~static_cast<uintptr_t>(GRANULE - 1);
    // Map and unmap's own half test, aligned down first because this member's `va` need not be
    // granule-aligned where range_ok's is. The walk reads the index bits of `va` alone, so
    // without this an address above the half ALIASES onto a low-half page and is answered with
    // that page's frame.
    if (not low_half_page(page))
    {
        return 0;
    }
    // Masked for the same reason the map unwind masks: it clears leaves and frees tables
    // under one mask, and a walk interleaved with it would read a table already back in the
    // pool.
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

// Chip<->arch contract: the boot root, the transient window's level-0 table and its base
// address, and the delta between an output address inside the image's DRAM share and the kernel
// address that reaches it. Must run before any space exists.
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
#if KICKOS_KERNEL_CORES > 1
    // Every hart's machine-mode prologue wrote THIS root into its own satp, so a core that has
    // activated nothing yet is on it. Seeded rather than left zero: a zero cell names no space
    // and would leave that core accounted for by nobody.
    uint64_t const boot_ppn = satp_of(user_root) & SATP_PPN_MASK;
    for (size_t c = 0; c < KICKOS_NUM_CORES; c++)
    {
        g_installed_root[c] = boot_ppn;
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
