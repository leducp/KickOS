// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// x86_64 ring 3: what makes an unprivileged level reachable at all, and the fast syscall
// pair's registers.
//
// On this firmware every entry along the walk to the image, to its data and to conventional
// memory carries the user bit CLEAR, and the permission ANDs down the walk, so it has to be
// granted at every entry from the root to the leaf. This file grants it over two ranges and
// leaves every other entry as firmware left it.
//
// THE GRANT IS BROAD, the unit the hardware grants being a LEAF and the image one flat link:
// an unprivileged thread can read and write kernel memory here, including a live translation
// table and the per-core block the syscall entry loads through IA32_GS_BASE. What stays out of
// reach is the device clause, and ring 3 can neither execute a privileged instruction, touch a
// port, raise its own level, nor write IA32_KERNEL_GS_BASE.
//
// The census in this file runs BEFORE aspace_init and covers only the tables the grant walked;
// probe4_x86_64.cc is what walks the whole hierarchy.

#include <kickos/arch/desc.h>
#include <kickos/arch/regs.h>
#include <kickos/arch/ring3.h>
#include <kickos/chip_com1.h>

#include <stddef.h>
#include <stdint.h>

extern "C" void kfault_terminate(void) __attribute__((noreturn));

// switch.S. HIDDEN is load-bearing: -fpie emits a global-offset-table load for the address of
// an external function and `ld -m i386pep` neither builds that table nor relaxes the form
// (tools/check-x86_64-no-got.sh).
extern "C" __attribute__((visibility("hidden"))) void kickos_x86_64_syscall_entry(void);

// The linker's own symbol for the image's first byte. Taken PC-relative, so it is the address
// firmware LOADED the image at; the PE carries an empty base-relocation directory, so nothing
// else here may be an absolute either.
extern "C" __attribute__((visibility("hidden"))) char __ImageBase[];

namespace kickos::x86_64
{
    namespace
    {
        using namespace kickos::q35;

        constexpr uint32_t msr_efer = 0xc0000080;
        constexpr uint32_t msr_star = 0xc0000081;
        constexpr uint32_t msr_lstar = 0xc0000082;
        constexpr uint32_t msr_fmask = 0xc0000084;
        constexpr uint32_t msr_gs_base = 0xc0000101;
        constexpr uint32_t msr_kernel_gs_base = 0xc0000102;

        constexpr uint64_t efer_sce = 1ull << 0;

        // CR0.WP: with it set, a ring 0 store respects a page's read-only bit.
        constexpr uint64_t cr0_wp = 1ull << 16;

        constexpr uint64_t cr4_la57 = 1ull << 12;
        constexpr uint64_t cr4_smep = 1ull << 20;
        constexpr uint64_t cr4_smap = 1ull << 21;

        constexpr uint64_t pte_present = 1ull << 0;
        constexpr uint64_t pte_user = 1ull << 2;
        constexpr uint64_t pte_large = 1ull << 7;
        constexpr uint64_t pte_addr_mask = 0x000ffffffffff000ull;

        // The four architecturally defined bits of a PE32+ section header this port reads.
        constexpr uint32_t scn_mem_discardable = 0x02000000u;
        constexpr uint32_t scn_mem_write = 0x80000000u;

        alignas(64) cpu_block g_cpu = {0, 0};

        static_assert(sizeof(cpu_block) == KICKOS_X86_64_CPU_SIZE,
                      "switch.S addresses the block with literal displacements");
        static_assert(offsetof(cpu_block, kernel_sp) == KICKOS_X86_64_CPU_KERNEL_SP,
                      "switch.S loads the kernel stack at CPU_KERNEL_SP");
        static_assert(offsetof(cpu_block, user_rsp) == KICKOS_X86_64_CPU_USER_RSP,
                      "switch.S parks the caller's stack pointer at CPU_USER_RSP");
        static_assert(KICKOS_X86_64_SEL_USER_CODE == sel_user_code,
                      "switch.S stamps the user code selector as an immediate");
        static_assert(KICKOS_X86_64_SEL_USER_DATA == sel_user_data,
                      "switch.S stamps the user stack selector as an immediate");

        // Bit 9 of the flag mask is the interrupt flag, and SYSCALL CLEARS every flag the
        // mask names (AMD APM Vol 3, SYSCALL). Without that bit the entry's first
        // instructions run with interrupts LIVE on a stack pointer the caller chose.
        constexpr uint64_t rflags_if = 1ull << 9;
        constexpr uint64_t fmask =
            (1ull << 8)      // trap: no single-step through the entry
            | rflags_if      // the one this step exists for
            | (1ull << 10)   // direction: the psABI wants it clear at a call boundary
            | (1ull << 12) | (1ull << 13) // I/O privilege level: 0 while privileged
            | (1ull << 14)   // nested task
            | (1ull << 18)   // alignment check
            | (1ull << 19) | (1ull << 20); // virtual interrupt, virtual interrupt pending
        static_assert((fmask & rflags_if) != 0,
                      "the flag mask must clear the interrupt flag or the syscall entry runs "
                      "interruptible on the caller's stack pointer");

        uintptr_t g_image_base = 0;
        uint32_t g_image_size = 0;
        uint8_t const* g_sections = nullptr;
        unsigned g_nsections = 0;

        unsigned g_granted = 0;
        unsigned g_already = 0;
        unsigned g_tables_exposed = 0;

        // The tables the grant walked, by physical address, distinct. Overflowing this is
        // REFUSED: a census over an incomplete record under-reports and reads as clean.
        constexpr unsigned max_tables_tracked = 64;
        uint64_t g_tables[max_tables_tracked] = {};
        unsigned g_tables_walked = 0;
        unsigned g_tables_dropped = 0;
        uint64_t g_control = 0;
        uint64_t g_control0 = 0;

        void invalidate(uintptr_t va)
        {
            __asm__ volatile("invlpg (%0)" ::"r"(va) : "memory");
        }

        [[noreturn]] void refuse(char const* what)
        {
            com1_puts("\nx86_64 ring3: ");
            com1_puts(what);
            com1_puts("\n");
            kfault_terminate();
        }

        void note_table(uint64_t pa)
        {
            for (unsigned i = 0; i < g_tables_walked; i++)
            {
                if (g_tables[i] == pa)
                {
                    return;
                }
            }
            if (g_tables_walked >= max_tables_tracked)
            {
                g_tables_dropped++;
                return;
            }
            g_tables[g_tables_walked] = pa;
            g_tables_walked++;
        }

        // Whether an unprivileged thread can reach `pa` through the live regime. Identity is
        // the adopted map's property, checked at aspace_init, so a table's own physical address
        // is the linear address to walk for.
        bool user_reachable(uint64_t pa, unsigned levels)
        {
            uint64_t table = read_cr3() & pte_addr_mask;
            for (unsigned level = levels; level >= 1; level--)
            {
                unsigned const shift = 12 + 9 * (level - 1);
                unsigned const index = static_cast<unsigned>((pa >> shift) & 0x1ffu);
                uint64_t const entry = reinterpret_cast<uint64_t const*>(table)[index];
                if ((entry & pte_present) == 0 or (entry & pte_user) == 0)
                {
                    return false;
                }
                if ((level == 1) or ((entry & pte_large) != 0))
                {
                    return true;
                }
                table = entry & pte_addr_mask;
            }
            return false;
        }

        // Set the user bit on every entry from the root to the leaf covering `va`, and return
        // the size of the leaf that was found so the caller can step past it. 0 says nothing
        // is mapped there, and the step is then the smallest level's span. The unit granted is
        // a LEAF, so a large leaf covering the end of a range exposes every byte to its end.
        uint64_t grant_one(uintptr_t va, unsigned levels)
        {
            uint64_t table = read_cr3() & pte_addr_mask;
            for (unsigned level = levels; level >= 1; level--)
            {
                note_table(table);
                unsigned const shift = 12 + 9 * (level - 1);
                unsigned const index = static_cast<unsigned>((va >> shift) & 0x1ffu);
                uint64_t* const entries = reinterpret_cast<uint64_t*>(table);
                uint64_t const entry = entries[index];
                if ((entry & pte_present) == 0)
                {
                    return 1ull << shift;
                }
                bool const leaf = (level == 1) or ((entry & pte_large) != 0);
                if ((entry & pte_user) != 0)
                {
                    if (leaf)
                    {
                        g_already++;
                        return 1ull << shift;
                    }
                }
                else
                {
                    entries[index] = entry | pte_user;
                    if (leaf)
                    {
                        g_granted++;
                    }
                }
                if (leaf)
                {
                    invalidate(va);
                    return 1ull << shift;
                }
                table = entry & pte_addr_mask;
            }
            return 4096;
        }

        void grant_range(uintptr_t lo, uintptr_t hi, unsigned levels)
        {
            uintptr_t va = lo & ~static_cast<uintptr_t>(4095);
            while (va < hi)
            {
                uint64_t const span = grant_one(va, levels);
                va = (va & ~static_cast<uintptr_t>(span - 1)) + static_cast<uintptr_t>(span);
            }
        }

        // The PE32+ headers, at the image's first byte because the loader maps them with it.
        void read_own_headers(void)
        {
            uint8_t const* const image = reinterpret_cast<uint8_t const*>(__ImageBase);
            g_image_base = reinterpret_cast<uintptr_t>(image);
            uint32_t const pe_offset = *reinterpret_cast<uint32_t const*>(image + 0x3c);
            uint8_t const* const pe = image + pe_offset;
            if (pe[0] != 'P' or pe[1] != 'E' or pe[2] != 0 or pe[3] != 0)
            {
                refuse("this image carries no PE signature, so it cannot describe itself");
            }
            g_nsections = *reinterpret_cast<uint16_t const*>(pe + 6);
            uint16_t const optional_size = *reinterpret_cast<uint16_t const*>(pe + 20);
            g_image_size = *reinterpret_cast<uint32_t const*>(pe + 24 + 56);
            g_sections = pe + 24 + optional_size;
        }
    }

    void ring3_init(uintptr_t ram_base, size_t ram_size)
    {
        uint64_t const cr4 = read_cr4();
        g_control = cr4;

        // Both are refused: supervisor-mode execution prevention would stop ring 0 fetching
        // from the pages granted below, which on a flat image is the kernel's own text, and
        // supervisor-mode access prevention would stop it reading a user buffer.
        if ((cr4 & cr4_smep) != 0)
        {
            refuse("supervisor-mode execution prevention is on and this image is one flat link");
        }
        if ((cr4 & cr4_smap) != 0)
        {
            refuse("supervisor-mode access prevention is on and no path here lifts it");
        }

        uint32_t cpuid_a = 0;
        uint32_t cpuid_b = 0;
        uint32_t cpuid_c = 0;
        uint32_t cpuid_d = 0;
        __asm__ volatile("cpuid"
                         : "=a"(cpuid_a), "=b"(cpuid_b), "=c"(cpuid_c), "=d"(cpuid_d)
                         : "a"(0x80000001u), "c"(0u));
        if ((cpuid_d & (1u << 11)) == 0)
        {
            refuse("this processor reports no SYSCALL/SYSRET");
        }

        read_own_headers();

        // The level count is read from the control register, 4 or 5.
        unsigned levels = 4;
        if ((cr4 & cr4_la57) != 0)
        {
            levels = 5;
        }
        // The firmware write-protects its own translation tables against ring 0: they are
        // mapped read-only and CR0.WP is set, so an edit without this takes a write page fault
        // at ring 0 on the root itself. WP is cleared for the edits and put back.
        uint64_t const cr0 = read_cr0();
        g_control0 = cr0;
        if ((cr0 & cr0_wp) != 0)
        {
            write_cr0(cr0 & ~cr0_wp);
        }
        grant_range(g_image_base, g_image_base + g_image_size, levels);
        if (ram_size != 0)
        {
            grant_range(ram_base, ram_base + ram_size, levels);
        }
        if ((cr0 & cr0_wp) != 0)
        {
            write_cr0(cr0);
        }

        // The exposure census, after every grant and over every table the grant walked. Each is
        // asked of the HARDWARE, by the same rule the processor applies.
        if (g_tables_dropped != 0)
        {
            refuse("more translation tables were walked than the exposure census records");
        }
        for (unsigned i = 0; i < g_tables_walked; i++)
        {
            if (user_reachable(g_tables[i], levels))
            {
                g_tables_exposed++;
            }
        }

        // The gs pair. IA32_KERNEL_GS_BASE holds the per-core pointer while a thread runs at
        // ring 3 and swapgs is what brings it back; WRMSR is privileged, so ring 3 can change
        // the base it is holding but never the one the entry gets.
        write_msr(msr_gs_base, reinterpret_cast<uint64_t>(&g_cpu));
        write_msr(msr_kernel_gs_base, 0);

        // IA32_STAR carries both instructions' selector bases: bits 47:32 for SYSCALL and
        // bits 63:48 for SYSRET, which reads SS from that field plus 8 and CS from plus 16.
        uint64_t const star = (static_cast<uint64_t>((sel_user_data & ~3u) - 8) << 48)
                              | (static_cast<uint64_t>(sel_kernel_code) << 32);
        write_msr(msr_star, star);
        write_msr(msr_lstar, reinterpret_cast<uint64_t>(&kickos_x86_64_syscall_entry));
        write_msr(msr_fmask, fmask);

        // LAST, so no SYSCALL can be executed before the three registers above name where it
        // goes and what it masks.
        write_msr(msr_efer, read_msr(msr_efer) | efer_sce);
    }

    void cpu_set_kernel_sp(uint64_t top)
    {
        g_cpu.kernel_sp = top;
    }

    uint64_t cpu_kernel_sp(void)
    {
        return g_cpu.kernel_sp;
    }

    bool image_range_mapped(uintptr_t ptr, size_t len, bool need_write)
    {
        if (len == 0)
        {
            return true;
        }
        if (g_sections == nullptr)
        {
            return false;
        }
        uintptr_t const end = ptr + len;
        if (end < ptr)
        {
            return false;
        }
        if (ptr < g_image_base or end > g_image_base + g_image_size)
        {
            return false;
        }
        for (unsigned i = 0; i < g_nsections; i++)
        {
            uint8_t const* const s = g_sections + i * 40u;
            uint32_t const characteristics = *reinterpret_cast<uint32_t const*>(s + 36);
            // A discardable section is one the loader is free not to map at all, so it is
            // not part of what this image can promise is there.
            if ((characteristics & scn_mem_discardable) != 0)
            {
                continue;
            }
            uint32_t const virtual_size = *reinterpret_cast<uint32_t const*>(s + 8);
            uint32_t const rva = *reinterpret_cast<uint32_t const*>(s + 12);
            uint32_t const raw_size = *reinterpret_cast<uint32_t const*>(s + 16);
            uint32_t span = virtual_size;
            if (raw_size > span)
            {
                span = raw_size;
            }
            uintptr_t const lo = g_image_base + rva;
            if (ptr < lo or end > lo + span)
            {
                continue;
            }
            if (need_write and (characteristics & scn_mem_write) == 0)
            {
                return false;
            }
            return true;
        }
        return false;
    }

    uintptr_t image_base(void)
    {
        return g_image_base;
    }

    size_t image_size(void)
    {
        return g_image_size;
    }

    unsigned image_sections(void)
    {
        return g_nsections;
    }

    unsigned user_leaves_granted(void)
    {
        return g_granted;
    }

    unsigned user_leaves_already(void)
    {
        return g_already;
    }

    unsigned user_tables_exposed(void)
    {
        return g_tables_exposed;
    }

    unsigned user_tables_walked(void)
    {
        return g_tables_walked;
    }

    uint64_t control_flags(void)
    {
        return g_control;
    }

    uint64_t control_flags0(void)
    {
        return g_control0;
    }
}
