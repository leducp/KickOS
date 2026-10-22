// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// x86-64 page tables (Intel SDM Vol. 3, chapter 5).
// Adopts the firmware root and its runtime paging depth; new spaces copy its kernel entries and
// share the child tables. PCIDs are off, so a CR3 write drops every non-global translation.

#include <kickos/arch/arch.h>
#include <kickos/arch/aspace.h>
#include <kickos/arch/aspace_residency.h>
#include <kickos/chip_com1.h>
#include <kickos/extent.h>

#include <stddef.h>
#include <stdint.h>

#include "sysops_x86_64.h"

extern "C"
{
    void kfault_terminate(void) __attribute__((noreturn));
    // The app window, where the loader put it (arch/x86/x86_64/pe_image.ld).
    extern unsigned char __kickos_app_rom_start[];
    extern unsigned char __kickos_app_sram_end[];
#if KICKOS_KERNEL_CORES > 1
    uint32_t kickos_x86_64_online_cores(void);
    void kickos_x86_64_translation_rendezvous(uint32_t peers);
#endif
}

namespace
{
    using namespace kickos::q35;

    constexpr unsigned GRANULE_SHIFT = 12;
    constexpr size_t GRANULE = static_cast<size_t>(1) << GRANULE_SHIFT;
    constexpr size_t PTES = GRANULE / sizeof(uint64_t);
    constexpr unsigned INDEX_BITS = 9;
    // PS is defined only at levels 2 and 3, never at the root or level 4.
    constexpr int HEIGHT_LARGEST_LEAF = 2;

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
    constexpr uint64_t PTE_PAT_LARGE = 1ull << 12;
    constexpr uint64_t PTE_XD = 1ull << 63;
    constexpr uint64_t PTE_ADDR_MASK = 0x000ffffffffff000ull;

    constexpr uint64_t CR4_PGE = 1ull << 7;
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

    constexpr unsigned TAG_BITS_RECORDED = 0;
}

#include "aspace_walk.h"

namespace
{

    // The live regime, read once at aspace_init.
    unsigned g_levels = 0;
    uint64_t* g_boot_root = nullptr;
    // The top of the conventional run, which is the widest output address this port programs.
    arch_phys_addr_t g_ram_hi = 0;

    // The arena and the app window where aspace_init found the boot root mapping every byte at
    // its own address; empty where it did not. Nothing edits the boot root after aspace_init.
    struct IdentityRun
    {
        arch_phys_addr_t lo;
        arch_phys_addr_t hi;
    };
    IdentityRun g_identity_runs[2] = {};

    // The leaf bits ARCH_MAP_NOCACHE selects under the PAT, which nothing writes after boot;
    // all ones where the PAT cannot encode it.
    uint64_t g_nocache_bits = ~0ull;
    uintptr_t g_user_lo = 0;
    uintptr_t g_user_hi = 0;

    // Root-keyed residency, updated by install_root and retained after switching away.
    kickos::aspace::Residency<KICKOS_NUM_CORES> g_residency;

    // CPUID 0x80000001, EDX bit 20. The extended leaf need not exist at all, so its own
    // maximum is read first.
    bool nx_supported(void)
    {
        uint32_t a = 0;
        uint32_t b = 0;
        uint32_t c = 0;
        uint32_t d = 0;
        kickos_x86_64_cpuid(0x80000000u, 0, &a, &b, &c, &d);
        if (a < 0x80000001u)
        {
            return false;
        }
        kickos_x86_64_cpuid(0x80000001u, 0, &a, &b, &c, &d);
        return (d & (1u << 20)) != 0;
    }

    // MAXPHYADDR capped at 52 bits, 36 where the leaf is absent (Intel SDM Vol 3, section 5.1.4).
    unsigned phys_addr_bits(void)
    {
        uint32_t a = 0;
        uint32_t b = 0;
        uint32_t c = 0;
        uint32_t d = 0;
        kickos_x86_64_cpuid(0x80000000u, 0, &a, &b, &c, &d);
        unsigned bits = 36;
        if (a >= 0x80000008u)
        {
            kickos_x86_64_cpuid(0x80000008u, 0, &a, &b, &c, &d);
            bits = static_cast<unsigned>(a & 0xFFu);
        }
        if (bits > 52)
        {
            bits = 52;
        }
        return bits;
    }

    // The whole range must fit MAXPHYADDR and the 52-bit descriptor: a truncated address would
    // land in low memory.
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

    bool pat_supported(void)
    {
        uint32_t a = 0;
        uint32_t b = 0;
        uint32_t c = 0;
        uint32_t d = 0;
        kickos_x86_64_cpuid(1, 0, &a, &b, &c, &d);
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

    arch_phys_addr_t entry_pa(uint64_t pte)
    {
        return static_cast<arch_phys_addr_t>(pte & PTE_ADDR_MASK);
    }

    int walk_top()
    {
        return static_cast<int>(g_levels) - 1;
    }

    __attribute__((always_inline)) inline bool is_leaf(uint64_t desc, int height)
    {
        if (height == 0)
        {
            return true;
        }
        if (height > HEIGHT_LARGEST_LEAF)
        {
            return false;
        }
        return (desc & PTE_PS) != 0;
    }

    bool entry_table(uint64_t desc, int height)
    {
        return (desc & PTE_P) != 0 and not is_leaf(desc, height);
    }

    // INVLPG also drops paging-structure caches, so a newly installed table entry needs no
    // other invalidation (Intel SDM Vol 3, section 5.10.4.1). This core only; peers_reload
    // covers the others.
    void invalidate_page(uintptr_t va)
    {
        kickos_x86_64_invlpg(va);
    }

    // Every other online core reloads CR3 before this returns, which with PCIDs off drops every
    // non-global translation it caches, and global pages are off on every core: one call covers
    // every entry edited before it. A frame an edit stopped mapping may go back to the pool only
    // after this.
    void peers_reload(void)
    {
#if KICKOS_KERNEL_CORES > 1
        kickos_x86_64_translation_rendezvous(kickos_x86_64_online_cores()
                                             & ~(1u << arch_cpu_id()));
#endif
    }

    void install_root(uint64_t cr3)
    {
        kickos_x86_64_write_cr3(cr3);
        g_residency.note(cr3 & PTE_ADDR_MASK, arch_cpu_id());
    }

    // No global entries exist, so a CR3 reload drops everything (section 5.10.4.1).
    void invalidate_all(void)
    {
        install_root(kickos_x86_64_read_cr3());
        peers_reload();
    }

    uint64_t root_key(struct arch_aspace* space)
    {
        return static_cast<uint64_t>(phys_of(root_of(space))) & PTE_ADDR_MASK;
    }

    // Includes spaces switched away from. Exact only because install_root, like every caller
    // of this, runs under the kernel lock.
    bool resident_anywhere(struct arch_aspace* space)
    {
        return g_residency.cores(root_key(space)) != 0;
    }

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

    // Walk any leaf size. Leave *pa unchanged on failure. A non-null `type` receives the
    // leaf's memory-type bits in their 4 KiB positions.
    __attribute__((always_inline)) inline bool walk(uint64_t const* root, uintptr_t va,
                                                    arch_phys_addr_t* pa, uint64_t* type,
                                                    uintptr_t* span = nullptr)
    {
        uint64_t const* table = root;
        for (int height = walk_top(); height >= 0; height--)
        {
            uint64_t const desc = table[index_at(va, height)];
            if ((desc & PTE_P) == 0)
            {
                return false;
            }
            if (is_leaf(desc, height))
            {
                // A large leaf's address field has its low bits reserved to zero, so the mask
                // gives the leaf's base and the offset inside it comes from `va`.
                uintptr_t const inside = va & (span_at(height) - 1);
                *pa = entry_pa(desc) + static_cast<arch_phys_addr_t>(inside & ~(GRANULE - 1));
                if (span != nullptr)
                {
                    *span = span_at(height);
                }
                if (type != nullptr)
                {
                    uint64_t bits = desc & (PTE_PWT | PTE_PCD);
                    if (height == 0)
                    {
                        bits |= desc & PTE_PAT_4K;
                    }
                    else if ((desc & PTE_PAT_LARGE) != 0)
                    {
                        bits |= PTE_PAT_4K;
                    }
                    *type = bits;
                }
                return true;
            }
            table = table_at(entry_pa(desc));
        }
        return false;
    }

    bool resolve(uint64_t const* root, uintptr_t va, arch_phys_addr_t* pa)
    {
        return walk(root, va, pa, nullptr);
    }

    bool resolve_typed(uint64_t const* root, uintptr_t va, arch_phys_addr_t* pa, uint64_t* type)
    {
        return walk(root, va, pa, type);
    }

    bool identity_maps(arch_phys_addr_t pa)
    {
        for (IdentityRun const& r : g_identity_runs)
        {
            if (pa >= r.lo and pa < r.hi)
            {
                return true;
            }
        }
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

    // A leaf whose first byte the boot root maps at its own address maps all of it so.
    IdentityRun identity_run(arch_phys_addr_t lo, arch_phys_addr_t hi)
    {
        arch_phys_addr_t at = lo & ~static_cast<arch_phys_addr_t>(GRANULE - 1);
        while (at < hi)
        {
            arch_phys_addr_t back = 0;
            uintptr_t span = 0;
            if (not walk(g_boot_root, static_cast<uintptr_t>(at), &back, nullptr, &span)
                or back != at)
            {
                return IdentityRun{};
            }
            at = (at & ~static_cast<arch_phys_addr_t>(span - 1u)) + span;
        }
        return IdentityRun{lo, hi};
    }

    // Slot presence, not descriptor equality: hardware sets accessed and dirty bits in each
    // root independently (Intel SDM Vol 3, section 5.8).
    bool root_slot_kept(uint64_t const*, size_t slot)
    {
        return (g_boot_root[slot] & PTE_P) != 0;
    }

    uint64_t leaf_desc(uint64_t attrs, arch_phys_addr_t pa)
    {
        return attrs | (static_cast<uint64_t>(pa) & PTE_ADDR_MASK);
    }

    void link_table(uint64_t* entry, arch_phys_addr_t table, Edit const&)
    {
        *entry = table_desc_user(table);
    }

    bool removal_owes(uint64_t)
    {
        return false;
    }

    // Only a CR3 reload drops a detached table's paging-structure entries walked for other
    // addresses (section 5.10.4.1).
    void edits_done(Edit const& e)
    {
        if (not e.resident)
        {
            return;
        }
        if (e.detached != 0)
        {
            invalidate_all();
            return;
        }
        peers_reload();
    }

    Edit edit_of(struct arch_aspace* space)
    {
        Edit const e = {resident_anywhere(space), 0, false, 0};
        return e;
    }

    // Whether an unprivileged access reaches any leaf under `table`. The user bit ANDs down
    // the walk, so a supervisor entry hides its whole subtree.
    bool user_reaches_a_leaf(uint64_t const* table, int height)
    {
        for (size_t i = 0; i < PTES; i++)
        {
            uint64_t const desc = table[i];
            if ((desc & PTE_P) == 0 or (desc & PTE_US) == 0)
            {
                continue;
            }
            if (is_leaf(desc, height))
            {
                return true;
            }
            if (user_reaches_a_leaf(table_at(entry_pa(desc)), height - 1))
            {
                return true;
            }
        }
        return false;
    }

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
        // User slots need not be contiguous, so every slot in the range is checked.
        int const top = walk_top();
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

    // Whether [pa, pa + bytes) moved up by the user offset lies wholly in user slots.
    bool alias_fits(uintptr_t pa, size_t bytes)
    {
        uintptr_t const lo = pa & ~static_cast<uintptr_t>(GRANULE - 1);
        uintptr_t end = 0;
        if (bytes == 0 or not kickos::extent_end(pa, bytes, 1, &end))
        {
            return bytes == 0;
        }
        size_t const pages = static_cast<size_t>((end - lo + GRANULE - 1) / GRANULE);
        return lo + g_user_lo >= lo and range_ok(lo + g_user_lo, pages);
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
        return kickos_x86_64_read_msr(MSR_PAT);
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
        uint64_t const cr4 = kickos_x86_64_read_cr4();
        // A CR3 reload keeps global translations, and the kernel half is the firmware's tables.
        if ((cr4 & CR4_PGE) != 0)
        {
            refuse("the firmware left global pages enabled, which a CR3 reload does not drop");
        }
        // ring3_init reads the same control-register bit; neither publishes it to the other.
        g_levels = 4;
        if ((cr4 & CR4_LA57) != 0)
        {
            g_levels = 5;
        }

        // EFER.NXE must be set before PTE_XD is used.
        if (not nx_supported())
        {
            refuse("this part reports no execute-disable bit, which every leaf here carries");
        }
        uint64_t const efer = kickos_x86_64_read_msr(MSR_EFER);
        if ((efer & EFER_NXE) == 0)
        {
            kickos_x86_64_write_msr(MSR_EFER, efer | EFER_NXE);
        }

        uint64_t nocache = 0;
        if (memtype_bits(ARCH_MAP_NOCACHE, &nocache))
        {
            g_nocache_bits = nocache;
        }

        g_boot_root =
            table_at(static_cast<arch_phys_addr_t>(kickos_x86_64_read_cr3() & PTE_ADDR_MASK));
        g_ram_hi = static_cast<arch_phys_addr_t>(ram_base)
                   + static_cast<arch_phys_addr_t>(ram_size);

        if (not identity_maps(phys_of(&g_boot_root)))
        {
            refuse("the adopted regime does not map this image at its own physical address");
        }
        if (user_reaches_a_leaf(g_boot_root, walk_top()))
        {
            refuse("the adopted map lets ring 3 reach a leaf of the kernel half");
        }

        int const top = walk_top();

        bool maps_any = false;
        for (size_t i = 0; i < PTES; i++)
        {
            if ((g_boot_root[i] & PTE_P) != 0)
            {
                maps_any = true;
                break;
            }
        }
        if (not maps_any)
        {
            refuse("the adopted root maps nothing, so there is no kernel half to share");
        }
        // Slot 0 stays unmapped so a null pointer faults.
        g_user_lo = 0;
        g_user_hi = 0;
        size_t const low_slots = PTES / 2;
        for (size_t i = 1; i < low_slots; i++)
        {
            if (not slot_is_user(i))
            {
                continue;
            }
            if (g_user_lo == 0)
            {
                g_user_lo = static_cast<uintptr_t>(i) * span_at(top);
            }
            g_user_hi = (static_cast<uintptr_t>(i) + 1) * span_at(top);
        }
        if (g_user_hi == 0)
        {
            refuse("the adopted root leaves no top-level slot for a space to map");
        }
        // A task reaches every frame of the arena, and the app window, at the physical address
        // plus the first user slot's base (arch_aspace_user_offset), so both have to land in
        // slots the firmware left absent.
        if (not alias_fits(ram_base, ram_size))
        {
            refuse("the arena moved up by the user offset leaves the slots a space may map");
        }
        uintptr_t const app_lo = reinterpret_cast<uintptr_t>(__kickos_app_rom_start);
        uintptr_t const app_hi = reinterpret_cast<uintptr_t>(__kickos_app_sram_end);
        if (app_hi > app_lo and not alias_fits(app_lo, app_hi - app_lo))
        {
            refuse("the app window moved up by the user offset leaves the slots a space may map");
        }
        aspace_identity_record(0, static_cast<arch_phys_addr_t>(ram_base), g_ram_hi);
        aspace_identity_record(1, static_cast<arch_phys_addr_t>(app_lo),
                               static_cast<arch_phys_addr_t>(app_hi));
        invalidate_all();
    }

    void aspace_identity_record(size_t run, arch_phys_addr_t lo, arch_phys_addr_t hi)
    {
        g_identity_runs[run] = identity_run(lo, hi);
    }

    bool aspace_identity_maps(arch_phys_addr_t pa)
    {
        return identity_maps(pa);
    }

    static arch_phys_addr_t aspace_frame_at_unchecked(struct arch_aspace* space, uintptr_t va)
    {
        if (space == nullptr)
        {
            return 0;
        }
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
    for (size_t i = 0; i < PTES; i++)
    {
        table[i] = g_boot_root[i];
    }
    // A recycled root frame needs no flush here: destroy invalidated it on every core first.
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
    // The boot tables are firmware's, not the frame pool's.
    if (table == g_boot_root)
    {
        return;
    }
    uint64_t const key = root_key(space);
    // Invalidate on every online core, close residency, then free: a freed table can be reused.
    invalidate_all();
    g_residency.close(key);
    free_subtree(table, walk_top(), true);
    kickos_frame_free(phys_of(table));
}

enum arch_aspace_result arch_aspace_map(struct arch_aspace* space, uintptr_t va,
                                        arch_phys_addr_t pa, size_t pages, uint32_t rights,
                                        enum arch_map_memtype type)
{
    if (space == nullptr or root_of(space) == g_boot_root or not range_ok(va, pages))
    {
        return ARCH_ASPACE_EINVAL;
    }
    if (not phys_range_ok(pa, pages))
    {
        return ARCH_ASPACE_EINVAL;
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
    if (space == nullptr or root_of(space) == g_boot_root or not range_ok(va, pages))
    {
        return ARCH_ASPACE_EINVAL;
    }
    return walk_unmap(root_of(space), va, pages, edit_of(space));
}

void arch_aspace_activate(struct arch_aspace* space)
{
    if (space == nullptr)
    {
        return;
    }
    arch_irq_state_t const s = arch_irq_save();
    install_root(static_cast<uint64_t>(phys_of(root_of(space))));
    arch_irq_restore(s);
}

// The firmware identity map is kernel half, so a frame is reached one whole user slot above
// its physical address: the first slot the adopted root leaves empty.
uintptr_t arch_aspace_user_offset(void)
{
    return g_user_lo;
}

// Spawn windows sit halfway up the user slot, far above g_user_lo plus every physical frame
// and the app window, and 1 GiB of it is theirs.
void arch_aspace_window_area(uintptr_t* base, size_t* size)
{
    *base = g_user_lo + (g_user_hi - g_user_lo) / 2u;
    *size = static_cast<size_t>(1) << 30;
}

struct arch_aspace* arch_aspace_boot(void)
{
    return reinterpret_cast<struct arch_aspace*>(g_boot_root);
}

void* arch_aspace_acquire(struct arch_aspace* space, uintptr_t va, bool* uncached,
                          arch_phys_addr_t* frame_out)
{
    if (frame_out != nullptr)
    {
        *frame_out = 0;
    }
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
    uint64_t type = 0;
    bool const mapped = resolve_typed(root_of(space),
                                      va & ~static_cast<uintptr_t>(GRANULE - 1), &frame, &type);
    bool reachable = false;
    if (mapped)
    {
        reachable = identity_maps(frame);
        if (frame_out != nullptr)
        {
            *frame_out = frame;
        }
    }
    arch_irq_restore(s);
    if (not reachable)
    {
        return nullptr;
    }
    if (uncached != nullptr)
    {
        *uncached = type == g_nocache_bits;
    }
    return reinterpret_cast<void*>(static_cast<uintptr_t>(frame) + off);
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
    uintptr_t const page = va & ~static_cast<uintptr_t>(GRANULE - 1);
    if (not range_ok(page, 1))
    {
        return 0;
    }
    return kickos::x86_64::aspace_frame_at_unchecked(space, page);
}

}
