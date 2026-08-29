// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// RISC-V RV64IMAC translating backend: the arch_aspace_* family of the arch.h seam over the
// configured paging mode's tables (RISC-V Privileged ISA, "Sv39 Page Table Entry"; the entry
// format is the same in every mode on this ladder).
//
// ONE ROOT REGISTER, so a space carries the kernel's fixed high range as a copy of the boot
// root's own entries and every space shares the tables below them. ONE ROOT ALTOGETHER
// since R2.2: the app links into a root slot of its own, so the U bit follows the half and
// no privilege transition changes satp. Activate writes it here, which is the only place it
// moves after boot.
//
// LEVELS RUN THE OTHER WAY FROM A64's NUMBERING: the leaf is level 0 and the root is the
// deepest level. Every walk below is written over those two, and both are DERIVED from the
// configured paging mode in <kickos/arch/rv64_paging.h>, which the chip's prologue derives its
// own chain from. So a mode change is a Kconfig line rather than an edit here, and a depth
// stated on one side of the chip<->arch seam only is refused at the handover below
// (docs/design-m6-mmu.md R3).
//
// TWO ROUTES TO A FRAME'S BYTES, and which one applies is the physical address:
//   - inside the kernel window the boot tables build, the frame is reached by adding the
//     window's fixed delta. Every table frame is here, the frame pool being carved inside the
//     image's DRAM share, so no walk spends a window slot.
//   - outside it, the frame is reached through a TRANSIENT WINDOW of ARCH_ASPACE_ACQUIRE_MIN
//     slots per core, mapped and unmapped by the acquire/release pair.

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
    // satp programs 4 KiB, the only granule these modes define; a 2 MiB or 1 GiB leaf is a
    // larger mapping and not a second granule (docs/design-m6-mmu.md F7).
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
    // THE MODE THIS FILE WRITES IS DERIVED FROM THE DEPTH IT WALKS, and not from the Kconfig
    // symbol a second time: satp.MODE is numbered so each value adds one level, so the mode is
    // LEVEL_ROOT + 6 and the two cannot disagree. The chip's prologue programmed satp from the
    // same symbol by its own route, which is what makes the comparison at the handover an
    // oracle rather than a tautology (docs/design-m6-mmu.md R3).
    constexpr uint64_t SATP_MODE = static_cast<uint64_t>(LEVEL_ROOT) + 6u;
    constexpr uint64_t SATP_MODE_MASK = 0xFull;
    // AND THE DEPTH IS HELD TO THE CONFIGURED MODE HERE. Not a tautology: LEVEL_ROOT is the
    // depth every walk below actually runs at, and the chip's prologue builds its tables and
    // programs satp from the mode on the right of this comparison. A level count stated here
    // rather than derived is refused by name at compile time instead of walking a tree one
    // level shallower than the hardware does.
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

    // The identifier width a human wrote for this class of part, compared against the WARL
    // measurement below. Nothing here allocates, generates or scopes anything on an
    // identifier: satp.ASID stays 0 in every root this file installs
    // (docs/design-m6-mmu.md R4).
    constexpr unsigned ASID_BITS_RECORDED = 16;

    // The physical address space the mode publishes. The architecture names no register
    // reporting the range, so the figure stands on the MODE the machine accepted and that mode
    // is read back rather than restated.
    constexpr unsigned PA_BITS = KICKOS_RV64_PA_BITS;

    // The window's slots per core. Sized for the seam's floor and asserted against the level-0
    // table the chip hands over, which holds PTES of them.
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

    struct WindowSlot
    {
        struct arch_aspace* space;
        uintptr_t page;
    };
    WindowSlot g_slots[KICKOS_NUM_CORES][ACQUIRE_CAPACITY];

#if defined(KICKOS_ENABLE_SELFTEST)
    // Invalidation sequences the map editor issued, and the ones a not-installed space
    // skipped. Both shapes are counted, the per-page one and the whole-hart fence a fresh
    // non-leaf entry owes, so the figure answers "did the editor do its maintenance" rather
    // than only "did it do the per-page half". Every writer holds the caller's IrqLock.
    uint32_t g_tlbi_issued = 0;
    uint32_t g_tlbi_elided = 0;
#endif

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

    // A table frame is reached by the kernel window's delta and never through the transient
    // window: the frame pool is carved inside the image's DRAM share, so every frame this file
    // allocates for a table falls in the range the boot tables already map.
    uint64_t* table_at(arch_phys_addr_t pa)
    {
        return reinterpret_cast<uint64_t*>(static_cast<uintptr_t>(pa) + g_window_delta);
    }

    arch_phys_addr_t phys_of(void const* p)
    {
        return static_cast<arch_phys_addr_t>(reinterpret_cast<uintptr_t>(p) - g_window_delta);
    }

    // The bound is the DRAM gigabyte the boot tables describe and NOT every address that
    // delta reaches: the device gigapage is mapped at the SAME delta, so an MMIO frame could
    // be answered by an addition too and is windowed instead. Deliberate, because every frame
    // the pool hands out is inside this bound, so the device frames are the only thing that
    // exercises the window pool at all and widening it would leave the fleet's only backend
    // that can fail ARCH_ASPACE_ACQUIRE_MIN with no live caller (docs/design-m6-mmu.md R4).
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

    // A NON-LEAF entry that has just become valid, which the per-leaf fence below it does NOT
    // cover. Privileged ISA 12.2.1: with rs1 != x0 and rs2 == x0 the fence "orders only reads
    // and writes made to leaf page table entries corresponding to the virtual address in rs1",
    // and the same section permits "the caching of PTEs whose V (Valid) bit is clear". So a
    // hart with a page-walk cache that cached the absence of this table keeps answering absent
    // for every page under it. The section's own remedy is the only one the ISA offers:
    // "If software modifies a non-leaf PTE, it should execute SFENCE.VMA with rs1=x0", and rs2
    // is x0 too because no translation here is tagged.
    //
    // The cost is a whole-hart flush, at most once per fresh table rather than once per page,
    // and there is no narrower form: no operand names the non-leaf entries for one address.
    void invalidate_nonleaf(bool installed)
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
#if defined(KICKOS_ENABLE_SELFTEST)
        g_tlbi_issued++;
#endif
        invalidate_all();
    }

    // Whether `space` is the root this core is RUNNING on. A space installed nowhere has no
    // cached translation and no cached absence: activate fences the whole space when it
    // installs a root, so nothing survives from a root this core is not on.
    //
    // The question is asked of satp itself now that there is one root, which is one fewer
    // shadow of the hardware to keep in step.
    //
    // At one core that is the whole question. A second core holds a root this one cannot read,
    // so the elision is compiled out above one core (docs/design-m6-mmu.md T9).
    bool installed_here(struct arch_aspace* space)
    {
        return (read_satp() & SATP_PPN_MASK) == (satp_of(root_of(space)) & SATP_PPN_MASK);
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

    // An entry carries permissions and NO memory type: the attribute is the physical
    // address's, fixed by the platform, and Svpbmt is absent on this part (menvcfg.PBMTE reads
    // back 0). Nothing here can downgrade a type because nothing here encodes one, and this
    // board reaches no frame through a data cache an observer outside the coherency would miss,
    // which is the state ARCH_MPU_NOCACHE_ALREADY names on the region seam beside it.
    bool memtype_known(enum arch_map_memtype type)
    {
        return type == ARCH_MAP_NORMAL or type == ARCH_MAP_NOCACHE or type == ARCH_MAP_DEVICE;
    }

    // A leaf for the unprivileged level. W without R is reserved by the architecture, so a
    // request with no read names permissions the entry format cannot express and is refused
    // rather than widened.
    bool leaf_attrs(uint32_t rights, enum arch_map_memtype type, uint64_t* out)
    {
        uint32_t const known = ARCH_MAP_R | ARCH_MAP_W | ARCH_MAP_X;
        if ((rights & ~known) != 0 or (rights & ARCH_MAP_R) == 0)
        {
            return false;
        }
        // Writable and executable at once is expressible here and refused anyway: a page
        // granted both is one an unprivileged thread can turn into code, and the descriptor is
        // the only thing there is to take it back with.
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
    // names the kernel half, whose tables every space shares, so walking into one would free
    // the tables the next space still points at.
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

    // Recursion is bounded by the level count.
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
                // A fresh slot needs one too: an absence may be cached like a presence, so the
                // new leaf is invisible until the stale negative entry is dropped.
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
                // The per-leaf fence below does NOT reach this entry: it names leaves only.
                // A page in the new table's span that this call does not map stays unmapped,
                // and a cached absence for it is still the right answer.
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

    // The low half only. Above it is the kernel's window, whose entries every space shares,
    // and this is the SINGLE statement of that boundary every guard below asks: a second copy
    // of it is a second truth.
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

    uintptr_t slot_va(size_t core, size_t slot)
    {
        return g_window_va + ((core * ACQUIRE_CAPACITY) + slot) * GRANULE;
    }

    uint64_t* slot_entry(size_t core, size_t slot)
    {
        return &g_window_leaves[(core * ACQUIRE_CAPACITY) + slot];
    }

    // A frame outside the kernel window, mapped into a slot of this core's own. Null when every
    // slot is held, which is the refusal a caller past ARCH_ASPACE_ACQUIRE_MIN gets.
    void* window_take(struct arch_aspace* space, uintptr_t page, arch_phys_addr_t pa)
    {
        size_t const core = static_cast<size_t>(arch_cpu_id());
        for (size_t i = 0; i < ACQUIRE_CAPACITY; i++)
        {
            if (g_slots[core][i].space != nullptr)
            {
                continue;
            }
            // Kernel data: no U and no X, so a slot is unreachable from an unprivileged thread
            // whichever root is live and nothing can be executed out of it. PTE_G because the
            // slot IS a mapping every space shares: create copies the boot root's entry for
            // this half, so every space walks these same tables. The non-leaf entries above
            // this one carry no G, so nothing here inherits it (Privileged ISA 12.3.1), and
            // every leaf startup.S assembles in this half sets it.
            *slot_entry(core, i) =
                pa_ppn(pa) | PTE_V | PTE_G | PTE_R | PTE_W | PTE_A | PTE_D;
            __asm volatile("fence w, w" ::: "memory");
            __asm volatile("sfence.vma %0, zero" ::"r"(slot_va(core, i)) : "memory");
            g_slots[core][i].space = space;
            g_slots[core][i].page = page;
            return reinterpret_cast<void*>(slot_va(core, i));
        }
        return nullptr;
    }

    // Surrenders one hold of (space, page), and says whether one matched. The key is the pair
    // and not the individual hold, so a release beside a live hold of the same page takes THAT
    // hold; what separates a mispaired release from a hold the offset route took is the route,
    // which arch_aspace_release asks below.
    bool window_drop(struct arch_aspace* space, uintptr_t page)
    {
        size_t const core = static_cast<size_t>(arch_cpu_id());
        for (size_t i = 0; i < ACQUIRE_CAPACITY; i++)
        {
            if (g_slots[core][i].space != space or g_slots[core][i].page != page)
            {
                continue;
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
    // translation because it preserves MODE and PPN, moves satp.ASID alone, and fences on both
    // sides (docs/design-m6-mmu.md R4).
    //
    // WARL means the legal values need not be a contiguous low run, so the readback's
    // POPCOUNT is not a width: a hart implementing a non-contiguous subset would report fewer
    // bits than the field it has. The width is the highest bit that stuck plus one, and
    // whether the bits below it stuck is reported BESIDE the figure rather than folded into
    // it, so such a hart reports its real span and clears the verdict bit instead of matching
    // the record with a narrower number.
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
    // The mode the machine ACCEPTED, read back rather than restated: satp.MODE is WARL, so a
    // mode this hart does not implement would not be standing here.
    unsigned const mode = static_cast<unsigned>((read_satp() >> SATP_MODE_SHIFT) & SATP_MODE_MASK);
    uint64_t granules = 0;
    unsigned pa_bits = 0;
    if (mode == SATP_MODE)
    {
        granules = 1; // the architecture defines one page size for this mode
        pa_bits = PA_BITS;
    }
    AsidField const asid = measure_asid_field();
    uint64_t out = 0;
    if (granules != 0 and GRANULE == 4096u)
    {
        out |= ARCH_ASPACE_MODEL_GRANULE;
    }
    // A span matching the record through bits the hart does not all implement is not the
    // field the record names, so contiguity gates the verdict bit and not the reported width.
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
    // shares the tables below them: a later kernel-half edit stays in step, and copying one
    // level deeper would make that half per-space state every edit had to walk.
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
    // while such an entry stands would be read as descriptors after the pool hands it out as
    // data.
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
    if ((pa & static_cast<arch_phys_addr_t>(GRANULE - 1)) != 0
        or pa_ppn(pa) != ((static_cast<uint64_t>(pa >> GRANULE_SHIFT) << 10)))
    {
        return ARCH_ASPACE_EINVAL; // misaligned, or an output past the PPN field
    }
    uint64_t leaf = 0;
    if (not leaf_attrs(rights, type, &leaf))
    {
        return ARCH_ASPACE_EINVAL;
    }
    bool const installed = installed_here(space);
    enum arch_aspace_result const rc =
        map_into(root_of(space), LEVEL_ROOT, va, pages, pa, leaf, installed);
    if (rc != ARCH_ASPACE_OK)
    {
        // Masked across the whole unwind: this space can be the running one, the self-grant
        // widening it mid-syscall, and a walk between the invalidate and the frees would cache
        // a table about to go back to the pool.
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
    bool const installed = installed_here(space);
    for (size_t i = 0; i < pages; i++)
    {
        uintptr_t const at = va + static_cast<uintptr_t>(i) * GRANULE;
        uint64_t* const entry = leaf_entry(root_of(space), at);
        *entry = 0;
        invalidate_page_if(at, installed);
    }
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
    // THE RUNNING TRANSLATION, kernel half included. Every space carries the same root entries
    // for the kernel window, the devices and the transient window, so the code making this
    // write keeps its own mappings across it; what changes is the app's slot and every
    // per-space range below it.
    //
    // No identifier is allocated here or anywhere (docs/design-m6-mmu.md R4), so the fence
    // names every address space and drops the outgoing one's translations with the incoming
    // one's absences.
    arch_irq_state_t const s = arch_irq_save();
    __asm volatile("csrw satp, %0" ::"r"(satp_of(root_of(space))) : "memory");
    __asm volatile("sfence.vma zero, zero" ::: "memory");
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
    // The half test arch_aspace_frame_at makes, for the same aliasing and for one more: a
    // frame this walk reaches through the kernel's own entries would be handed back as a
    // POINTER the caller may write through.
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

void arch_aspace_release(struct arch_aspace* space, uintptr_t va)
{
    if (space == nullptr or g_window_leaves == nullptr)
    {
        return;
    }
    uintptr_t const page = va & ~static_cast<uintptr_t>(GRANULE - 1);
    // REFUSED AND NOT A DEFECT. An address outside the half held no slot, acquire having
    // refused it, so there is nothing to drop and nothing mispaired; falling through would
    // walk the kernel's own entries and reach the panic below on a caller's bad address.
    if (not low_half_page(page))
    {
        return;
    }
    arch_irq_state_t const s = arch_irq_save();
    bool defect = false;
    if (not window_drop(space, page))
    {
        // No slot held this page, and the two reasons for that are not the same one. A hold
        // the OFFSET route took spends no slot, so nothing to drop is the whole answer for it.
        // A frame OUTSIDE the kernel window is only ever reached THROUGH a slot, so a release
        // there pairs with no hold of its own and the next release of the same page would
        // surrender a live one. The route is decided by the same question acquire asked.
        uint64_t const* const entry = leaf_entry(root_of(space), page);
        defect = entry != nullptr and not in_kernel_window(pte_pa(*entry));
    }
    arch_irq_restore(s);
    if (defect)
    {
        kickos::kpanic(kickos::diag::kAspaceRelease);
    }
}

arch_phys_addr_t arch_aspace_frame_at(struct arch_aspace* space, uintptr_t va)
{
    // acquire's guard, for its reason: the walk reaches every table through the kernel
    // window's delta, which is 0 until the chip hands it over, so a call before the handover
    // would read a table at its output address.
    if (space == nullptr or g_window_leaves == nullptr)
    {
        return 0;
    }
    uintptr_t const page = va & ~static_cast<uintptr_t>(GRANULE - 1);
    // MAP AND UNMAP'S OWN HALF TEST, and aligned down first because this member's `va` need
    // not be granule-aligned where range_ok's is. The walk reads the index bits of `va` alone,
    // so without this an address above the half ALIASES onto a low-half page and is answered
    // with that page's frame; where the index bits reach the kernel window instead, the tables
    // walked are the ones every space shares.
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
// address that reaches it. Called from arch_init before any space exists.
void kickos_rv64_aspace_boot(uint64_t* user_root, uint64_t* window_leaves, uintptr_t window_va,
                             uintptr_t window_delta, arch_phys_addr_t pa_lo,
                             arch_phys_addr_t pa_hi)
{
    g_boot_root = user_root;
    g_window_leaves = window_leaves;
    g_window_va = window_va;
    g_window_delta = window_delta;
    g_window_pa_lo = pa_lo;
    g_window_pa_hi = pa_hi;
    for (size_t c = 0; c < KICKOS_NUM_CORES; c++)
    {
        for (size_t i = 0; i < ACQUIRE_CAPACITY; i++)
        {
            g_slots[c][i].space = nullptr;
            g_slots[c][i].page = 0;
        }
    }
}

#if defined(KICKOS_ENABLE_SELFTEST)
uint64_t arch_aspace_tlbi_counts(void)
{
    return (static_cast<uint64_t>(g_tlbi_issued) << 32) | static_cast<uint64_t>(g_tlbi_elided);
}
#endif

}
