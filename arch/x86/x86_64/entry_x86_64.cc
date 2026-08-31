// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// x86_64: the UEFI handover. Firmware calls efi_main in long mode with paging already enabled
// and interrupts ENABLED (UEFI 2.11 section 2.3.4), so the first instruction here is `cli`.
// kickos_x86_64_landed is the per-image tail and runs after ExitBootServices.

#include <kickos/arch/desc.h>
#include <kickos/arch/regs.h>
#include <kickos/arch/uefi.h>
#include <kickos/chip_com1.h>

#include <stddef.h>
#include <stdint.h>

// The gate body every vector of the stopgap table below names. The visibility attribute keeps
// the address PC-relative, an image linked by `ld -m i386pep` carrying no global offset table.
extern "C" __attribute__((visibility("hidden"))) void kickos_x86_64_early_trap(void);

// The image-specific tail, carrying the largest conventional-memory range the map named. This
// is the last moment those bounds can be taken.
extern "C" void kickos_x86_64_landed(uintptr_t ram_base, uint64_t ram_size);

// tools/run-qemu-x86_64.sh greps for this string; move both or the arm fails.
#define KICKOS_X1_TOKEN "KICKOS-X1 8c41d7a2 x86_64/q35 uefi-handover"

namespace
{
    using namespace kickos::q35;
    using namespace kickos::uefi;
    using namespace kickos::x86_64;

    // A static buffer: an AllocatePool CHANGES the memory map and invalidates the map key
    // ExitBootServices must be given.
    constexpr uint64_t map_buffer_bytes = 32768;
    alignas(16) uint8_t g_map_buffer[map_buffer_bytes];

    uint64_t read_rflags(void)
    {
        uint64_t flags = 0;
        __asm__ volatile("pushfq\n\tpop %0" : "=r"(flags) : : "memory");
        return flags;
    }

    // RFLAGS.IF, bit 9.
    unsigned interrupt_flag(void)
    {
        return static_cast<unsigned>((read_rflags() >> 9) & 1u);
    }

    void report_if(char const* stage)
    {
        com1_puts(stage);
        com1_dec(interrupt_flag());
        com1_puts("\n");
    }

    constexpr uint32_t msr_efer = 0xc0000080u;

    // CPUID 0x80000001, EDX bit 20: the execute-disable bit's presence. The extended leaf need
    // not exist, so its own maximum is read first.
    unsigned nx_capable(void)
    {
        uint32_t a = 0;
        uint32_t b = 0;
        uint32_t c = 0;
        uint32_t d = 0;
        __asm__ volatile("cpuid"
                         : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(0x80000000u), "c"(0u));
        if (a < 0x80000001u)
        {
            return 0;
        }
        __asm__ volatile("cpuid"
                         : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(0x80000001u), "c"(0u));
        if ((d & (1u << 20)) == 0)
        {
            return 0;
        }
        return 1;
    }

    // tools/run-qemu-x86_64.sh asserts the if, paging, longmode, root and nxcap fields of this
    // line. wp and nx are this firmware's own choices and a firmware leaving either clear still
    // conforms.
    void report_handover(uint64_t rflags, uint64_t cr0, uint64_t cr3, uint32_t efer)
    {
        com1_puts(KICKOS_X1_TOKEN " handover if=");
        com1_dec(static_cast<unsigned>((rflags >> 9) & 1u));
        com1_puts(" paging=");
        com1_dec(static_cast<unsigned>((cr0 >> 31) & 1u));
        com1_puts(" longmode=");
        com1_dec(static_cast<unsigned>((efer >> 10) & 1u));
        com1_puts(" root=");
        unsigned root_live = 0;
        if (cr3 != 0)
        {
            root_live = 1;
        }
        com1_dec(root_live);
        com1_puts(" wp=");
        com1_dec(static_cast<unsigned>((cr0 >> 16) & 1u));
        com1_puts(" nx=");
        com1_dec(static_cast<unsigned>((efer >> 11) & 1u));
        com1_puts(" nxcap=");
        com1_dec(nx_capable());
        com1_puts("\n");
    }

    void print_vendor(char16_t const* vendor)
    {
        com1_puts(KICKOS_X1_TOKEN " firmware=");
        if (vendor == nullptr)
        {
            com1_puts("(none)");
        }
        else
        {
            uint32_t n = 0;
            // The bound FIRST: the other order reads element 64 before deciding it is out
            // of range.
            while (n < 64 and vendor[n] != 0)
            {
                char c = '?';
                if (vendor[n] >= 0x20 and vendor[n] < 0x7f)
                {
                    c = static_cast<char>(vendor[n]);
                }
                com1_putc(c);
                ++n;
            }
        }
        com1_puts("\n");
    }

    // Below the first mebibyte the map still names conventional pages this port must not take:
    // that window holds the legacy structures firmware and the platform keep.
    constexpr uint64_t ram_floor = 0x100000;
    // The map editor's large-leaf size, which the arena's base is rounded up to: arch_ram_alloc
    // aligns a region to its own size.
    constexpr uint64_t ram_align = 0x200000;

    memory_descriptor const* descriptor_at(uint64_t offset)
    {
        return reinterpret_cast<memory_descriptor const*>(g_map_buffer + offset);
    }

    // The half-open range a descriptor names, and false where this port cannot take it. The
    // page count is the FIRMWARE's figure and the multiply by the page size can wrap, which
    // would come back as a small run at a huge address and win the pick below.
    bool conventional_span(memory_descriptor const* d, uint64_t* lo, uint64_t* hi)
    {
        if (d->type != memory_conventional or d->page_count == 0)
        {
            return false;
        }
        if (d->page_count > (UINT64_MAX / 4096))
        {
            return false;
        }
        uint64_t const bytes = d->page_count * 4096;
        if (d->physical_start > UINT64_MAX - bytes)
        {
            return false;
        }
        *lo = d->physical_start;
        *hi = d->physical_start + bytes;
        return true;
    }

    // The arena is the largest contiguous conventional SPAN, firmware splitting conventional
    // memory wherever an attribute changes. UEFI does not promise a sorted map, so each pass
    // rescans for a run starting exactly where the span ends. A run straddling the floor is
    // CLAMPED to it, a firmware presenting one run from zero to the top of memory otherwise
    // leaving the arena empty.
    bool pick_arena(uint64_t map_size, uint64_t descriptor_size, uintptr_t* base, uint64_t* size)
    {
        uint64_t best_lo = 0;
        uint64_t best_hi = 0;
        for (uint64_t seed = 0; seed + descriptor_size <= map_size; seed += descriptor_size)
        {
            uint64_t lo = 0;
            uint64_t hi = 0;
            if (not conventional_span(descriptor_at(seed), &lo, &hi))
            {
                continue;
            }
            bool grew = true;
            while (grew)
            {
                grew = false;
                for (uint64_t o = 0; o + descriptor_size <= map_size; o += descriptor_size)
                {
                    uint64_t nlo = 0;
                    uint64_t nhi = 0;
                    if (not conventional_span(descriptor_at(o), &nlo, &nhi))
                    {
                        continue;
                    }
                    if (nlo == hi and nhi > hi)
                    {
                        hi = nhi;
                        grew = true;
                    }
                }
            }
            if (lo < ram_floor)
            {
                lo = ram_floor;
            }
            if (lo > UINT64_MAX - (ram_align - 1))
            {
                continue;
            }
            lo = (lo + (ram_align - 1)) & ~(ram_align - 1);
            if (hi <= lo)
            {
                continue;
            }
            if (hi - lo > best_hi - best_lo)
            {
                best_lo = lo;
                best_hi = hi;
            }
        }
        if (best_hi <= best_lo)
        {
            return false;
        }
        *base = static_cast<uintptr_t>(best_lo);
        *size = best_hi - best_lo;
        return true;
    }

    [[noreturn]] void halt(void)
    {
        while (true)
        {
            __asm__ volatile("cli\n\thlt");
        }
    }

    unsigned bit_of(uint64_t v, unsigned n)
    {
        return static_cast<unsigned>((v >> n) & 1u);
    }

    // CR0 bits 1, 2 and 3 and CR4 bits 9, 10 and 18 (Intel SDM Vol 3 section 2.5).
    constexpr uint64_t cr0_mp = 1ull << 1;
    constexpr uint64_t cr0_em = 1ull << 2;
    constexpr uint64_t cr0_ts = 1ull << 3;
    constexpr uint64_t cr4_osfxsr = 1ull << 9;
    constexpr uint64_t cr4_osxmmexcpt = 1ull << 10;
    constexpr uint64_t cr4_osxsave = 1ull << 18;

    void report_fp(char const* stage, uint64_t cr0, uint64_t cr4)
    {
        com1_puts(stage);
        com1_puts(" em=");
        com1_dec(bit_of(cr0, 2));
        com1_puts(" ts=");
        com1_dec(bit_of(cr0, 3));
        com1_puts(" mp=");
        com1_dec(bit_of(cr0, 1));
        com1_puts(" osfxsr=");
        com1_dec(bit_of(cr4, 9));
        com1_puts(" osxmmexcpt=");
        com1_dec(bit_of(cr4, 10));
        com1_puts(" osxsave=");
        com1_dec(bit_of(cr4, 18));
        com1_puts("\n");
    }

    // The port saves no x87, MMX, vector or extended state, so these six bits refuse every
    // instruction that touches any of it. -mno-sse -mno-mmx -mno-80387 bind the COMPILER; these
    // bits bind the machine, against a thread executing a raw opcode.
    //
    // Installed AFTER ExitBootServices: firmware's own code runs with vector state of its own.
    void fp_trap_install(void)
    {
        uint64_t cr0 = read_cr0();
        uint64_t cr4 = read_cr4();
        report_fp(KICKOS_X1_TOKEN " fp found", cr0, cr4);

        cr0 |= cr0_em | cr0_ts | cr0_mp;
        cr4 &= ~(cr4_osfxsr | cr4_osxmmexcpt | cr4_osxsave);
        write_cr0(cr0);
        write_cr4(cr4);

        // READ BACK, so the line below reports the machine's answer and not the value asked
        // for: both registers hold bits the processor may refuse to change.
        cr0 = read_cr0();
        cr4 = read_cr4();
        report_fp(KICKOS_X1_TOKEN " fp trapped", cr0, cr4);
    }

    // The interrupt table this port owns between ExitBootServices and desc_init: firmware's
    // table names handlers in storage whose lifetime ended with that call, and `cli` holds off
    // neither the non-maskable interrupt nor the machine check. Thirty two gates, the vectors
    // the processor itself raises; a vector above them takes a general-protection fault against
    // this limit. The selector is the LIVE cs, firmware's descriptor, this code already running
    // under it.
    constexpr unsigned early_vectors = 32;
    // present, ring 0, type 14 = 64-bit interrupt gate.
    constexpr uint8_t early_gate_interrupt = 0x8e;
    alignas(16) kickos::x86_64::idt_gate g_early_idt[early_vectors];

    void early_idt_load(void)
    {
        uint16_t cs = 0;
        __asm__ volatile("movw %%cs, %0" : "=r"(cs));
        uintptr_t const handler = reinterpret_cast<uintptr_t>(&kickos_x86_64_early_trap);
        for (unsigned v = 0; v < early_vectors; ++v)
        {
            g_early_idt[v].offset_lo = static_cast<uint16_t>(handler & 0xffff);
            g_early_idt[v].selector = cs;
            g_early_idt[v].ist = 0;
            g_early_idt[v].type_attr = early_gate_interrupt;
            g_early_idt[v].offset_mid = static_cast<uint16_t>((handler >> 16) & 0xffff);
            g_early_idt[v].offset_hi = static_cast<uint32_t>(handler >> 32);
            g_early_idt[v].reserved = 0;
        }
        kickos::x86_64::desc_ptr p;
        p.limit = static_cast<uint16_t>(sizeof(g_early_idt) - 1);
        p.base = reinterpret_cast<uint64_t>(&g_early_idt);
        __asm__ volatile("lidt %0" ::"m"(p) : "memory");
    }
}

// Entered with the hardware frame on whatever stack was live. It reports and halts, so the
// frame is never unwound.
extern "C" [[noreturn]] __attribute__((visibility("hidden"))) void
kickos_x86_64_early_trap_report(void)
{
    com1_puts("\n" KICKOS_X1_TOKEN " FAIL trap before the descriptor tables were installed\n");
    halt();
}

// The direction flag is cleared because delivery through an interrupt gate leaves it as the
// interrupted code set it (Intel SDM Vol 3 section 7.12.1.3) and the reporter is compiled to
// the SysV convention.
__asm__(".text\n"
        ".balign 16\n"
        ".globl kickos_x86_64_early_trap\n"
        "kickos_x86_64_early_trap:\n"
        "    cld\n"
        "    call kickos_x86_64_early_trap_report\n");

extern "C" KICKOS_EFIAPI status_t efi_main(handle_t image_handle, system_table* systab)
{
    // Read BEFORE the cli, which is the last moment the flag firmware handed over is visible.
    // Two volatile asm statements cannot be reordered against each other.
    uint64_t rflags_in = 0;
    __asm__ volatile("pushfq\n\tpop %0" : "=r"(rflags_in) : : "memory");
    uint64_t const cr0_in = read_cr0_ordered();
    uint64_t const cr3_in = read_cr3_ordered();
    uint32_t const efer_lo = read_msr_low_ordered(msr_efer);

    __asm__ volatile("cli");

    com1_init();
    com1_puts("\n" KICKOS_X1_TOKEN " entry\n");
    report_handover(rflags_in, cr0_in, cr3_in, efer_lo);
    report_if(KICKOS_X1_TOKEN " if_after_cli=");

    if (systab == nullptr or systab->boot_services == nullptr)
    {
        com1_puts(KICKOS_X1_TOKEN " FAIL no boot services table\n");
        halt();
    }
    print_vendor(systab->firmware_vendor);
    com1_puts(KICKOS_X1_TOKEN " revision=");
    com1_hex64(systab->hdr.revision);
    com1_puts("\n");

    boot_services* bs = systab->boot_services;

    uint64_t map_size = map_buffer_bytes;
    uint64_t map_key = 0;
    uint64_t descriptor_size = 0;
    uint32_t descriptor_version = 0;
    memory_descriptor* map = reinterpret_cast<memory_descriptor*>(g_map_buffer);
    status_t st = bs->get_memory_map(&map_size, map, &map_key, &descriptor_size,
                                     &descriptor_version);
    if (st != status_success)
    {
        com1_puts(KICKOS_X1_TOKEN " FAIL get_memory_map=");
        com1_hex64(st);
        com1_puts("\n");
        halt();
    }
    if (descriptor_size == 0)
    {
        com1_puts(KICKOS_X1_TOKEN " FAIL descriptor_size=0\n");
        halt();
    }

    uint64_t descriptors = map_size / descriptor_size;
    uint64_t conventional_pages = 0;
    uint64_t offset = 0;
    while (offset + descriptor_size <= map_size)
    {
        memory_descriptor const* d = descriptor_at(offset);
        if (d->type == memory_conventional)
        {
            conventional_pages += d->page_count;
        }
        offset += descriptor_size;
    }
    uintptr_t ram_base = 0;
    uint64_t ram_size = 0;
    bool const have_ram = pick_arena(map_size, descriptor_size, &ram_base, &ram_size);

    com1_puts(KICKOS_X1_TOKEN " map descriptors=");
    com1_dec(descriptors);
    com1_puts(" stride=");
    com1_dec(descriptor_size);
    com1_puts(" version=");
    com1_dec(descriptor_version);
    com1_puts(" conventional_pages=");
    com1_dec(conventional_pages);
    com1_puts(" arena=");
    com1_hex64(ram_base);
    com1_puts(" arena_pages=");
    com1_dec(ram_size / 4096);
    com1_puts("\n");

    // An empty arena is a failed handover: every image below this line publishes it as the RAM
    // seam's whole answer.
    if (not have_ram)
    {
        com1_puts(KICKOS_X1_TOKEN " FAIL no conventional run this port can take\n");
        halt();
    }

    st = bs->exit_boot_services(image_handle, map_key);
    if (st != status_success)
    {
        com1_puts(KICKOS_X1_TOKEN " FAIL exit_boot_services=");
        com1_hex64(st);
        com1_puts("\n");
        halt();
    }

    // The FIRST two things after the handover ends: the interrupt table firmware left behind
    // names storage whose lifetime ended with the call above, and the vector state it enabled
    // is state this port does not save.
    early_idt_load();
    fp_trap_install();

    com1_puts(KICKOS_X1_TOKEN " boot services left\n");
    report_if(KICKOS_X1_TOKEN " if_after_exit=");

    kickos_x86_64_landed(ram_base, ram_size);

    com1_puts(KICKOS_X1_TOKEN " landed, halting\n");
    halt();
}
