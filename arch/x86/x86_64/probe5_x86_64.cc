// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// SCAFFOLDING: the aspace family on a single root register, held against a booting image.
// Every arm reports at the privileged level, and there is no thread here at all.

#include <kickos/arch/apic.h>
#include <kickos/arch/arch.h>
#include <kickos/arch/aspace.h>
#include <kickos/arch/desc.h>
#include <kickos/arch/regs.h>
#include <kickos/arch/trap.h>
#include <kickos/chip_q35.h>

#include <stddef.h>
#include <stdint.h>

// tools/run-qemu-x86_64-x5.sh carries this string too; move both or the arm fails.
#define KICKOS_X5_TOKEN "KICKOS-X5 6b1e04c7 x86_64/q35 aspace"

// HIDDEN keeps the reference PC-relative (tools/check-x86_64-no-got.sh), and an attribute binds
// only on a symbol's FIRST declaration.
#define KICKOS_X5_LOCAL __attribute__((visibility("hidden")))

extern "C" KICKOS_X5_LOCAL uint64_t kickos_x86_64_probe5_load(uintptr_t addr);

namespace
{
    using namespace kickos::x86_64;

    // probe5_x86_64.S asserts this encoding, so a stub that changed length breaks the build
    // rather than resuming into the middle of a return.
    constexpr unsigned load_stub_bytes = 3;

    constexpr size_t granule = 4096;

    // Carved out of the conventional run firmware named, so a frame's bytes are at its own
    // address, which is the property acquire checks.
    constexpr size_t pool_frames = 256;

    arch_phys_addr_t g_pool_base = 0;
    uint8_t g_pool_used[pool_frames] = {};
    unsigned g_outstanding = 0;
    unsigned g_allocs = 0;
    // 0 never fails. Any other value refuses the Nth allocation and every one after it, which is
    // what makes the out-of-frames refusal and its unwind reachable.
    unsigned g_fail_from = 0;

    bool g_failed = false;

    // Results seen across every call in this image.
    unsigned g_seen[4] = {0, 0, 0, 0};

    // The resumable fault. `g_skip_bytes` is 0 everywhere except across one deliberate access.
    volatile unsigned g_skip_bytes = 0;
    volatile unsigned g_faults = 0;
    volatile uint64_t g_fault_vector = 0;
    volatile uint64_t g_fault_addr = 0;

    uintptr_t g_va = 0;
    uintptr_t g_va_unmapped = 0;
    uintptr_t g_span_va = 0;
    // Conventional memory just past the pool: identity mapped like the pool, and NOT a frame the
    // pool ever handed out, so a destroy that walks over it hands the allocator an address it
    // does not own and the allocator refuses it.
    arch_phys_addr_t g_span_pa = 0;

    struct arch_aspace* g_space_a = nullptr;
    struct arch_aspace* g_space_b = nullptr;
    struct arch_aspace* g_boot = nullptr;

    arch_phys_addr_t g_frame_a = 0;
    arch_phys_addr_t g_frame_b = 0;
    arch_phys_addr_t g_frame_z = 0;
    arch_phys_addr_t g_frame_w = 0;

    constexpr uint64_t magic_a = 0xA5A50F0F5A5AF0F0ull;
    constexpr uint64_t magic_b = 0x5A5AF0F0A5A50F0Full;
    constexpr uint64_t magic_z = 0x1122334455667788ull;
    constexpr uint64_t magic_w = 0x8877665544332211ull;

    arch_phys_addr_t g_root_boot = 0;
    arch_phys_addr_t g_root_a = 0;
    arch_phys_addr_t g_root_b = 0;

    uintptr_t g_image_probe = 0;
    uintptr_t g_ram_base = 0;
    size_t g_ram_size = 0;

    // --- printing ----------------------------------------------------------
    size_t slen(char const* s)
    {
        size_t n = 0;
        while (s[n] != '\0')
        {
            n++;
        }
        return n;
    }

    void put(char const* s)
    {
        arch_console_write(s, slen(s));
    }

    // Always eighteen characters, which is what lets the run script hold a report line to a
    // fixed-width pattern.
    void put_hex(uint64_t v)
    {
        char buf[19];
        buf[0] = '0';
        buf[1] = 'x';
        for (unsigned i = 0; i < 16; i++)
        {
            unsigned const nib = static_cast<unsigned>((v >> (60 - 4 * i)) & 0xFu);
            if (nib < 10)
            {
                buf[2 + i] = static_cast<char>('0' + nib);
            }
            else
            {
                buf[2 + i] = static_cast<char>('a' + (nib - 10));
            }
        }
        buf[18] = '\0';
        put(buf);
    }

    void put_dec(uint64_t v)
    {
        char buf[21];
        unsigned n = 0;
        if (v == 0)
        {
            put("0");
            return;
        }
        while (v != 0)
        {
            buf[n] = static_cast<char>('0' + (v % 10));
            v /= 10;
            n++;
        }
        char out[21];
        for (unsigned i = 0; i < n; i++)
        {
            out[i] = buf[n - 1 - i];
        }
        out[n] = '\0';
        put(out);
    }

    void arm(char const* name, bool ok)
    {
        put("  " KICKOS_X5_TOKEN " arm=");
        put(name);
        if (ok)
        {
            put(" ok=1\n");
            return;
        }
        put(" ok=0\n");
        g_failed = true;
    }

    // --- the frame pool ----------------------------------------------------
    void pool_init(void)
    {
        void* const p = arch_ram_alloc((pool_frames + 1) * granule);
        if (p == nullptr)
        {
            return;
        }
        uintptr_t const base = (reinterpret_cast<uintptr_t>(p) + (granule - 1))
                               & ~static_cast<uintptr_t>(granule - 1);
        g_pool_base = static_cast<arch_phys_addr_t>(base);
    }

    // --- helpers -----------------------------------------------------------
    //
    // Entry bits this file reads back off a leaf the map path composed.
    constexpr uint64_t pte_p = 1ull << 0;
    constexpr uint64_t pte_pwt = 1ull << 3;
    constexpr uint64_t pte_pcd = 1ull << 4;
    constexpr uint64_t pte_a = 1ull << 5;
    constexpr uint64_t pte_pat_4k = 1ull << 7;
    constexpr uint64_t pte_addr_mask = 0x000ffffffffff000ull;

    constexpr uint32_t msr_pat = 0x277;

    // SYNTHETIC attribute tables, handed to the decode as an argument. Nothing here writes
    // IA32_PAT, which would retype whatever the adopted regime already maps through the fields
    // it moves. Each field below is one of the encodings Table 14-10 allows.
    //
    // The power-up layout (Table 14-12): write-back at field 0, write-through 1, UC- 2, UC 3.
    constexpr uint64_t pat_power_up = 0x0007040600070406ull;
    // Write-back at field 4, UC- at 5 and UC at 6, so all three answers need the PAT bit.
    constexpr uint64_t pat_high_half = 0x0400070601040501ull;
    // Write-back at fields 1 and 5, which is where the first-match rule shows.
    constexpr uint64_t pat_twice = 0x0704060007040600ull;
    // No field encodes write-back.
    constexpr uint64_t pat_no_wb = 0x0401000705040100ull;
    // No field encodes UC-.
    constexpr uint64_t pat_no_uc_minus = 0x0406000406000406ull;
    // No field encodes UC.
    constexpr uint64_t pat_no_uc = 0x0406070406070406ull;

    // The leaf bits `type` composes under `pat`, or a value no leaf carries where the decode
    // refused. Bit 63 is execute-disable in a real entry, so it cannot collide with an answer.
    constexpr uint64_t memtype_refused = 1ull << 63;

    uint64_t memtype_under(uint64_t pat, enum arch_map_memtype type)
    {
        uint64_t bits = 0;
        if (not aspace_memtype_bits(pat, type, &bits))
        {
            return memtype_refused;
        }
        return bits;
    }

    void cpuid_here(uint32_t leaf, uint32_t* a, uint32_t* b, uint32_t* c, uint32_t* d)
    {
        __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
    }

    // One past the widest physical address a paging-structure entry may name, asked of the PART:
    // bits 51:MAXPHYADDR are reserved in every entry.
    arch_phys_addr_t phys_limit_here(void)
    {
        uint32_t a = 0;
        uint32_t b = 0;
        uint32_t c = 0;
        uint32_t d = 0;
        cpuid_here(0x80000000u, &a, &b, &c, &d);
        unsigned bits = 36;
        if (a >= 0x80000008u)
        {
            cpuid_here(0x80000008u, &a, &b, &c, &d);
            bits = static_cast<unsigned>(a & 0xFFu);
        }
        if (bits > 52)
        {
            bits = 52;
        }
        return static_cast<arch_phys_addr_t>(1) << bits;
    }

    // The attribute-table field a granule leaf selects, and the type that field encodes.
    unsigned leaf_pat_index(uint64_t desc)
    {
        unsigned index = 0;
        if ((desc & pte_pwt) != 0)
        {
            index |= 1u;
        }
        if ((desc & pte_pcd) != 0)
        {
            index |= 2u;
        }
        if ((desc & pte_pat_4k) != 0)
        {
            index |= 4u;
        }
        return index;
    }

    uint8_t pat_field(uint64_t pat, unsigned index)
    {
        return static_cast<uint8_t>((pat >> (index * 8u)) & 0x7ull);
    }

    void tally(enum arch_aspace_result rc)
    {
        g_seen[static_cast<unsigned>(rc) & 3u]++;
    }

    enum arch_aspace_result do_map(struct arch_aspace* s, uintptr_t va, arch_phys_addr_t pa,
                                   size_t pages, uint32_t rights, enum arch_map_memtype type)
    {
        enum arch_aspace_result const rc = arch_aspace_map(s, va, pa, pages, rights, type);
        tally(rc);
        return rc;
    }

    enum arch_aspace_result do_unmap(struct arch_aspace* s, uintptr_t va, size_t pages)
    {
        enum arch_aspace_result const rc = arch_aspace_unmap(s, va, pages);
        tally(rc);
        return rc;
    }

    // The write goes through the frame's own address. Every magic value below is planted this
    // way and read back through a per-space mapping.
    void plant(arch_phys_addr_t frame, uint64_t value)
    {
        *reinterpret_cast<volatile uint64_t*>(static_cast<uintptr_t>(frame)) = value;
    }

    uint64_t read_at(uintptr_t va)
    {
        return *reinterpret_cast<volatile uint64_t const*>(va);
    }

    // One deliberate access at an address the installed root does not map. Returns true when it
    // faulted, having been resumed past the load.
    bool faults_at(uintptr_t va)
    {
        unsigned const before = g_faults;
        g_fault_vector = 0;
        g_fault_addr = 0;
        g_skip_bytes = load_stub_bytes;
        (void)kickos_x86_64_probe5_load(va);
        g_skip_bytes = 0;
        return g_faults == before + 1;
    }

    uint32_t model_field(uint64_t model, unsigned shift)
    {
        return static_cast<uint32_t>((model >> shift) & ARCH_ASPACE_MODEL_FIELD_MASK);
    }

    // The type the leaf a map of `type` composed actually names, read out of the LIVE attribute
    // table.
    bool leaf_names_type(uintptr_t va, enum arch_map_memtype type, uint8_t want)
    {
        if (do_map(g_space_a, va, g_frame_a, 1, ARCH_MAP_R, type) != ARCH_ASPACE_OK)
        {
            return false;
        }
        uint64_t const desc = aspace_leaf_desc(g_space_a, va);
        bool const named = (desc & pte_p) != 0
                           and pat_field(read_msr(msr_pat), leaf_pat_index(desc)) == want;
        return do_unmap(g_space_a, va, 1) == ARCH_ASPACE_OK and named;
    }

    // The three attribute-table index bits a real leaf came out carrying, or `memtype_refused`
    // where the map path refused or left no leaf.
    uint64_t leaf_memtype_bits(uintptr_t va, enum arch_map_memtype type)
    {
        if (do_map(g_space_a, va, g_frame_a, 1, ARCH_MAP_R, type) != ARCH_ASPACE_OK)
        {
            return memtype_refused;
        }
        uint64_t const desc = aspace_leaf_desc(g_space_a, va);
        uint64_t bits = memtype_refused;
        if ((desc & pte_p) != 0)
        {
            bits = desc & (pte_pwt | pte_pcd | pte_pat_4k);
        }
        if (do_unmap(g_space_a, va, 1) != ARCH_ASPACE_OK)
        {
            return memtype_refused;
        }
        return bits;
    }

    // --- arms --------------------------------------------------------------
    void arm_shape(void)
    {
        uint64_t const model = arch_aspace_model();
        unsigned const levels = aspace_levels();
        put("  " KICKOS_X5_TOKEN " model=");
        put_hex(model);
        put(" levels=");
        put_dec(levels);
        put(" tag_bits=");
        put_dec(aspace_tag_bits());
        put(" tag_invalidate=");
        put_dec(static_cast<uint64_t>(aspace_tag_invalidate_present()));
        put("\n");

        arm("granule_is_4k", arch_aspace_granule() == granule);
        arm("levels_four_or_five", levels == 4 or levels == 5);
        // The control-register bit is read here too, not taken from the backend.
        unsigned expect = 4;
        if ((read_cr4() & (1ull << 12)) != 0)
        {
            expect = 5;
        }
        arm("levels_match_control_register", levels == expect);
        arm("model_granule_bore_out", (model & ARCH_ASPACE_MODEL_GRANULE) != 0);
        arm("model_physical_range_bore_out", (model & ARCH_ASPACE_MODEL_PA) != 0);
        arm("model_identifier_matches_record", (model & ARCH_ASPACE_MODEL_ASID) != 0);
        arm("model_physical_bits_reported",
            model_field(model, ARCH_ASPACE_MODEL_PA_SHIFT) != 0);
        // One granule and three mapping sizes.
        arm("model_one_granule_reported",
            model_field(model, ARCH_ASPACE_MODEL_GRAN_SHIFT) == 1);
        // A machine can offer the instruction that invalidates by identifier without offering
        // the identifier itself. Whichever way this one answers, the port records none.
        arm("model_identifier_width_is_the_record",
            model_field(model, ARCH_ASPACE_MODEL_ASID_SHIFT) == aspace_tag_bits());

        arm("memtype_normal", arch_aspace_memtype_support(ARCH_MAP_NORMAL));
        arm("memtype_nocache", arch_aspace_memtype_support(ARCH_MAP_NOCACHE));
        arm("memtype_device", arch_aspace_memtype_support(ARCH_MAP_DEVICE));
        arm("memtype_unknown_refused",
            not arch_aspace_memtype_support(static_cast<enum arch_map_memtype>(7)));
    }

    void arm_boot_space(void)
    {
        g_boot = arch_aspace_boot();
        g_root_boot = aspace_root_installed();
        put("  " KICKOS_X5_TOKEN " boot root=");
        put_hex(g_root_boot);
        put(" kernel_slots=");
        put_dec(aspace_kernel_slots());
        put(" user_lo=");
        put_hex(aspace_user_lo());
        put(" user_hi=");
        put_hex(aspace_user_hi());
        put("\n");

        arm("boot_space_answered", g_boot != nullptr);
        arm("boot_space_is_the_installed_root",
            static_cast<arch_phys_addr_t>(reinterpret_cast<uintptr_t>(g_boot)) == g_root_boot);
        arm("kernel_half_has_slots", aspace_kernel_slots() != 0);
        arm("user_half_measured", aspace_user_lo() != 0 and aspace_user_hi() > aspace_user_lo());
        // These ask aspace_frame_at_unchecked: every address below is in the adopted regime's
        // kernel half, which arch_aspace_frame_at answers 0 for by contract, and the claim here
        // is about what the boot space MAPS.
        uintptr_t const image_page = g_image_probe & ~static_cast<uintptr_t>(granule - 1);
        arm("boot_maps_this_image",
            aspace_frame_at_unchecked(g_boot, g_image_probe)
                    == static_cast<arch_phys_addr_t>(image_page)
                and arch_aspace_frame_at(g_boot, g_image_probe) == 0);
        arm("boot_maps_conventional_memory_identically",
            aspace_frame_at_unchecked(g_boot, g_ram_base)
                == static_cast<arch_phys_addr_t>(g_ram_base));
        // The boot space is built out of 2 MiB and 1 GiB leaves, so the walk has to handle one.
        arm("boot_walk_handles_a_large_leaf",
            aspace_frame_at_unchecked(g_boot, g_ram_base + 0x1234)
                == static_cast<arch_phys_addr_t>(g_ram_base + granule));
        arm("user_half_unmapped_in_boot", arch_aspace_frame_at(g_boot, g_va) == 0);

        // The boot root's top-level entries are what every other space copies, so an edit here
        // would silently join the kernel half of every space created afterwards.
        arm("map_into_boot_refused",
            do_map(g_boot, g_va, g_pool_base, 1, ARCH_MAP_R, ARCH_MAP_NORMAL)
                == ARCH_ASPACE_EINVAL);
        arm("unmap_from_boot_refused", do_unmap(g_boot, g_va, 1) == ARCH_ASPACE_EINVAL);
        unsigned const held = g_outstanding;
        arch_aspace_destroy(g_boot);
        arm("destroy_boot_is_a_no_op",
            g_outstanding == held
                and aspace_frame_at_unchecked(g_boot, g_image_probe) != 0);
        arch_aspace_destroy(nullptr);
        arm("destroy_null_is_a_no_op", g_outstanding == held);
    }

    void arm_create(void)
    {
        unsigned const before = g_outstanding;
        g_space_a = arch_aspace_create();
        arm("create_a", g_space_a != nullptr);
        arm("create_costs_one_frame", g_outstanding == before + 1);
        g_space_b = arch_aspace_create();
        arm("create_b", g_space_b != nullptr);
        arm("two_spaces_are_distinct", g_space_a != g_space_b);

        // The kernel half is present in both, the create having copied the boot root's
        // top-level entries.
        arm("kernel_half_in_a",
            aspace_frame_at_unchecked(g_space_a, g_image_probe)
                == aspace_frame_at_unchecked(g_boot, g_image_probe));
        arm("kernel_half_in_b",
            aspace_frame_at_unchecked(g_space_b, g_image_probe)
                == aspace_frame_at_unchecked(g_boot, g_image_probe));
        arm("conventional_memory_in_a",
            aspace_frame_at_unchecked(g_space_a, g_ram_base)
                == static_cast<arch_phys_addr_t>(g_ram_base));
        arm("user_half_empty_in_a", arch_aspace_frame_at(g_space_a, g_va) == 0);
        arm("user_half_empty_in_b", arch_aspace_frame_at(g_space_b, g_va) == 0);
    }

    // The kernel half is SHARED. Both spaces above were created BEFORE any of the edits below,
    // so a create copying the level under the top would leave them on their own snapshots.
    void arm_kernel_half_shared(void)
    {
        uintptr_t const kwin = aspace_kernel_window();
        put("  " KICKOS_X5_TOKEN " window va=");
        put_hex(kwin);
        put(" pages=");
        put_dec(aspace_kernel_window_pages());
        put(" first_child_entries=");
        put_dec(aspace_first_child_entries());
        put("\n");

        arm("kernel_window_anchored", kwin != 0 and aspace_kernel_window_pages() != 0);
        // The range this port took is the kernel half's, so no space may map into it. The
        // per-slot leg of the range test.
        arm("kernel_window_outside_the_user_half",
            kwin < aspace_user_lo() or kwin >= aspace_user_hi());
        arm("map_at_the_window_refused",
            do_map(g_space_a, kwin, g_frame_a, 1, ARCH_MAP_R, ARCH_MAP_NORMAL)
                == ARCH_ASPACE_EINVAL);
        // The window is in the kernel half, so these read the backend's own walk. The second
        // arm also asserts the seam member's refusal, which answers 0 for a different reason.
        arm("kernel_window_starts_unmapped", aspace_frame_at_unchecked(g_boot, kwin) == 0);
        arm("kernel_window_unmapped_in_a",
            aspace_frame_at_unchecked(g_space_a, kwin) == 0
                and arch_aspace_frame_at(g_space_a, kwin) == 0
                and arch_aspace_acquire(g_space_a, kwin) == nullptr);

        plant(g_frame_z, magic_z);
        arm("kernel_window_map", aspace_kernel_map(0, g_frame_z));
        arm("kernel_edit_seen_from_boot", aspace_frame_at_unchecked(g_boot, kwin) == g_frame_z);
        // Without walking either space: nothing between the edit and these two reads touched a
        // table of A's or of B's.
        arm("kernel_edit_seen_from_a", aspace_frame_at_unchecked(g_space_a, kwin) == g_frame_z);
        arm("kernel_edit_seen_from_b", aspace_frame_at_unchecked(g_space_b, kwin) == g_frame_z);

        // And the same thing through the HARDWARE rather than through a walk of this port's own.
        arch_aspace_activate(g_space_a);
        arm("kernel_window_reads_under_a", read_at(kwin) == magic_z);
        arch_aspace_activate(g_space_b);
        arm("kernel_window_reads_under_b", read_at(kwin) == magic_z);

        // A second edit, made while A is installed, read back under B: a per-space copy would
        // still answer the first frame here.
        plant(g_frame_w, magic_w);
        arch_aspace_activate(g_space_a);
        arm("kernel_remap_under_a", aspace_kernel_map(0, g_frame_w));
        arm("kernel_remap_reads_under_a", read_at(kwin) == magic_w);
        arch_aspace_activate(g_space_b);
        arm("kernel_remap_reads_under_b", read_at(kwin) == magic_w);
        arm("kernel_remap_seen_from_boot_walk",
            aspace_frame_at_unchecked(g_boot, kwin) == g_frame_w);

        arch_aspace_activate(g_boot);
        arm("kernel_window_unmap", aspace_kernel_unmap(0));
        arm("kernel_unmap_seen_from_a", aspace_frame_at_unchecked(g_space_a, kwin) == 0);
        arm("kernel_unmap_seen_from_b", aspace_frame_at_unchecked(g_space_b, kwin) == 0);
        arm("kernel_unmap_faults_under_boot", faults_at(kwin));
        arm("kernel_unmap_fault_was_a_translation_fault", g_fault_vector == 14);
        arm("kernel_unmap_fault_named_the_window", g_fault_addr == kwin);
        arm("kernel_window_unmap_twice_refused", not aspace_kernel_unmap(0));
    }

    // Requirement: the root is genuinely REPLACED while executing. Three distinct values are read
    // back out of the register, and one virtual address answers three different ways under them.
    void arm_root_switch(void)
    {
        plant(g_frame_a, magic_a);
        plant(g_frame_b, magic_b);
        arm("map_a", do_map(g_space_a, g_va, g_frame_a, 1, ARCH_MAP_R | ARCH_MAP_W,
                            ARCH_MAP_NORMAL)
                         == ARCH_ASPACE_OK);
        arm("map_b", do_map(g_space_b, g_va, g_frame_b, 1, ARCH_MAP_R | ARCH_MAP_W,
                            ARCH_MAP_NORMAL)
                         == ARCH_ASPACE_OK);

        // The code performing the switch has to be mapped at the same address on both sides of
        // it, asked of the walk.
        uintptr_t const here = reinterpret_cast<uintptr_t>(&g_va);
        arch_phys_addr_t const here_boot = aspace_frame_at_unchecked(g_boot, here);
        arm("switch_code_data_mapped_under_a",
            here_boot != 0 and aspace_frame_at_unchecked(g_space_a, here) == here_boot);
        arm("switch_code_data_mapped_under_b",
            here_boot != 0 and aspace_frame_at_unchecked(g_space_b, here) == here_boot);

        arch_aspace_activate(g_space_a);
        g_root_a = aspace_root_installed();
        arch_aspace_activate(g_space_b);
        g_root_b = aspace_root_installed();
        arch_aspace_activate(g_boot);
        arch_phys_addr_t const back = aspace_root_installed();

        put("  " KICKOS_X5_TOKEN " roots boot=");
        put_hex(g_root_boot);
        put(" a=");
        put_hex(g_root_a);
        put(" b=");
        put_hex(g_root_b);
        put("\n");

        arm("activate_a_wrote_the_root", g_root_a != 0 and g_root_a != g_root_boot);
        arm("activate_b_wrote_the_root", g_root_b != 0 and g_root_b != g_root_a);
        arm("activate_boot_restored_the_root", back == g_root_boot);
        arm("root_a_is_the_handle",
            g_root_a == static_cast<arch_phys_addr_t>(reinterpret_cast<uintptr_t>(g_space_a)));
        arm("root_b_is_the_handle",
            g_root_b == static_cast<arch_phys_addr_t>(reinterpret_cast<uintptr_t>(g_space_b)));
        // The tag field in the low bits of the root register is zero in every value read back.
        arm("no_identifier_in_any_root",
            (g_root_a & 0xFFFull) == 0 and (g_root_b & 0xFFFull) == 0
                and (g_root_boot & 0xFFFull) == 0);

        // One address, three answers, and only the root register changed between them.
        arch_aspace_activate(g_space_a);
        arm("one_address_reads_a", read_at(g_va) == magic_a);
        arch_aspace_activate(g_space_b);
        arm("one_address_reads_b", read_at(g_va) == magic_b);
        arch_aspace_activate(g_boot);
        arm("one_address_faults_under_boot", faults_at(g_va));
        arm("boot_fault_was_a_translation_fault", g_fault_vector == 14);
        arm("boot_fault_named_the_address", g_fault_addr == g_va);
        // And the fault damaged nothing: the same address still answers A's frame.
        arch_aspace_activate(g_space_a);
        arm("one_address_reads_a_again", read_at(g_va) == magic_a);

        // A write through the mapping reaches the FRAME.
        *reinterpret_cast<volatile uint64_t*>(g_va) = magic_z;
        arm("write_through_a_reached_the_frame",
            *reinterpret_cast<volatile uint64_t*>(static_cast<uintptr_t>(g_frame_a)) == magic_z);
        plant(g_frame_a, magic_a);
        arch_aspace_activate(g_boot);
    }

    void arm_refusals(void)
    {
        arm("refuse_null_space",
            do_map(nullptr, g_va, g_frame_a, 1, ARCH_MAP_R, ARCH_MAP_NORMAL)
                == ARCH_ASPACE_EINVAL);
        arm("refuse_misaligned_va",
            do_map(g_space_a, g_va + 1, g_frame_a, 1, ARCH_MAP_R, ARCH_MAP_NORMAL)
                == ARCH_ASPACE_EINVAL);
        arm("refuse_zero_pages",
            do_map(g_space_a, g_va_unmapped, g_frame_a, 0, ARCH_MAP_R, ARCH_MAP_NORMAL)
                == ARCH_ASPACE_EINVAL);
        arm("refuse_misaligned_pa",
            do_map(g_space_a, g_va_unmapped, g_frame_a + 1, 1, ARCH_MAP_R, ARCH_MAP_NORMAL)
                == ARCH_ASPACE_EINVAL);
        arm("refuse_write_and_execute",
            do_map(g_space_a, g_va_unmapped, g_frame_a, 1, ARCH_MAP_R | ARCH_MAP_W | ARCH_MAP_X,
                   ARCH_MAP_NORMAL)
                == ARCH_ASPACE_EINVAL);
        // No read-disable and no execute-only form on this architecture, so a request without
        // read names permissions it cannot express.
        arm("refuse_no_read",
            do_map(g_space_a, g_va_unmapped, g_frame_a, 1, ARCH_MAP_W, ARCH_MAP_NORMAL)
                == ARCH_ASPACE_EINVAL);
        arm("refuse_unknown_right",
            do_map(g_space_a, g_va_unmapped, g_frame_a, 1, ARCH_MAP_R | 0x8u, ARCH_MAP_NORMAL)
                == ARCH_ASPACE_EINVAL);
        arm("refuse_unknown_memtype",
            do_map(g_space_a, g_va_unmapped, g_frame_a, 1, ARCH_MAP_R,
                   static_cast<enum arch_map_memtype>(7))
                == ARCH_ASPACE_EINVAL);
        // A range inside a top-level slot the boot root has: that slot is the kernel half, whose
        // tables every space shares, so an edit there is not this space's to make.
        arm("refuse_kernel_half_address",
            do_map(g_space_a, g_image_probe & ~static_cast<uintptr_t>(granule - 1), g_frame_a, 1,
                   ARCH_MAP_R, ARCH_MAP_NORMAL)
                == ARCH_ASPACE_EINVAL);
        // The WHOLE output run: bits 51:MAXPHYADDR are reserved in every paging-structure entry
        // and the address field is 52 bits wide, so a run walking off the top of either comes
        // back aliased onto a low frame.
        arm("refuse_pa_past_the_physical_width",
            do_map(g_space_a, g_va_unmapped, phys_limit_here(), 1, ARCH_MAP_R, ARCH_MAP_NORMAL)
                == ARCH_ASPACE_EINVAL);
        arm("refuse_pa_range_past_the_physical_width",
            do_map(g_space_a, g_va_unmapped, phys_limit_here() - granule, 2, ARCH_MAP_R,
                   ARCH_MAP_NORMAL)
                == ARCH_ASPACE_EINVAL);
        arm("kernel_window_map_past_the_physical_width_refused",
            not aspace_kernel_map(1, phys_limit_here()));
        arm("refuse_range_past_the_user_half",
            do_map(g_space_a, aspace_user_hi() - granule, g_frame_a, 2, ARCH_MAP_R,
                   ARCH_MAP_NORMAL)
                == ARCH_ASPACE_EINVAL);
        arm("refuse_page_count_that_wraps",
            do_map(g_space_a, g_va_unmapped, g_frame_a, SIZE_MAX, ARCH_MAP_R, ARCH_MAP_NORMAL)
                == ARCH_ASPACE_EINVAL);
        arm("nothing_was_mapped_by_a_refusal",
            arch_aspace_frame_at(g_space_a, g_va_unmapped) == 0);

        // Total or fail on the unmap side: a range not wholly mapped is refused and nothing in
        // it is cleared.
        arm("unmap_refuses_a_partial_range",
            do_unmap(g_space_a, g_va, 2) == ARCH_ASPACE_EINVAL);
        arm("the_partial_range_is_still_mapped",
            arch_aspace_frame_at(g_space_a, g_va) == g_frame_a);
        arm("unmap_refuses_an_unmapped_page",
            do_unmap(g_space_a, g_va_unmapped, 1) == ARCH_ASPACE_EINVAL);
    }

    void arm_memory_types(void)
    {
        // What the entry bits actually name, answered from the LIVE IA32_PAT: three index bits
        // select one of its eight fields and the field carries the type.
        arm("leaf_memtype_normal_is_write_back",
            leaf_names_type(g_va_unmapped, ARCH_MAP_NORMAL, 0x06));
        arm("leaf_memtype_nocache_is_uncached_minus",
            leaf_names_type(g_va_unmapped, ARCH_MAP_NOCACHE, 0x07));
        arm("leaf_memtype_device_is_uncacheable",
            leaf_names_type(g_va_unmapped, ARCH_MAP_DEVICE, 0x00));

        // The same three types over an inert table. Permuting the live one would retype
        // whatever the adopted regime already maps through the fields it moves, so the decode
        // takes its table as an argument.
        uint64_t const live = aspace_attribute_table();
        put("  " KICKOS_X5_TOKEN " pat live=");
        put_hex(live);
        put(" power_up=");
        put_hex(pat_power_up);
        put("\n");

        arm("memtype_decode_power_up_normal_selects_field_zero",
            memtype_under(pat_power_up, ARCH_MAP_NORMAL) == 0);
        arm("memtype_decode_power_up_nocache_selects_field_two",
            memtype_under(pat_power_up, ARCH_MAP_NOCACHE) == pte_pcd);
        arm("memtype_decode_power_up_device_selects_field_three",
            memtype_under(pat_power_up, ARCH_MAP_DEVICE) == (pte_pwt | pte_pcd));

        arm("memtype_decode_permuted_normal_selects_field_four",
            memtype_under(pat_high_half, ARCH_MAP_NORMAL) == pte_pat_4k);
        arm("memtype_decode_permuted_nocache_selects_field_five",
            memtype_under(pat_high_half, ARCH_MAP_NOCACHE) == (pte_pat_4k | pte_pwt));
        arm("memtype_decode_permuted_device_selects_field_six",
            memtype_under(pat_high_half, ARCH_MAP_DEVICE) == (pte_pat_4k | pte_pcd));

        arm("memtype_decode_takes_the_first_field_that_matches",
            memtype_under(pat_twice, ARCH_MAP_NORMAL) == pte_pwt);

        // A type no field of the running table encodes is REFUSED, which is what
        // arch_aspace_memtype_support answers.
        arm("memtype_decode_refuses_a_table_with_no_write_back",
            memtype_under(pat_no_wb, ARCH_MAP_NORMAL) == memtype_refused);
        arm("memtype_decode_answers_the_other_types_of_that_table",
            memtype_under(pat_no_wb, ARCH_MAP_NOCACHE) == pte_pat_4k
                and memtype_under(pat_no_wb, ARCH_MAP_DEVICE) == 0);
        arm("memtype_decode_refuses_a_table_with_no_uncached_minus",
            memtype_under(pat_no_uc_minus, ARCH_MAP_NOCACHE) == memtype_refused);
        arm("memtype_decode_refuses_a_table_with_no_uncacheable",
            memtype_under(pat_no_uc, ARCH_MAP_DEVICE) == memtype_refused);
        arm("memtype_decode_refuses_an_unknown_type",
            memtype_under(pat_power_up, static_cast<enum arch_map_memtype>(7)) == memtype_refused);

        // Ties the decode above to the map path: the bits a real leaf came out carrying are the
        // bits the decode answers for the table the backend says it is using. This firmware
        // leaves the power-up layout in place, so a backend handed that constant instead of the
        // register would compose the same leaf and both arms would stay green.
        arm("attribute_table_in_use_is_the_live_msr", live == read_msr(msr_pat));
        arm("leaf_bits_match_the_decode_on_the_live_table",
            leaf_memtype_bits(g_va_unmapped, ARCH_MAP_NORMAL) == memtype_under(live, ARCH_MAP_NORMAL)
                and leaf_memtype_bits(g_va_unmapped, ARCH_MAP_NOCACHE)
                        == memtype_under(live, ARCH_MAP_NOCACHE)
                and leaf_memtype_bits(g_va_unmapped, ARCH_MAP_DEVICE)
                        == memtype_under(live, ARCH_MAP_DEVICE));
    }

    void arm_span(void)
    {
        // Six hundred granules: one page, then a whole level-1 table's worth, then the remainder,
        // so the walk has to build a second table and the run has to cross it.
        constexpr size_t span_pages = 600;
        arch_phys_addr_t const span_pa = g_span_pa;
        arm("span_map",
            do_map(g_space_a, g_span_va, span_pa, span_pages, ARCH_MAP_R, ARCH_MAP_NORMAL)
                == ARCH_ASPACE_OK);
        arm("span_first_page", arch_aspace_frame_at(g_space_a, g_span_va) == span_pa);
        arm("span_page_at_the_table_boundary",
            arch_aspace_frame_at(g_space_a, g_span_va + 512 * granule)
                == span_pa + 512 * granule);
        arm("span_last_page",
            arch_aspace_frame_at(g_space_a, g_span_va + (span_pages - 1) * granule)
                == span_pa + (span_pages - 1) * granule);
        arm("span_page_before_is_unmapped",
            arch_aspace_frame_at(g_space_a, g_span_va - granule) == 0);
        arm("span_page_after_is_unmapped",
            arch_aspace_frame_at(g_space_a, g_span_va + span_pages * granule) == 0);
        arm("span_unmap", do_unmap(g_space_a, g_span_va, span_pages) == ARCH_ASPACE_OK);
        arm("span_first_page_gone", arch_aspace_frame_at(g_space_a, g_span_va) == 0);
        arm("span_last_page_gone",
            arch_aspace_frame_at(g_space_a, g_span_va + (span_pages - 1) * granule) == 0);
    }

    void arm_out_of_frames(void)
    {
        // In a space of its own, and the table cost is MEASURED: a hard-coded refusal point that
        // stopped landing on the last table would leave the unwind path unreached and green.
        constexpr size_t pages = 600;
        unsigned const held = g_outstanding;
        struct arch_aspace* c = arch_aspace_create();
        unsigned const before_tables = g_allocs;
        enum arch_aspace_result rc =
            do_map(c, g_span_va, g_span_pa, pages, ARCH_MAP_R, ARCH_MAP_NORMAL);
        unsigned const tables = g_allocs - before_tables;
        arm("cost_of_a_clean_map_measured", rc == ARCH_ASPACE_OK and tables >= 2);
        arch_aspace_destroy(c);
        arm("the_measuring_space_came_back", g_outstanding == held);
        put("  " KICKOS_X5_TOKEN " tables per span=");
        put_dec(tables);
        put("\n");

        // The FIRST table refused, so nothing was installed at all.
        c = arch_aspace_create();
        g_fail_from = g_allocs + 1;
        rc = do_map(c, g_span_va, g_span_pa, pages, ARCH_MAP_R, ARCH_MAP_NORMAL);
        g_fail_from = 0;
        arm("out_of_frames_on_the_first_table", rc == ARCH_ASPACE_ENOMEM);
        arm("first_table_refusal_installed_nothing",
            arch_aspace_frame_at(c, g_span_va) == 0);
        // The space still works afterwards, which is what "as it was" has to mean.
        arm("space_still_maps_after_a_refusal",
            do_map(c, g_span_va, g_span_pa, 1, ARCH_MAP_R, ARCH_MAP_NORMAL)
                == ARCH_ASPACE_OK);
        arm("and_the_mapping_answers", arch_aspace_frame_at(c, g_span_va) == g_span_pa);
        arm("cleanup_unmap", do_unmap(c, g_span_va, 1) == ARCH_ASPACE_OK);
        arch_aspace_destroy(c);
        arm("first_table_refusal_leaked_no_frame", g_outstanding == held);

        // The LAST table refused, so leaves were installed and the unwind has to clear them and
        // prune the tables it allocated.
        c = arch_aspace_create();
        g_fail_from = g_allocs + tables;
        rc = do_map(c, g_span_va, g_span_pa, pages, ARCH_MAP_R, ARCH_MAP_NORMAL);
        g_fail_from = 0;
        arm("out_of_frames_partway", rc == ARCH_ASPACE_ENOMEM);
        // TOTAL OR FAIL: on anything but OK the space is as it was, with no partial mapping left.
        bool clean = true;
        for (size_t i = 0; i < pages; i++)
        {
            if (arch_aspace_frame_at(c, g_span_va + i * granule) != 0)
            {
                clean = false;
                break;
            }
        }
        arm("partway_refusal_left_no_partial_mapping", clean);
        arch_aspace_destroy(c);
        arm("partway_refusal_leaked_no_frame", g_outstanding == held);
    }

    void arm_acquire(void)
    {
        void* const p = arch_aspace_acquire(g_space_a, g_va);
        arm("acquire_answers_the_frames_own_address",
            p == reinterpret_cast<void*>(static_cast<uintptr_t>(g_frame_a)));
        arm("acquire_reads_the_frame", p != nullptr and *static_cast<uint64_t*>(p) == magic_a);
        void* const q = arch_aspace_acquire(g_space_a, g_va + 7);
        arm("acquire_carries_the_offset",
            q == reinterpret_cast<void*>(static_cast<uintptr_t>(g_frame_a) + 7));
        arm("acquire_of_an_unmapped_page_is_null",
            arch_aspace_acquire(g_space_a, g_va_unmapped) == nullptr);
        arm("acquire_of_a_null_space_is_null", arch_aspace_acquire(nullptr, g_va) == nullptr);
        // All six of the seam's floor are live at once and none of them displaced another.
        void* held[ARCH_ASPACE_ACQUIRE_MIN];
        bool all = true;
        for (unsigned i = 0; i < ARCH_ASPACE_ACQUIRE_MIN; i++)
        {
            held[i] = arch_aspace_acquire(g_space_a, g_va);
            if (held[i] == nullptr)
            {
                all = false;
            }
        }
        arm("acquire_floor_is_met", all);
        for (unsigned i = 0; i < ARCH_ASPACE_ACQUIRE_MIN; i++)
        {
            arch_aspace_release(g_space_a, g_va);
        }
        arm("acquire_after_release_still_answers",
            arch_aspace_acquire(g_space_a, g_va) != nullptr);
        arch_aspace_release(g_space_a, g_va);
        arch_aspace_release(g_space_a, g_va + 7);
        arch_aspace_release(g_space_a, g_va);

        arm("frame_at_agrees_with_acquire",
            arch_aspace_frame_at(g_space_a, g_va) == static_cast<arch_phys_addr_t>(
                                                        reinterpret_cast<uintptr_t>(p)));
        arm("frame_at_drops_the_offset",
            arch_aspace_frame_at(g_space_a, g_va + 7) == g_frame_a);
        arm("frame_at_of_an_unmapped_page_is_zero",
            arch_aspace_frame_at(g_space_a, g_va_unmapped) == 0);
        arm("frame_at_of_a_null_space_is_zero", arch_aspace_frame_at(nullptr, g_va) == 0);
        // The frame the two spaces map at one address differs, which is the identity a caller
        // subtracting two acquire pointers would be after.
        arm("frame_at_separates_the_two_spaces",
            arch_aspace_frame_at(g_space_a, g_va) != arch_aspace_frame_at(g_space_b, g_va));
    }

    void arm_invalidation(void)
    {
#if !defined(KICKOS_ENABLE_SELFTEST)
        // The seam publishes the invalidation counts in a self-test build only, so this witness
        // has to be taken in one. Failing loudly here is the point: an image without them cannot
        // answer the arms below at all, and a silent skip would read as a pass.
        put("  " KICKOS_X5_TOKEN " FAIL the invalidation arms need a self-test build\n");
        g_failed = true;
        return;
#else
        // The issued count alone cannot tell an elision from an edit that never happened, which
        // is why the seam reports both halves. Elided is bits 31..8: the low byte is the
        // window-release mispairing count, which no backend with an addition for acquire has.
        arch_aspace_activate(g_space_a);
        uint64_t before = arch_aspace_tlbi_counts();
        (void)do_map(g_space_a, g_va_unmapped, g_frame_b, 1, ARCH_MAP_R, ARCH_MAP_NORMAL);
        uint64_t after = arch_aspace_tlbi_counts();
        uint32_t issued = static_cast<uint32_t>(after >> 32) - static_cast<uint32_t>(before >> 32);
        uint32_t elided = static_cast<uint32_t>((after >> 8) & 0xFFFFFFu)
                          - static_cast<uint32_t>((before >> 8) & 0xFFFFFFu);
        arm("fresh_map_issues_one", issued == 1 and elided == 0);

        before = after;
        (void)do_map(g_space_a, g_va_unmapped, g_frame_a, 1, ARCH_MAP_R, ARCH_MAP_NORMAL);
        after = arch_aspace_tlbi_counts();
        issued = static_cast<uint32_t>(after >> 32) - static_cast<uint32_t>(before >> 32);
        // BREAK BEFORE MAKE: one for the clear and one for the new entry, so the invalidate falls
        // BETWEEN the two writes rather than after them.
        arm("replacing_a_live_page_issues_two", issued == 2);
        arm("and_the_replacement_took",
            arch_aspace_frame_at(g_space_a, g_va_unmapped) == g_frame_a);

        before = after;
        (void)do_unmap(g_space_a, g_va_unmapped, 1);
        after = arch_aspace_tlbi_counts();
        issued = static_cast<uint32_t>(after >> 32) - static_cast<uint32_t>(before >> 32);
        arm("unmap_issues_one", issued == 1);

        // A space installed on no core has no cached translation and no cached absence, so its
        // seeding costs no maintenance at all.
        arch_aspace_activate(g_boot);
        before = arch_aspace_tlbi_counts();
        (void)do_map(g_space_a, g_va_unmapped, g_frame_b, 1, ARCH_MAP_R, ARCH_MAP_NORMAL);
        after = arch_aspace_tlbi_counts();
        issued = static_cast<uint32_t>(after >> 32) - static_cast<uint32_t>(before >> 32);
        elided = static_cast<uint32_t>((after >> 8) & 0xFFFFFFu)
                 - static_cast<uint32_t>((before >> 8) & 0xFFFFFFu);
        arm("map_into_a_space_this_core_is_not_on_elides", issued == 0 and elided == 1);
        (void)do_unmap(g_space_a, g_va_unmapped, 1);

        put("  " KICKOS_X5_TOKEN " tlbi issued=");
        put_dec(static_cast<uint32_t>(arch_aspace_tlbi_counts() >> 32));
        put(" elided=");
        put_dec(static_cast<uint32_t>((arch_aspace_tlbi_counts() >> 8) & 0xFFFFFFu));
        put("\n");
#endif
    }

    void arm_destroy(void)
    {
        // Both spaces still map the kernel window's tables through their copied top-level
        // entries, so a destroy that walked into one would free the tables the other still uses.
        plant(g_frame_z, magic_z);
        (void)aspace_kernel_map(0, g_frame_z);
        uintptr_t const kwin = aspace_kernel_window();

        arch_aspace_activate(g_boot);
        // THE BORROWER UNMAPS FIRST, which is the rule arch.h states for destroy: a space must
        // not still map a frame it does not own when destroy runs, or the pool is handed a frame
        // that is still somebody else's. The four frames the arms plant their values in are this
        // witness's, lent to the spaces, so both mappings go before either space does.
        arm("borrowed_page_unmapped_before_destroy",
            do_unmap(g_space_a, g_va, 1) == ARCH_ASPACE_OK);
        arm("the_lent_frame_survived_the_unmap",
            *reinterpret_cast<volatile uint64_t*>(static_cast<uintptr_t>(g_frame_a)) == magic_a);

        // The processor sets the accessed flag in every paging-structure entry a translation
        // walks, and a space's copy is walked independently of the boot root, so one shared
        // slot's two descriptors drift apart while both still name the same table. The slot is
        // SEARCHED for, so a firmware leaving none unwalked reddens the arm.
        uint64_t* const root_a = reinterpret_cast<uint64_t*>(g_space_a);
        uint64_t const* const root_boot = reinterpret_cast<uint64_t const*>(g_boot);
        size_t shared_slot = 0;
        uint64_t shared_desc = 0;
        uint64_t shared_child_first = 0;
        unsigned shared_child_present = 0;
        bool diverged = false;
        for (size_t i = 0; i < 512 and not diverged; i++)
        {
            if ((root_boot[i] & pte_p) == 0 or (root_boot[i] & pte_a) != 0)
            {
                continue;
            }
            if (root_boot[i] != root_a[i])
            {
                continue;
            }
            shared_slot = i;
            shared_desc = root_boot[i];
            uint64_t const* const child = reinterpret_cast<uint64_t const*>(
                static_cast<uintptr_t>(shared_desc & pte_addr_mask));
            shared_child_first = child[0];
            for (size_t k = 0; k < 512; k++)
            {
                if ((child[k] & pte_p) != 0)
                {
                    shared_child_present++;
                }
            }
            root_a[i] |= pte_a;
            diverged = true;
        }
        arm("a_shared_slot_diverged_by_an_accessed_bit",
            diverged and root_a[shared_slot] != root_boot[shared_slot]
                and (root_a[shared_slot] & pte_addr_mask)
                        == (root_boot[shared_slot] & pte_addr_mask));

        arch_aspace_destroy(g_space_a);
        {
            uint64_t const* const child = reinterpret_cast<uint64_t const*>(
                static_cast<uintptr_t>(shared_desc & pte_addr_mask));
            unsigned present = 0;
            for (size_t k = 0; k < 512; k++)
            {
                if ((child[k] & pte_p) != 0)
                {
                    present++;
                }
            }
            arm("the_diverged_slot_kept_its_table",
                root_boot[shared_slot] == shared_desc and child[0] == shared_child_first
                    and present == shared_child_present);
        }
        arm("kernel_half_survives_a_destroy",
            aspace_frame_at_unchecked(g_boot, g_image_probe) != 0
                and aspace_frame_at_unchecked(g_boot, g_ram_base)
                        == static_cast<arch_phys_addr_t>(g_ram_base));
        arm("the_shared_window_survives_a_destroy",
            aspace_frame_at_unchecked(g_space_b, kwin) == g_frame_z
                and read_at(kwin) == magic_z);
        arm("the_other_space_still_maps_its_own_page",
            arch_aspace_frame_at(g_space_b, g_va) == g_frame_b);
        arch_aspace_activate(g_space_b);
        arm("the_other_space_still_reads_its_own_page", read_at(g_va) == magic_b);

        arch_aspace_activate(g_boot);
        arm("second_borrowed_page_unmapped_before_destroy",
            do_unmap(g_space_b, g_va, 1) == ARCH_ASPACE_OK);
        arch_aspace_destroy(g_space_b);
        arm("kernel_half_survives_the_second_destroy",
            aspace_frame_at_unchecked(g_boot, g_image_probe) != 0);
        arm("the_window_survives_the_second_destroy",
            aspace_frame_at_unchecked(g_boot, kwin) == g_frame_z);
        (void)aspace_kernel_unmap(0);
        g_space_a = nullptr;
        g_space_b = nullptr;

        // AND WHAT A DESTROY DOES RECLAIM, which the two unmaps above deliberately kept out of
        // its way: a frame the pool DID hand out, mapped and left mapped, comes back with the
        // tables. Without this arm the pair above would read as "destroy frees nothing".
        unsigned const held = g_outstanding;
        arch_phys_addr_t const owned = kickos_frame_alloc();
        arm("a_frame_for_the_space_to_own", owned != 0 and g_outstanding == held + 1);
        struct arch_aspace* const c = arch_aspace_create();
        arm("mapped_and_left_mapped",
            do_map(c, g_va, owned, 1, ARCH_MAP_R | ARCH_MAP_W, ARCH_MAP_NORMAL)
                == ARCH_ASPACE_OK);
        arch_aspace_destroy(c);
        arm("destroy_reclaimed_the_frame_the_space_mapped", g_outstanding == held);
    }

    void arm_frames_returned(unsigned baseline)
    {
        put("  " KICKOS_X5_TOKEN " frames outstanding=");
        put_dec(g_outstanding);
        put(" baseline=");
        put_dec(baseline);
        put(" allocations=");
        put_dec(g_allocs);
        put("\n");
        // Every frame the two spaces held came back: their roots, every table under them and
        // nothing belonging to the kernel half.
        arm("every_frame_came_back", g_outstanding == baseline);
    }

    void arm_capacity_refusal(void)
    {
        put("  " KICKOS_X5_TOKEN " results ok=");
        put_dec(g_seen[ARCH_ASPACE_OK]);
        put(" enomem=");
        put_dec(g_seen[ARCH_ASPACE_ENOMEM]);
        put(" ecapacity=");
        put_dec(g_seen[ARCH_ASPACE_ECAPACITY]);
        put(" einval=");
        put_dec(g_seen[ARCH_ASPACE_EINVAL]);
        put("\n");
        arm("every_result_class_but_capacity_was_reached",
            g_seen[ARCH_ASPACE_OK] != 0 and g_seen[ARCH_ASPACE_ENOMEM] != 0
                and g_seen[ARCH_ASPACE_EINVAL] != 0);
        // A radix backend has no full bucket to evict from, so nothing here can produce the
        // capacity refusal; it stays in the seam for the first hashed-table backend.
        arm("capacity_refusal_is_unproducible", g_seen[ARCH_ASPACE_ECAPACITY] == 0);
    }
}

extern "C"
{

// --- the frame pool the backend calls ---------------------------------------
arch_phys_addr_t kickos_frame_alloc(void)
{
    if (g_pool_base == 0)
    {
        return 0;
    }
    g_allocs++;
    if (g_fail_from != 0 and g_allocs >= g_fail_from)
    {
        return 0;
    }
    for (size_t i = 0; i < pool_frames; i++)
    {
        if (g_pool_used[i] != 0)
        {
            continue;
        }
        g_pool_used[i] = 1;
        g_outstanding++;
        return g_pool_base + static_cast<arch_phys_addr_t>(i) * granule;
    }
    return 0;
}

// A frame this pool never handed out is REFUSED rather than accepted, which is what lets a space
// hold a page the pool does not own without a destroy reclaiming it.
void kickos_frame_free(arch_phys_addr_t frame)
{
    if (g_pool_base == 0 or frame < g_pool_base)
    {
        return;
    }
    arch_phys_addr_t const off = frame - g_pool_base;
    if ((off % granule) != 0)
    {
        return;
    }
    size_t const idx = static_cast<size_t>(off / granule);
    if (idx >= pool_frames or g_pool_used[idx] == 0)
    {
        return;
    }
    g_pool_used[idx] = 0;
    g_outstanding--;
}

// --- the kernel-side symbols the archives reference -------------------------
// No thread exists in this image, so containment answers no and every hook below declines. The
// one exception is the fault reporter, which is what makes a deliberate translation fault
// resumable.
bool kickos_fault_kill_thread(void* frame)
{
    kickos::x86_64::trap_frame* const f = static_cast<kickos::x86_64::trap_frame*>(frame);
    if (g_skip_bytes == 0)
    {
        return false;
    }
    uint64_t const cr2 = kickos::x86_64::read_cr2();
    g_faults = g_faults + 1;
    g_fault_vector = f->vector;
    g_fault_addr = cr2;
    f->rip = f->rip + g_skip_bytes;
    return true;
}

bool kickos_fault_frame_on_kernel_stack(void const* frame, size_t bytes)
{
    (void)frame;
    (void)bytes;
    return false;
}

uintptr_t kickos_fault_stack_top(void)
{
    return 0;
}

void kickos_fault_record(char const* status_name, uint64_t status, uintptr_t pc, uintptr_t addr,
                         int addr_valid)
{
    (void)status_name;
    (void)status;
    (void)pc;
    (void)addr;
    (void)addr_valid;
}

void kickos_thread_fault_exit(void)
{
    while (true)
    {
        __asm__ volatile("cli");
        __asm__ volatile("hlt");
    }
}

void kickos_user_thread_return(void)
{
    while (true)
    {
        __asm__ volatile("cli");
        __asm__ volatile("hlt");
    }
}

void kickos_thread_return(void)
{
    while (true)
    {
        __asm__ volatile("cli");
        __asm__ volatile("hlt");
    }
}

uint64_t syscall_dispatch(uintptr_t nr, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    (void)nr;
    (void)a0;
    (void)a1;
    (void)a2;
    (void)a3;
    return 0;
}

void kickos_isr_timer(void)
{
}

void kickos_isr_irq(int irq)
{
    (void)irq;
}

void kickos_x86_64_landed(uintptr_t ram_base, uint64_t ram_size)
{
    kickos::q35::ram_publish(ram_base, static_cast<size_t>(ram_size));
    arch_init();

    g_ram_base = ram_base;
    g_ram_size = static_cast<size_t>(ram_size);
    g_image_probe = reinterpret_cast<uintptr_t>(&kickos_x86_64_landed);

    put("\n" KICKOS_X5_TOKEN " arms\n");

    pool_init();
    if (g_pool_base == 0)
    {
        put(KICKOS_X5_TOKEN " FAIL no frame pool\n");
        arch_shutdown(1);
    }
    // Four frames off the top of the pool, planted with values the arms read back through a
    // mapping. Taken before anything else so a later refusal arm cannot move them.
    g_frame_a = kickos_frame_alloc();
    g_frame_b = kickos_frame_alloc();
    g_frame_z = kickos_frame_alloc();
    g_frame_w = kickos_frame_alloc();
    unsigned const baseline = g_outstanding;

    g_va = aspace_user_lo() + 0x10000;
    g_va_unmapped = g_va + 0x1000;
    g_span_va = g_va + 0x200000;
    g_span_pa = g_pool_base + pool_frames * granule;
    put("  " KICKOS_X5_TOKEN " pool base=");
    put_hex(g_pool_base);
    put(" frames=");
    put_dec(pool_frames);
    put(" va=");
    put_hex(g_va);
    put("\n");

    arm_shape();
    arm_boot_space();
    arm_create();
    arm_kernel_half_shared();
    arm_root_switch();
    arm_refusals();
    arm_memory_types();
    arm_span();
    arm_out_of_frames();
    arm_acquire();
    arm_invalidation();
    arm_destroy();
    arm_frames_returned(baseline);
    arm_capacity_refusal();

    // The register is back where firmware left it, which is the posture the image halts in.
    arm("root_register_ends_on_the_boot_space", aspace_root_installed() == g_root_boot);

    if (g_failed)
    {
        put(KICKOS_X5_TOKEN " FAIL\n");
        arch_shutdown(1);
    }
    put(KICKOS_X5_TOKEN " PASS\n");
    arch_shutdown(0);
}

}
