// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// SCAFFOLDING: the arms that witness ring 3, the fast syscall entry, the kernel stack that
// entry loads by hand, the flag mask it runs under, fault attribution and the two user-pointer
// oracles. Taken AT THE ARCH SEAM.
//
// Every arm reports at ring 0, the console being port I/O and port I/O being refused at ring 3.
// An unprivileged arm records into a global and the syscall trap carries it out.

#include <kickos/arch/apic.h>
#include <kickos/arch/arch.h>
#include <kickos/arch/aspace.h>
#include <kickos/arch/desc.h>
#include <kickos/arch/regs.h>
#include <kickos/arch/ring3.h>
#include <kickos/arch/trap.h>
#include <kickos/chip_q35.h>

#include <stddef.h>
#include <stdint.h>

// tools/run-qemu-x86_64-x4.sh carries this string too; move both or the arm fails.
#define KICKOS_X4_TOKEN "KICKOS-X4 4f2ba917 x86_64/q35 ring3"

// probe4_x86_64.S. HIDDEN keeps the reference PC-relative
// (tools/check-x86_64-no-got.sh).
#define KICKOS_X4_LOCAL __attribute__((visibility("hidden")))
extern "C" KICKOS_X4_LOCAL void kickos_x86_64_probe_sysret(uintptr_t rip, uintptr_t rsp,
                                                          uintptr_t nr);
extern "C" KICKOS_X4_LOCAL void kickos_x86_64_probe_sysret_stub(void);
extern "C" KICKOS_X4_LOCAL void kickos_x86_64_probe_cr0(void);
extern "C" KICKOS_X4_LOCAL void kickos_x86_64_probe_outb(void);
extern "C" KICKOS_X4_LOCAL void kickos_x86_64_probe_ud2(void);
extern "C" KICKOS_X4_LOCAL uint64_t kickos_x86_64_probe_flag_forge(uintptr_t nr,
                                                                  uint64_t forged);

namespace
{
    using kickos::x86_64::trap_frame;

    constexpr uint64_t ns_per_ms = 1000000;

    // The probe's own call numbers.
    constexpr uintptr_t nr_echo = 1;
    constexpr uintptr_t nr_block = 2;
    constexpr uintptr_t nr_exit = 3;
    constexpr uintptr_t nr_kfault = 4;
    constexpr uintptr_t nr_sysret = 5;
    constexpr uintptr_t nr_ring0 = 6;

    // Distinct in every nibble that matters, so an argument delivered in the wrong register or
    // truncated to 32 bits is a different number rather than a plausible one.
    constexpr uintptr_t echo_a0 = 0x1122334455667788ull;
    constexpr uintptr_t echo_a1 = 0x99aabbccddeeff00ull;
    constexpr uintptr_t echo_a2 = 0x0f1e2d3c4b5a6978ull;
    constexpr uintptr_t echo_a3 = 0x8796a5b4c3d2e1f0ull;
    constexpr uint64_t echo_result = 0xfeedface12345678ull;
    constexpr uint64_t block_result = 0x00000000abcdef01ull;
    constexpr uint64_t ring0_result = 0x0000000013572468ull;

    constexpr unsigned fault_class_cr0 = 0;
    constexpr unsigned fault_class_outb = 1;

    constexpr unsigned phase_main = 0;
    constexpr unsigned phase_user = 1;

    // One block per unprivileged context, sized as the knob says, standing in for the pool
    // kmain carves. Its top is what thread_create would have seated in ctx.kernel_sp.
    constexpr size_t kblock_bytes = KICKOS_KERNEL_STACK_SIZE;
    alignas(16) uint8_t g_kblock_a[kblock_bytes];
    alignas(16) uint8_t g_kblock_f[kblock_bytes];
    alignas(16) uint8_t g_kblock_s[kblock_bytes];

    alignas(16) uint8_t g_ustack_a[16384];
    alignas(16) uint8_t g_ustack_f[16384];
    alignas(16) uint8_t g_ustack_s[16384];
    // The privileged launcher's own stack. The SYSRET arm has to be entered through an
    // ordinary switch: a context that is currently RUNNING cannot be the target of one, and
    // resuming its stale frame a second time is a general protection fault on the iretq.
    alignas(16) uint8_t g_lstack[8192];

    struct arch_context g_main;
    struct arch_context g_ctx_a;
    struct arch_context g_ctx_f;
    struct arch_context g_ctx_s;

    // The running unprivileged context's block top. The probe's own stand-in for what
    // kickos_fault_frame_on_kernel_stack reads out of the current thread in a real image.
    volatile uintptr_t g_running_kernel_sp = 0;
    volatile unsigned g_phase = phase_main;

    bool g_failed = false;

    // What the dispatch observed about itself, which is where the hazard arms live.
    volatile uint32_t g_sys_calls = 0;
    volatile uint64_t g_sys_rsp = 0;
    volatile uint64_t g_sys_flags = 0;
    volatile uint16_t g_sys_cs = 0;
    volatile int g_sys_in_isr = -1;
    volatile uint64_t g_sys_tss_rsp0 = 0;
    volatile uint64_t g_sys_cpu_sp = 0;
    volatile bool g_sys_args_ok = false;

    // The unprivileged thread's own observations, carried out through the trap.
    volatile uint16_t g_user_cs = 0;
    volatile uint16_t g_user_ss = 0;
    volatile uint64_t g_user_flags = 0;
    volatile uint64_t g_echo64 = 0;
    volatile uintptr_t g_echo_low = 0;
    volatile uintptr_t g_block_ret = 0;
    volatile bool g_blocked = false;
    volatile bool g_resumed = false;
    volatile bool g_a_exited = false;
    volatile uint64_t g_spun = 0;
    volatile uint64_t g_forged_back = 0;

    volatile uint32_t g_timer_count = 0;
    volatile uint32_t g_preempts = 0;

    // Fault attribution.
    volatile unsigned g_fault_class = fault_class_cr0;
    volatile uint32_t g_faults = 0;
    volatile uint64_t g_fault_vector = 0;
    volatile uint64_t g_fault_cs = 0;
    volatile bool g_fault_attributed = false;
    volatile bool g_fault_on_block = false;
    volatile bool g_fault_body_continued = false;
    volatile uint16_t g_death_cs = 0;
    volatile uint64_t g_death_rsp = 0;
    volatile uint32_t g_deaths = 0;

    // The ring 0 controls.
    volatile uint32_t g_kfaults = 0;
    volatile bool g_kfault_attributed = true;
    volatile bool g_kfault_on_block = false;
    volatile uint64_t g_kfault_cs = 0;
    volatile unsigned g_skip_bytes = 0;
    volatile bool g_cr0_at_ring0_returned = false;

    // The SYSRET arm.
    volatile uint16_t g_sysret_cs = 0;
    volatile uint16_t g_sysret_ss = 0;
    volatile bool g_sysret_arrived = false;

    // Oracle subjects. One object per section under -fdata-sections, which is what makes the
    // read-only and writable arms different addresses rather than different offsets.
    char const g_ro_subject[16] = "x4-rodata-probe";
    uint64_t g_rw_subject = 0;

    void put(char const* s)
    {
        size_t n = 0;
        while (s[n] != '\0')
        {
            n++;
        }
        arch_console_write(s, n);
    }

    void put_hex(uint64_t v)
    {
        char buf[19];
        buf[0] = '0';
        buf[1] = 'x';
        for (int i = 0; i < 16; i++)
        {
            unsigned const nib = static_cast<unsigned>((v >> (60 - 4 * i)) & 0xfu);
            char c = static_cast<char>('0' + nib);
            if (nib >= 10)
            {
                c = static_cast<char>('a' + (nib - 10));
            }
            buf[2 + i] = c;
        }
        arch_console_write(buf, 18);
    }

    void put_dec(uint64_t v)
    {
        char buf[21];
        int n = 0;
        if (v == 0)
        {
            arch_console_write("0", 1);
            return;
        }
        while (v != 0)
        {
            buf[n] = static_cast<char>('0' + (v % 10));
            n++;
            v /= 10;
        }
        while (n > 0)
        {
            n--;
            arch_console_write(&buf[n], 1);
        }
    }

    // The step X3 line shape, so one reader and one script serve both steps.
    void arm2(char const* prefix, char const* name, bool ok)
    {
        put("  ");
        put(KICKOS_X4_TOKEN);
        put(" arm=");
        put(prefix);
        put(name);
        if (ok)
        {
            put(" ok=1\n");
            return;
        }
        put(" ok=0\n");
        g_failed = true;
    }

    void arm(char const* name, bool ok)
    {
        arm2("", name, ok);
    }

    uint64_t read_flags(void)
    {
        uint64_t flags = 0;
        __asm__ volatile("pushfq\n\tpop %0" : "=r"(flags)::"memory");
        return flags;
    }

    uint16_t read_cs(void)
    {
        uint16_t v = 0;
        __asm__ volatile("movw %%cs, %0" : "=r"(v));
        return v;
    }

    uint16_t read_ss(void)
    {
        uint16_t v = 0;
        __asm__ volatile("movw %%ss, %0" : "=r"(v));
        return v;
    }

    uint64_t read_rsp(void)
    {
        uint64_t v = 0;
        __asm__ volatile("movq %%rsp, %0" : "=r"(v));
        return v;
    }

    bool in_block(uint64_t addr, uint8_t const* block)
    {
        uintptr_t const lo = reinterpret_cast<uintptr_t>(block);
        return addr >= lo and addr < lo + kblock_bytes;
    }


    // --- The CPL3 reachability census ---------------------------------------
    // Walks the whole live hierarchy from the root and reports every leaf carrying the user bit
    // at every level, classified by what it maps. It runs from kickos_x86_64_landed once
    // arch_init has returned, so it sees the kernel window aspace_init installed.
    //
    // The device, low-legacy and outside counts are ASSERTED; the reachable translation table,
    // the per-core block and the leaves carrying them are a measured exposure PINNED to exactly
    // what was measured, so anything more reddens an arm and is named on a line of its own.
    constexpr uint64_t ent_present = 1ull << 0;
    constexpr uint64_t ent_write = 1ull << 1;
    constexpr uint64_t ent_user = 1ull << 2;
    constexpr uint64_t ent_large = 1ull << 7;
    constexpr uint64_t ent_addr = 0x000ffffffffff000ull;

    constexpr uint64_t apic_window_lo = 0xfec00000ull;
    constexpr uint64_t apic_window_hi = 0xfee01000ull;
    constexpr uint64_t low_legacy_hi = 0x100000ull;

    // Overflowing this is REFUSED: a census over an incomplete table record under-reports and
    // reads as clean.
    constexpr unsigned census_max_tables = 4096;
    uint64_t g_hier_tables[census_max_tables] = {};
    unsigned g_hier_tables_n = 0;
    unsigned g_hier_tables_dropped = 0;

    unsigned g_levels = 4;

    // The user bit over the whole hierarchy. `dormant` is the population the sibling question
    // is about: a leaf that carries the bit while an ancestor without it hides the leaf.
    struct u_census
    {
        uint64_t nonleaf;
        uint64_t leaf;
        uint64_t reachable;
        uint64_t dormant;
    };

    u_census g_u_before = {0, 0, 0, 0};
    u_census g_u_after = {0, 0, 0, 0};

    uint64_t g_reach_leaves = 0;
    uint64_t g_reach_bytes = 0;
    uint64_t g_reach_image = 0;
    uint64_t g_reach_arena = 0;
    uint64_t g_reach_table = 0;
    uint64_t g_reach_percpu = 0;
    uint64_t g_reach_low = 0;
    uint64_t g_reach_apic = 0;
    uint64_t g_reach_outside = 0;
    uint64_t g_reach_nonidentity = 0;
    uint64_t g_reach_writable = 0;
    uint64_t g_tables_reachable = 0;

    uint64_t g_img_lo = 0;
    uint64_t g_img_hi = 0;
    uint64_t g_arena_lo = 0;
    uint64_t g_arena_hi = 0;
    uint64_t g_percpu_lo = 0;
    uint64_t g_percpu_hi = 0;
    uint64_t g_anchor = 0;

    struct leaf_example
    {
        bool seen;
        uint64_t va;
        uint64_t pa;
        uint64_t size;
    };

    leaf_example g_ex_image = {false, 0, 0, 0};
    leaf_example g_ex_arena = {false, 0, 0, 0};
    leaf_example g_ex_table = {false, 0, 0, 0};
    leaf_example g_ex_percpu = {false, 0, 0, 0};
    leaf_example g_ex_outside = {false, 0, 0, 0};

    uint64_t g_first_table_reachable = 0;

    // Every reachable table page, listed so a page that is not the pinned one is NAMED.
    // Overflowing this is refused by the arm below, an unlisted page reading as clean.
    constexpr unsigned reachable_tables_max = 16;
    uint64_t g_reachable_tables[reachable_tables_max] = {};
    unsigned g_reachable_tables_n = 0;
    unsigned g_reachable_tables_dropped = 0;

    uint64_t g_kwin_table_pa = 0;
    bool g_anchor_is_percpu = false;

    // What the unprivileged body is pointed at, and what it read back. Loads only: the
    // permission bits the census prints are the hardware's own answer about writing.
    volatile uint64_t g_ring3_table_va = 0;
    volatile uint64_t g_ring3_anchor_va = 0;
    volatile uint64_t g_ring3_table_word = 0;
    volatile uint64_t g_ring3_anchor_word = 0;
    volatile bool g_ring3_read_table = false;
    volatile bool g_ring3_read_anchor = false;

    void keep(leaf_example* e, uint64_t va, uint64_t pa, uint64_t size)
    {
        if (e->seen)
        {
            return;
        }
        e->seen = true;
        e->va = va;
        e->pa = pa;
        e->size = size;
    }

    bool overlaps(uint64_t lo, uint64_t hi, uint64_t a, uint64_t b)
    {
        return lo < b and a < hi;
    }

    void note_hier_table(uint64_t pa)
    {
        for (unsigned i = 0; i < g_hier_tables_n; i++)
        {
            if (g_hier_tables[i] == pa)
            {
                return;
            }
        }
        if (g_hier_tables_n >= census_max_tables)
        {
            g_hier_tables_dropped++;
            return;
        }
        g_hier_tables[g_hier_tables_n] = pa;
        g_hier_tables_n++;
    }

    // Every table page in the hierarchy, reached through present entries whatever their user
    // bit: the question is which of them a leaf exposes as DATA, not which of them the walk
    // to a leaf goes through.
    void collect_tables(uint64_t table, unsigned level)
    {
        note_hier_table(table);
        if (level == 1)
        {
            return;
        }
        uint64_t const* const entries = reinterpret_cast<uint64_t const*>(table);
        for (unsigned i = 0; i < 512; i++)
        {
            uint64_t const entry = entries[i];
            if ((entry & ent_present) == 0 or (entry & ent_large) != 0)
            {
                continue;
            }
            collect_tables(entry & ent_addr, level - 1);
        }
    }

    bool holds_a_table(uint64_t pa, uint64_t size)
    {
        for (unsigned i = 0; i < g_hier_tables_n; i++)
        {
            if (g_hier_tables[i] >= pa and g_hier_tables[i] < pa + size)
            {
                return true;
            }
        }
        return false;
    }

    void count_user_bits(uint64_t table, unsigned level, bool chain, u_census* out)
    {
        uint64_t const* const entries = reinterpret_cast<uint64_t const*>(table);
        for (unsigned i = 0; i < 512; i++)
        {
            uint64_t const entry = entries[i];
            if ((entry & ent_present) == 0)
            {
                continue;
            }
            bool const user = (entry & ent_user) != 0;
            bool const leaf = (level == 1) or ((entry & ent_large) != 0);
            if (leaf)
            {
                if (user)
                {
                    out->leaf++;
                    if (chain)
                    {
                        out->reachable++;
                    }
                    else
                    {
                        out->dormant++;
                    }
                }
                continue;
            }
            if (user)
            {
                out->nonleaf++;
            }
            count_user_bits(entry & ent_addr, level - 1, chain and user, out);
        }
    }

    // The rule the processor applies, asked of one linear address: the user bit ANDed from the
    // root to the leaf.
    bool reachable_linear(uint64_t va)
    {
        uint64_t table = kickos::x86_64::read_cr3() & ent_addr;
        for (unsigned level = g_levels; level >= 1; level--)
        {
            unsigned const shift = 12 + 9 * (level - 1);
            unsigned const index = static_cast<unsigned>((va >> shift) & 0x1ffu);
            uint64_t const entry = reinterpret_cast<uint64_t const*>(table)[index];
            if ((entry & ent_present) == 0 or (entry & ent_user) == 0)
            {
                return false;
            }
            if ((level == 1) or ((entry & ent_large) != 0))
            {
                return true;
            }
            table = entry & ent_addr;
        }
        return false;
    }

    // The write permission ANDs down the walk the same way, so an unprivileged store needs
    // the bit at every level as well.
    bool writable_linear(uint64_t va)
    {
        uint64_t table = kickos::x86_64::read_cr3() & ent_addr;
        for (unsigned level = g_levels; level >= 1; level--)
        {
            unsigned const shift = 12 + 9 * (level - 1);
            unsigned const index = static_cast<unsigned>((va >> shift) & 0x1ffu);
            uint64_t const entry = reinterpret_cast<uint64_t const*>(table)[index];
            if ((entry & ent_present) == 0 or (entry & ent_user) == 0
                or (entry & ent_write) == 0)
            {
                return false;
            }
            if ((level == 1) or ((entry & ent_large) != 0))
            {
                return true;
            }
            table = entry & ent_addr;
        }
        return false;
    }

    // The entry that installs `child` in the live hierarchy.
    bool put_parent(uint64_t table, unsigned level, uint64_t child)
    {
        uint64_t const* const entries = reinterpret_cast<uint64_t const*>(table);
        for (unsigned i = 0; i < 512; i++)
        {
            uint64_t const entry = entries[i];
            if ((entry & ent_present) == 0 or (entry & ent_large) != 0 or level == 1)
            {
                continue;
            }
            if ((entry & ent_addr) == child)
            {
                put(" parent l");
                put_dec(level);
                put("[");
                put_dec(i);
                put("]@");
                put_hex(table);
                put("=");
                put_hex(entry);
                return true;
            }
            if (put_parent(entry & ent_addr, level - 1, child))
            {
                return true;
            }
        }
        return false;
    }

    void put_path(uint64_t va)
    {
        uint64_t table = kickos::x86_64::read_cr3() & ent_addr;
        for (unsigned level = g_levels; level >= 1; level--)
        {
            unsigned const shift = 12 + 9 * (level - 1);
            unsigned const index = static_cast<unsigned>((va >> shift) & 0x1ffu);
            uint64_t const entry = reinterpret_cast<uint64_t const*>(table)[index];
            put(" l");
            put_dec(level);
            put("[");
            put_dec(index);
            put("]@");
            put_hex(table);
            put("=");
            put_hex(entry);
            if ((entry & ent_present) == 0)
            {
                return;
            }
            if ((level == 1) or ((entry & ent_large) != 0))
            {
                return;
            }
            table = entry & ent_addr;
        }
    }

    // The kernel window's own level-3 table, named by the SLOT that installs it: a link moves
    // the address and not the slot.
    uint64_t kernel_window_table_pa(void)
    {
        uintptr_t const kwin = kickos::x86_64::aspace_kernel_window();
        if (kwin == 0)
        {
            return 0;
        }
        uint64_t const root = kickos::x86_64::read_cr3() & ent_addr;
        unsigned const shift = 12 + 9 * (g_levels - 1);
        unsigned const index = static_cast<unsigned>((kwin >> shift) & 0x1ffu);
        uint64_t const entry = reinterpret_cast<uint64_t const*>(root)[index];
        if ((entry & ent_present) == 0 or (entry & ent_large) != 0)
        {
            return 0;
        }
        return entry & ent_addr;
    }

    // Whether `anchor` addresses the per-core block itself, asked by writing through the
    // block's own setter and reading the word back through the anchor. The previous value is
    // put back: the syscall entry loads this word.
    bool anchor_is_percpu_block(uint64_t anchor)
    {
        if (anchor == 0)
        {
            return false;
        }
        constexpr uint64_t sentinel = 0x5ec0de00c0ffee01ull;
        uint64_t const keep = kickos::x86_64::cpu_kernel_sp();
        kickos::x86_64::cpu_set_kernel_sp(sentinel);
        uint64_t const seen =
            *reinterpret_cast<uint64_t volatile*>(anchor + KICKOS_X86_64_CPU_KERNEL_SP);
        kickos::x86_64::cpu_set_kernel_sp(keep);
        return seen == sentinel;
    }

    void put_example(char const* label, leaf_example const* e)
    {
        put("  ");
        put(KICKOS_X4_TOKEN);
        put(" census path ");
        put(label);
        if (not e->seen)
        {
            put(" none\n");
            return;
        }
        put(" va=");
        put_hex(e->va);
        put(" pa=");
        put_hex(e->pa);
        put(" size=");
        put_hex(e->size);
        put_path(e->va);
        put("\n");
    }

    void census_leaf(uint64_t va, uint64_t pa, uint64_t size, bool writable)
    {
        g_reach_leaves++;
        g_reach_bytes += size;
        if (writable)
        {
            g_reach_writable++;
        }
        if (va != pa)
        {
            g_reach_nonidentity++;
        }
        uint64_t const hi = pa + size;
        bool outside = true;
        if (overlaps(pa, hi, g_img_lo, g_img_hi))
        {
            g_reach_image++;
            outside = false;
            keep(&g_ex_image, va, pa, size);
        }
        if (overlaps(pa, hi, g_arena_lo, g_arena_hi))
        {
            g_reach_arena++;
            outside = false;
            keep(&g_ex_arena, va, pa, size);
        }
        if (holds_a_table(pa, size))
        {
            g_reach_table++;
            keep(&g_ex_table, va, pa, size);
        }
        if (overlaps(pa, hi, g_percpu_lo, g_percpu_hi))
        {
            g_reach_percpu++;
            keep(&g_ex_percpu, va, pa, size);
        }
        if (overlaps(pa, hi, 0, low_legacy_hi))
        {
            g_reach_low++;
        }
        if (overlaps(pa, hi, apic_window_lo, apic_window_hi))
        {
            g_reach_apic++;
        }
        if (outside)
        {
            g_reach_outside++;
            keep(&g_ex_outside, va, pa, size);
            if (g_reach_outside <= 32)
            {
                put("  ");
                put(KICKOS_X4_TOKEN);
                put(" census outside va=");
                put_hex(va);
                put(" pa=");
                put_hex(pa);
                put(" size=");
                put_hex(size);
                put("\n");
            }
        }
    }

    void census_walk(uint64_t table, unsigned level, uint64_t va_prefix, bool rw)
    {
        uint64_t const* const entries = reinterpret_cast<uint64_t const*>(table);
        unsigned const shift = 12 + 9 * (level - 1);
        for (unsigned i = 0; i < 512; i++)
        {
            uint64_t const entry = entries[i];
            if ((entry & ent_present) == 0 or (entry & ent_user) == 0)
            {
                continue;
            }
            uint64_t va = va_prefix | (static_cast<uint64_t>(i) << shift);
            // The top level's index reaches the sign bit of a canonical address.
            if (level == g_levels and (va & (1ull << (shift + 8))) != 0)
            {
                va |= ~((1ull << (shift + 9)) - 1);
            }
            if ((level == 1) or ((entry & ent_large) != 0))
            {
                uint64_t const size = 1ull << shift;
                // Bit 12 of a large-page entry is an attribute selector, not an address bit.
                uint64_t const pa = entry & ent_addr & ~(size - 1);
                census_leaf(va, pa, size, rw and (entry & ent_write) != 0);
                continue;
            }
            census_walk(entry & ent_addr, level - 1, va, rw and (entry & ent_write) != 0);
        }
    }

    // The state firmware left, taken BEFORE arch_init runs the grant. The console is not up
    // yet, so census_report is what prints the figures.
    void census_before_grant(void)
    {
        g_levels = 4;
        if ((kickos::x86_64::read_cr4() & (1ull << 12)) != 0)
        {
            g_levels = 5;
        }
        count_user_bits(kickos::x86_64::read_cr3() & ent_addr, g_levels, true, &g_u_before);
    }

    void census_report(void)
    {
        g_img_lo = kickos::x86_64::image_base();
        g_img_hi = g_img_lo + kickos::x86_64::image_size();
        g_arena_lo = arch_ram_base();
        g_arena_hi = g_arena_lo + arch_ram_size();
        // The anchor the syscall entry loads, read out of the register rather than assumed.
        g_anchor = kickos::x86_64::read_msr(0xc0000101u);
        g_percpu_lo = g_anchor;
        g_percpu_hi = g_anchor + sizeof(kickos::x86_64::cpu_block);

        uint64_t const root = kickos::x86_64::read_cr3() & ent_addr;
        collect_tables(root, g_levels);
        count_user_bits(root, g_levels, true, &g_u_after);

        put("  ");
        put(KICKOS_X4_TOKEN);
        put(" census levels=");
        put_dec(g_levels);
        put(" root=");
        put_hex(root);
        put(" tables=");
        put_dec(g_hier_tables_n);
        put(" dropped=");
        put_dec(g_hier_tables_dropped);
        put("\n  ");
        put(KICKOS_X4_TOKEN);
        put(" census extents image=");
        put_hex(g_img_lo);
        put("..");
        put_hex(g_img_hi);
        put(" arena=");
        put_hex(g_arena_lo);
        put("..");
        put_hex(g_arena_hi);
        put("\n");

        census_walk(root, g_levels, 0, true);

        for (unsigned i = 0; i < g_hier_tables_n; i++)
        {
            if (reachable_linear(g_hier_tables[i]))
            {
                g_tables_reachable++;
                if (g_first_table_reachable == 0)
                {
                    g_first_table_reachable = g_hier_tables[i];
                }
                if (g_reachable_tables_n < reachable_tables_max)
                {
                    g_reachable_tables[g_reachable_tables_n] = g_hier_tables[i];
                    g_reachable_tables_n++;
                }
                else
                {
                    g_reachable_tables_dropped++;
                }
            }
        }

        put("  ");
        put(KICKOS_X4_TOKEN);
        put(" census reachable leaves=");
        put_dec(g_reach_leaves);
        put(" bytes=");
        put_hex(g_reach_bytes);
        put(" writable=");
        put_dec(g_reach_writable);
        put(" nonidentity=");
        put_dec(g_reach_nonidentity);
        put("\n  ");
        put(KICKOS_X4_TOKEN);
        put(" census overlap image=");
        put_dec(g_reach_image);
        put(" arena=");
        put_dec(g_reach_arena);
        put(" table=");
        put_dec(g_reach_table);
        put(" percpu=");
        put_dec(g_reach_percpu);
        put(" lowlegacy=");
        put_dec(g_reach_low);
        put(" apicwindow=");
        put_dec(g_reach_apic);
        put(" outside=");
        put_dec(g_reach_outside);
        put("\n  ");
        put(KICKOS_X4_TOKEN);
        put(" census pregrant u_nonleaf=");
        put_dec(g_u_before.nonleaf);
        put(" u_leaf=");
        put_dec(g_u_before.leaf);
        put(" u_reachable=");
        put_dec(g_u_before.reachable);
        put(" u_dormant=");
        put_dec(g_u_before.dormant);
        put("\n  ");
        put(KICKOS_X4_TOKEN);
        put(" census postgrant u_nonleaf=");
        put_dec(g_u_after.nonleaf);
        put(" u_leaf=");
        put_dec(g_u_after.leaf);
        put(" u_reachable=");
        put_dec(g_u_after.reachable);
        put(" u_dormant=");
        put_dec(g_u_after.dormant);
        put("\n  ");
        put(KICKOS_X4_TOKEN);
        put(" census q1 tables_reachable=");
        put_dec(g_tables_reachable);
        put(" of=");
        put_dec(g_hier_tables_n);
        put(" first=");
        put_hex(g_first_table_reachable);
        put(" first_writable=");
        put_dec(static_cast<uint64_t>(writable_linear(g_first_table_reachable)));
        put("\n  ");
        put(KICKOS_X4_TOKEN);
        put(" census q2 device_leaves=");
        put_dec(g_reach_low + g_reach_apic);
        put(" lowlegacy=");
        put_dec(g_reach_low);
        put(" apicwindow=");
        put_dec(g_reach_apic);
        put("\n  ");
        put(KICKOS_X4_TOKEN);
        put(" census q3 anchor=");
        put_hex(g_anchor);
        put(" reachable=");
        put_dec(static_cast<uint64_t>(reachable_linear(g_anchor)));
        put(" writable=");
        put_dec(static_cast<uint64_t>(writable_linear(g_anchor)));
        put(" kernel_sp_at=");
        put_hex(g_anchor + KICKOS_X86_64_CPU_KERNEL_SP);
        put("\n");

        put_example("image", &g_ex_image);
        put_example("arena", &g_ex_arena);
        put_example("table", &g_ex_table);
        put_example("percpu", &g_ex_percpu);
        put_example("outside", &g_ex_outside);
        if (g_first_table_reachable != 0)
        {
            put("  ");
            put(KICKOS_X4_TOKEN);
            put(" census path table_pa va=");
            put_hex(g_first_table_reachable);
            put_path(g_first_table_reachable);
            put_parent(root, g_levels, g_first_table_reachable);
            put("\n");
        }
        put("  ");
        put(KICKOS_X4_TOKEN);
        put(" census path anchor va=");
        put_hex(g_anchor);
        put_path(g_anchor);
        put("\n");

        g_ring3_table_va = g_first_table_reachable;
        g_ring3_anchor_va = g_anchor;

        g_kwin_table_pa = kernel_window_table_pa();
        g_anchor_is_percpu = anchor_is_percpu_block(g_anchor);

        put("  ");
        put(KICKOS_X4_TOKEN);
        put(" census pin tables_reachable=");
        put_dec(g_tables_reachable);
        put(" pinned=1 listed=");
        put_dec(g_reachable_tables_n);
        put(" unlisted=");
        put_dec(g_reachable_tables_dropped);
        put(" kernel_window_l3=");
        put_hex(g_kwin_table_pa);
        put("\n");
        for (unsigned i = 0; i < g_reachable_tables_n; i++)
        {
            put("  ");
            put(KICKOS_X4_TOKEN);
            put(" census pin table pa=");
            put_hex(g_reachable_tables[i]);
            put(" writable=");
            put_dec(static_cast<uint64_t>(writable_linear(g_reachable_tables[i])));
            put(" role=");
            if (g_kwin_table_pa != 0 and g_reachable_tables[i] == g_kwin_table_pa)
            {
                put("kernel-window-l3");
            }
            else
            {
                put("UNPINNED");
            }
            put("\n");
        }
        put("  ");
        put(KICKOS_X4_TOKEN);
        put(" census pin anchor pa=");
        put_hex(g_anchor);
        put(" leaves=");
        put_dec(g_reach_percpu);
        put(" pinned=1 role=");
        if (g_anchor_is_percpu)
        {
            put("per-core-block");
        }
        else
        {
            put("UNPINNED");
        }
        put("\n");

        // The walker's own control and its floor: a census that walks nothing reports zero
        // reachable leaves and satisfies every clause below it.
        arm("census_table_record_complete", g_hier_tables_dropped == 0);
        arm("census_hierarchy_floor", g_hier_tables_n >= g_levels);
        arm("census_found_reachable_leaves", g_reach_leaves > 0);
        arm("census_found_the_image", g_reach_image > 0);
        arm("census_found_the_arena", g_reach_arena > 0);
        arm("census_control_admits_a_reachable_leaf",
            g_ex_image.seen and reachable_linear(g_ex_image.va));
        arm("census_control_refuses_the_root_table", not reachable_linear(root));

        // What this port claims, asserted: the two device bands the census classifies, then the
        // general clause.
        arm("census_no_apic_window_leaves", g_reach_apic == 0);
        arm("census_no_low_legacy_leaves", g_reach_low == 0);
        arm("census_no_leaves_outside_image_or_arena", g_reach_outside == 0);

        // What is exposed, pinned to what was measured. The counts are exact, so a page that
        // appears reddens an arm.
        arm("census_pin_reachable_tables_listed", g_reachable_tables_dropped == 0);
        arm("census_pin_one_reachable_table", g_tables_reachable == 1);
        arm("census_pin_table_is_the_kernel_window",
            g_kwin_table_pa != 0 and g_reachable_tables_n == 1
                and g_reachable_tables[0] == g_kwin_table_pa);
        arm("census_pin_one_table_bearing_leaf", g_reach_table == 1);
        arm("census_pin_anchor_is_the_percpu_block", g_anchor_is_percpu);
        arm("census_pin_one_percpu_leaf", g_reach_percpu == 1);
        arm("census_pin_one_image_leaf", g_reach_image == 1);
        arm("census_pin_reachable_set_is_image_and_arena",
            g_reach_leaves == g_reach_image + g_reach_arena);
        arm("census_pin_table_and_anchor_writable",
            writable_linear(g_first_table_reachable) and writable_linear(g_anchor));
    }

    // --- The unprivileged bodies --------------------------------------------
    void user_a(void* arg)
    {
        (void)arg;
        g_user_cs = read_cs();
        g_user_ss = read_ss();
        g_user_flags = read_flags();

        // At ring 3, through the live regime: the census says these two linear addresses are
        // reachable, and these loads are what says so without a walk.
        if (g_ring3_table_va != 0)
        {
            g_ring3_table_word = *reinterpret_cast<uint64_t volatile*>(g_ring3_table_va);
            g_ring3_read_table = true;
        }
        if (g_ring3_anchor_va != 0)
        {
            g_ring3_anchor_word = *reinterpret_cast<uint64_t volatile*>(g_ring3_anchor_va);
            g_ring3_read_anchor = true;
        }

        g_echo64 = arch_syscall64(nr_echo, echo_a0, echo_a1, echo_a2, echo_a3);
        g_echo_low = arch_syscall(nr_echo, echo_a0, echo_a1, echo_a2, echo_a3);

        // A blocking syscall: the dispatch switches away and this call returns only when the
        // continuation it parked on this thread's own kernel block is resumed.
        g_block_ret = arch_syscall(nr_block, 0, 0, 0, 0);

        // Spins at ring 3 until the timer preempts it, which is the only arm that exercises an
        // interrupt gate entered from ring 3.
        uint64_t spins = 0;
        while (g_preempts == 0 and spins < 400000000ull)
        {
            spins++;
        }
        g_spun = spins;

        // The flags this thread ASKED to come back with: an I/O privilege level of 3 would
        // open every device port to it.
        g_forged_back = kickos_x86_64_probe_flag_forge(nr_ring0, 0x3202ull);

        // A fault taken by the DISPATCH, at ring 0, on this thread's own block: the control
        // that separates the block test from the privilege test in arch_fault_is_user_thread.
        arch_syscall(nr_kfault, 0, 0, 0, 0);
        // Returns into kickos_user_thread_return, which exits through the trap.
    }

    void user_fault(void* arg)
    {
        (void)arg;
        if (g_fault_class == fault_class_cr0)
        {
            kickos_x86_64_probe_cr0();
        }
        else
        {
            kickos_x86_64_probe_outb();
        }
        // NOT REACHED unless the instruction was permitted, which is the arm.
        g_fault_body_continued = true;
        arch_syscall(nr_exit, 0, 0, 0, 0);
        while (true)
        {
        }
    }

    // --- The arms -----------------------------------------------------------
    void arm_registers(void)
    {
        using namespace kickos::x86_64;
        uint64_t const efer = read_msr(0xc0000080);
        uint64_t const star = read_msr(0xc0000081);
        uint64_t const lstar = read_msr(0xc0000082);
        uint64_t const fmask = read_msr(0xc0000084);

        put("  ");
        put(KICKOS_X4_TOKEN);
        put(" cr4=");
        put_hex(control_flags());
        put(" efer=");
        put_hex(efer);
        put("\n  ");
        put(KICKOS_X4_TOKEN);
        put(" star=");
        put_hex(star);
        put(" fmask=");
        put_hex(fmask);
        put("\n  ");
        put(KICKOS_X4_TOKEN);
        put(" image=");
        put_hex(image_base());
        put(" size=");
        put_hex(image_size());
        put(" sections=");
        put_dec(image_sections());
        put("\n  ");
        put(KICKOS_X4_TOKEN);
        put(" leaves granted=");
        put_dec(user_leaves_granted());
        put(" already=");
        put_dec(user_leaves_already());
        put(" tables_exposed=");
        put_dec(user_tables_exposed());
        put(" tables_walked=");
        put_dec(user_tables_walked());
        put("\n");

        arm("efer_sce_set", (efer & 1ull) != 0);
        // Read back from the register: bit 9 is the interrupt flag, and SYSCALL clears every
        // flag the mask names.
        arm("fmask_clears_if", (fmask & (1ull << 9)) != 0);
        arm("fmask_clears_iopl", (fmask & (3ull << 12)) == (3ull << 12));
        arm("star_syscall_cs", ((star >> 32) & 0xffffu) == sel_kernel_code);
        arm("star_sysret_base", ((star >> 48) & 0xffffu) + 16 == (sel_user_code & ~3u));
        arm("lstar_in_image", lstar >= image_base() and lstar < image_base() + image_size());
        arm("smep_smap_clear", (control_flags() & ((1ull << 20) | (1ull << 21))) == 0);
        arm("user_leaves_granted", user_leaves_granted() > 0);
        // Named for their CORPUS, the tables the grant walked, taken inside ring3_init. The
        // whole-hierarchy census below is what answers the reachability question.
        arm("walked_tables_census_nonempty", user_tables_walked() > 0);
        arm("walked_tables_not_user_reachable", user_tables_exposed() == 0);
    }

    void arm_oracles(void)
    {
        uintptr_t const ro = reinterpret_cast<uintptr_t>(g_ro_subject);
        uintptr_t const rw = reinterpret_cast<uintptr_t>(&g_rw_subject);
        uintptr_t const text = reinterpret_cast<uintptr_t>(&kickos_x86_64_probe_ud2);
        uintptr_t const arena = arch_ram_base();
        uintptr_t const past = kickos::x86_64::image_base() + kickos::x86_64::image_size();

        arm("oracle_read_admits_rodata", arch_user_text_readable(ro, 8));
        arm("oracle_read_admits_text", arch_user_text_readable(text, 8));
        arm("oracle_read_admits_data", arch_user_text_readable(rw, 8));
        arm("oracle_read_refuses_null", not arch_user_text_readable(0, 8));
        arm("oracle_read_refuses_arena", not arch_user_text_readable(arena, 8));
        arm("oracle_read_refuses_past_image", not arch_user_text_readable(past, 8));
        // The unit is one SECTION: a long range wholly inside one is admitted, and a range that
        // leaves its section is refused with both ends still inside the image.
        arm("oracle_read_admits_long_range", arch_user_text_readable(rw, 8192));
        arm("oracle_read_refuses_cross_section",
            not arch_user_text_readable(text, past - 1 - text));
        arm("oracle_read_zero_len", arch_user_text_readable(0, 0));

        arm("oracle_write_admits_data", arch_user_data_writable(rw, 8));
        // The discriminating pair: read admits this address and write must not.
        arm("oracle_write_refuses_text", not arch_user_data_writable(text, 8));
        arm("oracle_write_refuses_rodata", not arch_user_data_writable(ro, 8));
        arm("oracle_write_refuses_arena", not arch_user_data_writable(arena, 8));
        arm("oracle_write_zero_len", arch_user_data_writable(0, 0));
    }

    void arm_ring0_controls(void)
    {
        // The same instruction the unprivileged arm is refused for. It returns here, so the
        // refusal below is about the LEVEL and not about the instruction.
        g_skip_bytes = 0;
        kickos_x86_64_probe_cr0();
        g_cr0_at_ring0_returned = true;
        arm("cr0_read_returns_at_ring0", g_cr0_at_ring0_returned);
    }

    void run_user_a(void)
    {
        g_ctx_a.kernel_sp = reinterpret_cast<uintptr_t>(g_kblock_a) + kblock_bytes;
        arch_context_init(&g_ctx_a, user_a, nullptr, g_ustack_a, sizeof(g_ustack_a), 0);
        arm("ctx_frame_on_block", in_block(g_ctx_a.sp, g_kblock_a));
        arm("ctx_kernel_sp_kept",
            g_ctx_a.kernel_sp == reinterpret_cast<uintptr_t>(g_kblock_a) + kblock_bytes);

        g_running_kernel_sp = g_ctx_a.kernel_sp;
        arch_timer_arm(arch_clock_now() + 20 * ns_per_ms);
        while (not g_a_exited)
        {
            arch_irq_state_t const state = arch_irq_save();
            g_phase = phase_user;
            arch_switch(&g_main, &g_ctx_a);
            g_phase = phase_main;
            arch_irq_restore(state);
        }
        arch_timer_disarm();
        g_running_kernel_sp = 0;

        put("  ");
        put(KICKOS_X4_TOKEN);
        put(" user cs=");
        put_hex(g_user_cs);
        put(" ss=");
        put_hex(g_user_ss);
        put(" flags=");
        put_hex(g_user_flags);
        put("\n  ");
        put(KICKOS_X4_TOKEN);
        put(" dispatch rsp=");
        put_hex(g_sys_rsp);
        put(" flags=");
        put_hex(g_sys_flags);
        put(" cs=");
        put_hex(g_sys_cs);
        put("\n  ");
        put(KICKOS_X4_TOKEN);
        put(" calls=");
        put_dec(g_sys_calls);
        put(" preempts=");
        put_dec(g_preempts);
        put(" spun=");
        put_dec(g_spun);
        put("\n");

        // The LIVE register, read by the thread itself: the current privilege level is the low
        // two bits of cs.
        arm("ring3_cs_is_user", g_user_cs == kickos::x86_64::sel_user_code);
        arm("ring3_cpl_is_three", (g_user_cs & 3u) == 3);
        arm("ring3_ss_is_user", g_user_ss == kickos::x86_64::sel_user_data);
        arm("ring3_runs_with_interrupts", (g_user_flags & (1ull << 9)) != 0);
        arm("ring3_iopl_zero", (g_user_flags & (3ull << 12)) == 0);

        // The hazard arm: the dispatch must stand on the calling thread's own kernel block.
        // Both halves are asserted, inside the block and outside the user stack.
        arm("syscall_on_kernel_block", in_block(g_sys_rsp, g_kblock_a));
        arm("syscall_off_user_stack",
            g_sys_rsp < reinterpret_cast<uintptr_t>(g_ustack_a)
                or g_sys_rsp >= reinterpret_cast<uintptr_t>(g_ustack_a) + sizeof(g_ustack_a));
        arm("syscall_frame_below_block_top",
            g_sys_rsp <= g_ctx_a.kernel_sp - sizeof(trap_frame));

        // The flag mask, WITNESSED: the caller had the interrupt flag set and the dispatch sees
        // it clear.
        arm("syscall_interrupts_masked", (g_sys_flags & (1ull << 9)) == 0);
        arm("syscall_runs_at_ring0", (g_sys_cs & 3u) == 0);
        arm("syscall_not_in_isr", g_sys_in_isr == 0);
        arm("syscall_args_delivered", g_sys_args_ok);
        arm("syscall_result64", g_echo64 == echo_result);
        arm("syscall_result_low", g_echo_low == static_cast<uintptr_t>(echo_result));

        // Both places the incoming block is published to, read from the dispatch.
        arm("tss_rsp0_is_block_top", g_sys_tss_rsp0 == g_ctx_a.kernel_sp);
        arm("percpu_sp_is_block_top", g_sys_cpu_sp == g_ctx_a.kernel_sp);

        arm("syscall_blocks_and_resumes", g_blocked and g_resumed);
        arm("syscall_block_result", g_block_ret == static_cast<uintptr_t>(block_result));
        // The forged r11 asked for an I/O privilege level of 3; the thread must come back with
        // the level at 0.
        put("  ");
        put(KICKOS_X4_TOKEN);
        put(" forged_flags_returned=");
        put_hex(g_forged_back);
        put("\n");
        arm("ring3_cannot_raise_iopl", (g_forged_back & (3ull << 12)) == 0);
        arm("ring3_keeps_interrupts_on_return", (g_forged_back & (1ull << 9)) != 0);

        arm("ring3_preempted", g_preempts >= 1 and g_spun > 0);
        arm("ring3_exits_through_trap", g_a_exited);

        // The ring 0 fault taken by the dispatch, on this thread's own block.
        arm("kfault_seen", g_kfaults == 1);
        arm("kfault_on_kernel_block", g_kfault_on_block);
        arm("kfault_cs_is_kernel", (g_kfault_cs & 3u) == 0);
        arm("kfault_not_attributed", not g_kfault_attributed);
    }

    void run_user_fault(unsigned cls, char const* label)
    {
        g_fault_class = cls;
        g_fault_body_continued = false;
        g_fault_attributed = false;
        g_fault_on_block = false;
        uint32_t const deaths_before = g_deaths;

        g_ctx_f.kernel_sp = reinterpret_cast<uintptr_t>(g_kblock_f) + kblock_bytes;
        arch_context_init(&g_ctx_f, user_fault, nullptr, g_ustack_f, sizeof(g_ustack_f), 0);
        g_running_kernel_sp = g_ctx_f.kernel_sp;

        arch_irq_state_t const state = arch_irq_save();
        arch_switch(&g_main, &g_ctx_f);
        arch_irq_restore(state);
        g_running_kernel_sp = 0;

        put("  ");
        put(KICKOS_X4_TOKEN);
        put(" fault ");
        put(label);
        put(" vector=");
        put_dec(g_fault_vector);
        put(" cs=");
        put_hex(g_fault_cs);
        put(" death cs=");
        put_hex(g_death_cs);
        put("\n");

        arm2(label, "_refused", not g_fault_body_continued);
        arm2(label, "_general_protection", g_fault_vector == 13);
        arm2(label, "_frame_cs_is_user", (g_fault_cs & 3u) == 3);
        arm2(label, "_attributed_to_thread", g_fault_attributed);
        arm2(label, "_frame_on_kernel_block", g_fault_on_block);
        arm2(label, "_death_stub_privileged", (g_death_cs & 3u) == 0);
        arm2(label, "_death_stub_on_block", in_block(g_death_rsp, g_kblock_f));
        arm2(label, "_thread_died", g_deaths == deaths_before + 1);
    }

    void sysret_launcher(void* arg)
    {
        (void)arg;
        uintptr_t const top = (reinterpret_cast<uintptr_t>(g_ustack_s) + sizeof(g_ustack_s))
                              & ~static_cast<uintptr_t>(15);
        kickos_x86_64_probe_sysret(reinterpret_cast<uintptr_t>(&kickos_x86_64_probe_sysret_stub),
                                   top, nr_sysret);
    }

    void arm_sysret(void)
    {
        uintptr_t const block = reinterpret_cast<uintptr_t>(g_kblock_s) + kblock_bytes;
        // Entered through an ORDINARY SWITCH, so the frame the dispatch resumes main from is
        // one main itself just wrote. The launcher is privileged, and its block is what the
        // ring 3 stub's own syscall stands on.
        g_ctx_s.kernel_sp = block;
        arch_context_init(&g_ctx_s, sysret_launcher, nullptr, g_lstack, sizeof(g_lstack), 1);
        g_ctx_s.kernel_sp = block;
        g_running_kernel_sp = block;

        arch_irq_state_t const state = arch_irq_save();
        arch_switch(&g_main, &g_ctx_s);
        arch_irq_restore(state);
        g_running_kernel_sp = 0;

        put("  ");
        put(KICKOS_X4_TOKEN);
        put(" sysret cs=");
        put_hex(g_sysret_cs);
        put(" ss=");
        put_hex(g_sysret_ss);
        put("\n");
        arm("sysret_reached_ring3", g_sysret_arrived);
        arm("sysret_cs_is_user", g_sysret_cs == kickos::x86_64::sel_user_code);
        arm("sysret_ss_is_user", g_sysret_ss == kickos::x86_64::sel_user_data);
    }

    void census_ring3_report(void)
    {
        put("  ");
        put(KICKOS_X4_TOKEN);
        put(" census ring3 table_va=");
        put_hex(g_ring3_table_va);
        put(" read=");
        put_dec(static_cast<uint64_t>(g_ring3_read_table));
        put(" word=");
        put_hex(g_ring3_table_word);
        put(" ring0_word=");
        if (g_ring3_table_va != 0)
        {
            put_hex(*reinterpret_cast<uint64_t volatile*>(g_ring3_table_va));
        }
        else
        {
            put_hex(0);
        }
        put("\n  ");
        put(KICKOS_X4_TOKEN);
        put(" census ring3 anchor_va=");
        put_hex(g_ring3_anchor_va);
        put(" read=");
        put_dec(static_cast<uint64_t>(g_ring3_read_anchor));
        put(" word=");
        put_hex(g_ring3_anchor_word);
        put(" ring0_word=");
        put_hex(kickos::x86_64::cpu_kernel_sp());
        put("\n");
    }
}

extern "C"
{

// --- The kernel's side of the seam, supplied by this image -------------------
uint64_t syscall_dispatch(uintptr_t nr, uintptr_t a0, uintptr_t a1, uintptr_t a2, uintptr_t a3)
{
    g_sys_calls = g_sys_calls + 1;
    g_sys_rsp = read_rsp();
    g_sys_flags = read_flags();
    g_sys_cs = read_cs();
    g_sys_in_isr = arch_in_isr();
    g_sys_tss_rsp0 = kickos::x86_64::tss_rsp0();
    g_sys_cpu_sp = kickos::x86_64::cpu_kernel_sp();

    if (nr == nr_echo)
    {
        g_sys_args_ok = (a0 == echo_a0 and a1 == echo_a1 and a2 == echo_a2 and a3 == echo_a3);
        return echo_result;
    }
    if (nr == nr_block)
    {
        // Blocks: the dispatch is on this thread's own continuation, so an ordinary synchronous
        // switch parks it and resuming the thread resumes this call inline.
        g_blocked = true;
        arch_switch(&g_ctx_a, &g_main);
        g_resumed = true;
        return block_result;
    }
    if (nr == nr_kfault)
    {
        // A fault at ring 0 whose frame lands on the calling thread's own kernel block. The
        // reporter below steps past it, so the image continues.
        g_skip_bytes = 2;
        kickos_x86_64_probe_ud2();
        g_skip_bytes = 0;
        return 0;
    }
    if (nr == nr_sysret)
    {
        g_sysret_cs = static_cast<uint16_t>(a0);
        g_sysret_ss = static_cast<uint16_t>(a1);
        g_sysret_arrived = true;
        arch_switch(&g_ctx_s, &g_main);
        while (true)
        {
            __asm__ volatile("cli\n\thlt");
        }
    }
    if (nr == nr_ring0)
    {
        return ring0_result;
    }
    // nr_exit and anything else: park the caller for good.
    g_a_exited = true;
    arch_switch(&g_ctx_a, &g_main);
    while (true)
    {
        __asm__ volatile("cli\n\thlt");
    }
}

// The chip's arch_init carries the map editor, so it is in this link too; this image has no
// pool, so a create answers null.
arch_phys_addr_t kickos_frame_alloc(void)
{
    return 0;
}

void kickos_frame_free(arch_phys_addr_t frame)
{
    (void)frame;
}

void kickos_isr_timer(void)
{
    g_timer_count = g_timer_count + 1;
    if (g_phase != phase_user)
    {
        return;
    }
    if (g_preempts != 0)
    {
        return;
    }
    g_preempts = g_preempts + 1;
    // A deferred switch, booked from the interrupt the unprivileged thread was taking: the
    // frame the entry built on its block IS its saved context.
    arch_switch(&g_ctx_a, &g_main);
}

void kickos_isr_irq(int irq)
{
    (void)irq;
}

// A PRIVILEGED thread's entry returned; the arch plants kickos_user_thread_return for the
// unprivileged ones.
void kickos_thread_return(void)
{
    while (true)
    {
        __asm__ volatile("cli\n\thlt");
    }
}

// An UNPRIVILEGED thread's entry returned, at ring 3. The trap is its only way to the kernel.
void kickos_user_thread_return(void)
{
    arch_syscall(nr_exit, 0, 0, 0, 0);
    while (true)
    {
    }
}

// --- Fault isolation's kernel half ------------------------------------------
// Stands in for kernel/init/fault.cc, which this image does not link.
bool kickos_fault_frame_on_kernel_stack(void const* frame, size_t bytes)
{
    uintptr_t const hi = g_running_kernel_sp;
    if (hi == 0)
    {
        return false;
    }
    uintptr_t const lo = hi - kblock_bytes;
    uintptr_t const f = reinterpret_cast<uintptr_t>(frame);
    return f >= lo and f < hi and bytes <= (hi - f);
}

uintptr_t kickos_fault_stack_top(void)
{
    return g_running_kernel_sp;
}

void kickos_fault_record(char const* status_name, uint64_t status,
                         uintptr_t pc, uintptr_t addr, int addr_valid)
{
    (void)status_name;
    (void)status;
    (void)pc;
    (void)addr;
    (void)addr_valid;
}

bool kickos_fault_kill_thread(void* frame)
{
    trap_frame* const f = static_cast<trap_frame*>(frame);
    bool const on_block = kickos_fault_frame_on_kernel_stack(frame, sizeof(trap_frame));
    bool const attributed = arch_fault_is_user_thread(frame);

    if (not attributed)
    {
        // The ring 0 control: recorded and then stepped past, so one image carries both
        // directions of the attribution test.
        g_kfaults = g_kfaults + 1;
        g_kfault_attributed = attributed;
        g_kfault_on_block = on_block;
        g_kfault_cs = f->cs;
        if (g_skip_bytes == 0)
        {
            return false;
        }
        f->rip = f->rip + g_skip_bytes;
        return true;
    }

    g_faults = g_faults + 1;
    g_fault_vector = f->vector;
    g_fault_cs = f->cs;
    g_fault_attributed = attributed;
    g_fault_on_block = on_block;
    arch_fault_redirect_to_exit(frame);
    return true;
}

// Where the redirect points the dying thread: privileged, on its own block's top.
void kickos_thread_fault_exit(void)
{
    g_deaths = g_deaths + 1;
    g_death_cs = read_cs();
    g_death_rsp = read_rsp();
    arch_switch(&g_ctx_f, &g_main);
    while (true)
    {
        __asm__ volatile("cli\n\thlt");
    }
}

void kickos_x86_64_landed(uintptr_t ram_base, uint64_t ram_size)
{
    kickos::q35::ram_publish(ram_base, static_cast<size_t>(ram_size));
    // BEFORE arch_init, which is where the grant runs.
    census_before_grant();
    arch_init();

    put("\n" KICKOS_X4_TOKEN " arms\n");
    arm_registers();
    census_report();
    arm_oracles();
    arm_ring0_controls();
    run_user_a();
    census_ring3_report();
    run_user_fault(fault_class_cr0, "cr0_read");
    run_user_fault(fault_class_outb, "port_write");
    arm_sysret();

    // The privileged leaf's own branch: a ring 0 caller reaches the dispatch by a plain call,
    // SYSCALL recording no privilege level for the entry to branch on.
    uint64_t const before = g_sys_calls;
    uintptr_t const r = arch_syscall(nr_ring0, 0, 0, 0, 0);
    arm("ring0_syscall_result", r == static_cast<uintptr_t>(ring0_result));
    arm("ring0_syscall_dispatched", g_sys_calls == before + 1);
    arm("ring0_syscall_on_caller_stack", not in_block(g_sys_rsp, g_kblock_a));


    if (g_failed)
    {
        put(KICKOS_X4_TOKEN " FAIL\n");
        arch_shutdown(1);
    }
    put(KICKOS_X4_TOKEN " PASS\n");
    arch_shutdown(0);
}

}
