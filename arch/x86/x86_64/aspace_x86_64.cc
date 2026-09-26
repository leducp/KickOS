// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// x86-64 page tables (Intel SDM Vol. 3, chapter 5).
// Adopt the firmware root and its runtime paging depth. New spaces copy
// its kernel entries and share the child tables. Acquire checks the boot
// mapping instead of assuming identity mapping. PCIDs are disabled;
// CR3 writes invalidate non-global translations.

#include <kickos/arch/arch.h>
#include <kickos/arch/aspace.h>
#include <kickos/arch/aspace_residency.h>
#include <kickos/arch/aspace_table.h>
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
#if KICKOS_KERNEL_CORES > 1
    uint32_t kickos_x86_64_online_cores(void);
#endif
}

namespace
{
    using namespace kickos::q35;

    // The granule is 4 KiB; 2 MiB and 1 GiB mappings use larger leaves.
    constexpr unsigned GRANULE_SHIFT = 12;
    constexpr size_t GRANULE = static_cast<size_t>(1) << GRANULE_SHIFT;
    constexpr size_t PTES = GRANULE / sizeof(uint64_t);
    constexpr unsigned INDEX_BITS = 9;
    constexpr int LEVEL_LEAF = 1;
    // PS is valid only at levels 2 and 3, never at the root or level 4.
    constexpr int LEVEL_LARGEST_LEAF = 3;
    constexpr int LEVEL_MAX = 5;

    // Acquire uses no temporary slots.
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
    // Bit 7 selects PAT only in 4 KiB leaves. Larger leaves use bit 12.
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

    // This backend does not allocate PCIDs.
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

    // Static kernel-window tables, needed before the frame pool is initialized.
    // Indexed by level minus one; initially unmapped.
    alignas(4096) uint64_t g_kwin_table[LEVEL_MAX - 1][PTES] = {};
    uintptr_t g_kwin_va = 0;

#if defined(KICKOS_ENABLE_SELFTEST)
    // Invalidation counters, protected by the caller's IrqLock.
    uint32_t g_tlbi_issued = 0;
    uint32_t g_tlbi_elided = 0;
#endif

    // Root-keyed residency, updated by install_root and retained after switching away.
    kickos::aspace::Residency<KICKOS_NUM_CORES> g_residency;

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

    // Read MAXPHYADDR, capped at 52 bits. Use 36 if the CPUID leaf is absent
    // (Intel SDM Vol. 3, section 5.1.4).
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

    // Validate alignment and the whole physical range against MAXPHYADDR and
    // the 52-bit descriptor limit to prevent truncation into low memory.
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

    // aspace_init verifies identity mapping before these pointers are used.
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

    // Recognize large leaves only at levels 2 and 3, where PS is defined.
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

    // INVLPG also invalidates paging-structure caches for this PCID, so newly
    // installed table entries need no separate invalidation
    // (Intel SDM Vol. 3, section 5.10.4.1).
    void invalidate_page(uintptr_t va)
    {
#if defined(KICKOS_ENABLE_SELFTEST)
        g_tlbi_issued++;
#endif
        __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
#if KICKOS_KERNEL_CORES > 1
        uint32_t const peers = kickos_x86_64_online_cores() & ~(1u << arch_cpu_id());
        arch_ipi_send(peers);
        arch_ipi_wait(peers);
#endif
    }

    // Keep the root register and residency record in sync.
    void install_root(uint64_t cr3)
    {
        write_cr3(cr3);
        g_residency.note(cr3 & PTE_ADDR_MASK, arch_cpu_id());
    }

    // Reload CR3 to invalidate non-global translations and paging-structure
    // caches. This backend creates no global entries (section 5.10.4.1).
    void invalidate_all(void)
    {
        install_root(read_cr3());
#if KICKOS_KERNEL_CORES > 1
        uint32_t const peers = kickos_x86_64_online_cores() & ~(1u << arch_cpu_id());
        arch_ipi_send(peers);
        arch_ipi_wait(peers);
#endif
    }

    uint64_t root_key(struct arch_aspace* space)
    {
        return static_cast<uint64_t>(phys_of(root_of(space))) & PTE_ADDR_MASK;
    }

    // Use recorded residency for maintenance, including spaces switched away from.
    bool resident_anywhere(struct arch_aspace* space)
    {
        return g_residency.cores(root_key(space)) != 0;
    }

#if defined(KICKOS_ENABLE_SELFTEST)
    // Current root, for self-tests only.
    bool installed_here(struct arch_aspace* space)
    {
        return (read_cr3() & PTE_ADDR_MASK) == (phys_of(root_of(space)) & PTE_ADDR_MASK);
    }
#endif

    void invalidate_page_if(uintptr_t va, bool resident)
    {
#if KICKOS_NUM_CORES == 1
        if (not resident)
        {
#if defined(KICKOS_ENABLE_SELFTEST)
            g_tlbi_elided++;
#endif
            return;
        }
#else
        (void)resident;
#endif
        invalidate_page(va);
    }

    // Compose leaf memory-type bits using the live PAT.
    bool memtype_bits(enum arch_map_memtype type, uint64_t* out)
    {
        return kickos::x86_64::aspace_memtype_bits(kickos::x86_64::aspace_attribute_table(), type,
                                                   out);
    }

    // x86 cannot disable reads or express execute-only mappings.
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

    // Allow user access through table entries; leaf permissions restrict it.
    uint64_t table_desc_user(arch_phys_addr_t frame)
    {
        return static_cast<uint64_t>(frame) | PTE_P | PTE_RW | PTE_US;
    }

    // Walk any leaf size. Leave *pa unchanged on failure.
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

    // Return a 4 KiB leaf or null. This editor cannot split large leaves.
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

    // Check whether the boot mapping maps this physical address to itself.
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

    // Present boot-root slots are shared kernel tables. Compare slot presence,
    // not descriptor equality: hardware can set accessed/dirty bits independently
    // in each root (Intel SDM Vol. 3, section 5.8).
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
            // The allocator ignores outputs it does not own, such as device pages.
            kickos_frame_free(out);
            table[i] = 0;
        }
    }

    // Recursion is bounded by the level count.
    enum arch_aspace_result map_into(uint64_t* table, int level, uintptr_t va, size_t pages,
                                    arch_phys_addr_t pa, uint64_t leaf, bool resident)
    {
        while (pages != 0)
        {
            size_t const idx = index_at(va, level);
            if (level == LEVEL_LEAF)
            {
                if ((table[idx] & PTE_P) != 0)
                {
                    // Invalidate between clearing and replacing the entry (section 5.10.4.4).
                    table[idx] = 0;
                    invalidate_page_if(va, resident);
                }
                table[idx] = leaf | (static_cast<uint64_t>(pa) & PTE_ADDR_MASK);
                // Invalidate fresh entries too; skipping requires proof that every earlier
                // clear of this slot was invalidated (section 5.10.4.3).
                invalidate_page_if(va, resident);
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
                kickos::aspace::zero_table(table_at(frame), PTES);
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
        return kickos::aspace::table_empty(table, PTES);
    }

    // Only slots absent from the boot root are available to userspace.
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
        // Check every slot in the range; user slots need not be contiguous.
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

        // Require execute-disable support to enforce non-executable mappings.
        // EFER.NXE must be set before PTE_XD is used.
        if (not nx_supported())
        {
            refuse("this part reports no execute-disable bit, which every leaf here carries");
        }
        // Existing valid descriptors have bit 63 clear while NXE is disabled.
        uint64_t const efer = read_msr(MSR_EFER);
        if ((efer & EFER_NXE) == 0)
        {
            write_msr(MSR_EFER, efer | EFER_NXE);
        }

        g_boot_root = table_at(static_cast<arch_phys_addr_t>(read_cr3() & PTE_ADDR_MASK));
        g_ram_hi = static_cast<arch_phys_addr_t>(ram_base)
                   + static_cast<arch_phys_addr_t>(ram_size);

        // Verify identity mapping before walking tables by physical address.
        if (not identity_maps(phys_of(&g_kwin_table[0][0])))
        {
            refuse("the adopted regime does not map this image at its own physical address");
        }
        // Misaligned table addresses would be truncated by the descriptor mask.
        if ((phys_of(&g_kwin_table[0][0]) & static_cast<arch_phys_addr_t>(GRANULE - 1)) != 0)
        {
            refuse("the window tables are not granule aligned where the loader put this image");
        }

        int const top = static_cast<int>(g_levels);

        // Reserve a free high-half root slot before any user root copies it.
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
        // Temporarily disable write protection to edit firmware tables.
        // Keep every entry in the new chain supervisor-only.
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
        // Sign-extend the high-half address.
        g_kwin_va = static_cast<uintptr_t>(kwin_slot) << shift_at(top);
        if (kwin_slot >= PTES / 2)
        {
            g_kwin_va |= ~((static_cast<uintptr_t>(1) << (shift_at(top) + INDEX_BITS)) - 1);
        }

        // Include the new window in the shared kernel slots.
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
        // Exclude slot 0 to keep null pointers unmapped.
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
        // Build child tables on demand under the shared root entry.
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
        // CPUID leaf 1, ECX bit 17 reports 12-bit PCID support.
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
        // INVPCID support is independent of PCID support: leaf 7, EBX bit 10.
        cpuid_at(7, 0, &a, &b, &c, &d);
        return (b & (1u << 10)) != 0;
    }

    // Backend-only table walk without user-range validation.
    arch_phys_addr_t aspace_frame_at_unchecked(struct arch_aspace* space, uintptr_t va)
    {
        if (space == nullptr)
        {
            return 0;
        }
        // Protect the table walk from concurrent rollback and table reclamation.
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
    unsigned const pa_bits = phys_addr_bits();
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
    // The supported physical range must cover the entire frame pool.
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
    // Copy kernel root entries; their child tables remain shared.
    for (size_t i = 0; i < PTES; i++)
    {
        table[i] = g_boot_root[i];
    }
    // Destroy invalidates the old root before its frame can be reused.
    g_residency.open(static_cast<uint64_t>(root) & PTE_ADDR_MASK);
    return reinterpret_cast<struct arch_aspace*>(table);
}

void arch_aspace_destroy(struct arch_aspace* space)
{
    if (space == nullptr)
    {
        return;
    }
    uint64_t* const table = root_of(space);
    // The boot tables belong to firmware, not the frame pool.
    if (table == g_boot_root)
    {
        return;
    }
    uint64_t const key = root_key(space);
    // Invalidate cached walks before freeing tables.
    invalidate_all();
    // Remove residency after invalidation and before reusing the root.
    g_residency.close(key);
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
    // Do not edit the boot root through the user mapping API.
    if (root_of(space) == g_boot_root)
    {
        return ARCH_ASPACE_EINVAL;
    }
    // Validate the whole range before making any edits.
    if (not phys_range_ok(pa, pages))
    {
        return ARCH_ASPACE_EINVAL;
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
    // Never-run spaces have no cached translations to invalidate.
    bool const resident = resident_anywhere(space);
    enum arch_aspace_result const rc =
        map_into(root_of(space), static_cast<int>(g_levels), va, pages, pa, leaf, resident);
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
    bool const resident = resident_anywhere(space);
    for (size_t i = 0; i < pages; i++)
    {
        uintptr_t const at = va + static_cast<uintptr_t>(i) * GRANULE;
        uint64_t* const entry = leaf_entry(root_of(space), at);
        *entry = 0;
        invalidate_page_if(at, resident);
    }
    // Empty tables are reclaimed by destroy.
    return ARCH_ASPACE_OK;
}

void arch_aspace_activate(struct arch_aspace* space)
{
    if (space == nullptr)
    {
        return;
    }
    // All roots share kernel mappings. Reloading CR3 invalidates non-global
    // translations and paging-structure caches (section 5.10.4.1).
    // Mask interrupts while updating CR3 and its residency record.
    arch_irq_state_t const s = arch_irq_save();
    install_root(static_cast<uint64_t>(phys_of(root_of(space))));
    arch_irq_restore(s);
}

struct arch_aspace* arch_aspace_boot(void)
{
    return reinterpret_cast<struct arch_aspace*>(g_boot_root);
}

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

// Validate against user slots before walking the mapping.
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
    // The low byte is zero: direct acquisition has no window holds to mispair.
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
