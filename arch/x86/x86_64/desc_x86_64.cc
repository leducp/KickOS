// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// desc_init runs AFTER ExitBootServices; replacing the tables earlier takes the ground out
// from under the firmware's own calls. CR3 is untouched here, paging being adopted as handed
// over.

#include <kickos/arch/desc.h>
#include <kickos/arch/portio.h>
#include <kickos/arch/trap.h>
#include <kickos/chip_com1.h>

#include <stdint.h>

namespace kickos::x86_64
{
    namespace
    {
        using namespace kickos::q35;

        // Index into the GDT, in 8-byte entries. The task-state segment descriptor is a
        // system descriptor and takes two of them in long mode.
        constexpr unsigned gdt_index_null = 0;
        constexpr unsigned gdt_index_kernel_code = 1;
        constexpr unsigned gdt_index_kernel_data = 2;
        constexpr unsigned gdt_index_user_data = 3;
        constexpr unsigned gdt_index_user_code = 4;
        constexpr unsigned gdt_index_tss = 5;
        constexpr unsigned gdt_entries = 7;

        static_assert(sel_kernel_code == gdt_index_kernel_code * 8, "selector is a byte offset");
        static_assert((sel_kernel_data & ~3u) == gdt_index_kernel_data * 8, "selector is a byte offset");
        static_assert((sel_user_data & ~3u) == gdt_index_user_data * 8, "selector is a byte offset");
        static_assert((sel_user_code & ~3u) == gdt_index_user_code * 8, "selector is a byte offset");
        static_assert(sel_tss == gdt_index_tss * 8, "selector is a byte offset");

        constexpr unsigned vector_double_fault = 8;
        constexpr unsigned vector_nmi = 2;
        constexpr unsigned vector_machine_check = 18;

        // access byte: present, the descriptor privilege level, code-or-data, then the type
        // nibble.
        constexpr uint8_t access_code = 0x9a;
        constexpr uint8_t access_data = 0x92;
        constexpr uint8_t access_code_user = 0xfa;
        constexpr uint8_t access_data_user = 0xf2;
        // present, ring 0, system, type 9 = available 64-bit task-state segment.
        constexpr uint8_t access_tss = 0x89;

        // flags byte: granularity, default-size, long-mode, available, limit 19:16.
        constexpr uint8_t flags_code64 = 0xaf;
        constexpr uint8_t flags_data = 0xcf;

        // present, ring 0, type 14 = 64-bit interrupt gate, which clears the interrupt flag
        // on entry.
        constexpr uint8_t gate_interrupt = 0x8e;

        constexpr unsigned stack_bytes = 8192;

        constexpr unsigned nmi_stack_bytes = 4096;

        alignas(16) uint8_t g_kernel_stack[stack_bytes];
        alignas(16) uint8_t g_fault_stack[stack_bytes];
        alignas(16) uint8_t g_nmi_stack[nmi_stack_bytes];
        alignas(16) uint8_t g_mce_stack[nmi_stack_bytes];

        alignas(16) gdt_entry g_gdt[gdt_entries];
        alignas(16) tss64 g_tss;
        alignas(16) idt_gate g_idt[idt_vectors];

        struct __attribute__((packed)) tss_desc_hi
        {
            uint32_t base_hi;
            uint32_t reserved;
        };

        void set_entry(unsigned index, uint8_t access, uint8_t flags)
        {
            g_gdt[index].limit_lo = 0xffff;
            g_gdt[index].base_lo = 0;
            g_gdt[index].base_mid = 0;
            g_gdt[index].access = access;
            g_gdt[index].flags_limit_hi = flags;
            g_gdt[index].base_hi = 0;
        }

        void clear_entry(unsigned index)
        {
            g_gdt[index].limit_lo = 0;
            g_gdt[index].base_lo = 0;
            g_gdt[index].base_mid = 0;
            g_gdt[index].access = 0;
            g_gdt[index].flags_limit_hi = 0;
            g_gdt[index].base_hi = 0;
        }

        void build_tss(void)
        {
            g_tss.reserved0 = 0;
            g_tss.rsp0 = reinterpret_cast<uint64_t>(g_kernel_stack) + stack_bytes;
            g_tss.rsp1 = 0;
            g_tss.rsp2 = 0;
            g_tss.reserved1 = 0;
            for (unsigned i = 0; i < 7; ++i)
            {
                g_tss.ist[i] = 0;
            }
            g_tss.ist[ist_double_fault - 1] =
                reinterpret_cast<uint64_t>(g_fault_stack) + stack_bytes;
            g_tss.ist[ist_nmi - 1] =
                reinterpret_cast<uint64_t>(g_nmi_stack) + nmi_stack_bytes;
            g_tss.ist[ist_machine_check - 1] =
                reinterpret_cast<uint64_t>(g_mce_stack) + nmi_stack_bytes;
            g_tss.reserved2 = 0;
            g_tss.reserved3 = 0;
            // Past the end of the segment: no I/O permission bitmap, so port access from
            // anything but ring 0 is refused.
            g_tss.iomap_base = static_cast<uint16_t>(sizeof(tss64));
        }

        void build_tss_descriptor(void)
        {
            uint64_t const base = reinterpret_cast<uint64_t>(&g_tss);
            g_gdt[gdt_index_tss].limit_lo = static_cast<uint16_t>(sizeof(tss64) - 1);
            g_gdt[gdt_index_tss].base_lo = static_cast<uint16_t>(base & 0xffff);
            g_gdt[gdt_index_tss].base_mid = static_cast<uint8_t>((base >> 16) & 0xff);
            g_gdt[gdt_index_tss].access = access_tss;
            g_gdt[gdt_index_tss].flags_limit_hi = 0;
            g_gdt[gdt_index_tss].base_hi = static_cast<uint8_t>((base >> 24) & 0xff);

            tss_desc_hi* const hi = reinterpret_cast<tss_desc_hi*>(&g_gdt[gdt_index_tss + 1]);
            hi->base_hi = static_cast<uint32_t>(base >> 32);
            hi->reserved = 0;
        }

        void build_idt(void)
        {
            uintptr_t const stubs = reinterpret_cast<uintptr_t>(kickos_x86_64_vector_base);
            for (unsigned v = 0; v < idt_vectors; ++v)
            {
                uintptr_t const handler = stubs + kickos_x86_64_vector_offsets[v];
                g_idt[v].offset_lo = static_cast<uint16_t>(handler & 0xffff);
                g_idt[v].selector = sel_kernel_code;
                g_idt[v].ist = 0;
                if (v == vector_double_fault)
                {
                    g_idt[v].ist = static_cast<uint8_t>(ist_double_fault);
                }
                if (v == vector_nmi)
                {
                    g_idt[v].ist = static_cast<uint8_t>(ist_nmi);
                }
                if (v == vector_machine_check)
                {
                    g_idt[v].ist = static_cast<uint8_t>(ist_machine_check);
                }
                g_idt[v].type_attr = gate_interrupt;
                g_idt[v].offset_mid = static_cast<uint16_t>((handler >> 16) & 0xffff);
                g_idt[v].offset_hi = static_cast<uint32_t>(handler >> 32);
                g_idt[v].reserved = 0;
            }
        }

        void load_gdt(void)
        {
            desc_ptr p;
            p.limit = static_cast<uint16_t>(sizeof(g_gdt) - 1);
            p.base = reinterpret_cast<uint64_t>(&g_gdt);
            __asm__ volatile("lgdt %0" ::"m"(p) : "memory");

            // The code selector is not writable, so it is reloaded through a far return. The
            // target is PC-relative: this image carries no base relocations.
            __asm__ volatile("leaq 1f(%%rip), %%rax\n\t"
                             "pushq %0\n\t"
                             "pushq %%rax\n\t"
                             "lretq\n\t"
                             "1:"
                             ::"i"(static_cast<uint64_t>(sel_kernel_code))
                             : "rax", "memory");

            uint16_t const data = sel_kernel_data;
            __asm__ volatile("movw %0, %%ds\n\t"
                             "movw %0, %%es\n\t"
                             "movw %0, %%ss"
                             ::"r"(data)
                             : "memory");
            // A null selector zeroes both bases. ring3_init then writes the gs pair by MSR,
            // the only writer of the per-core pointer the syscall entry reads.
            uint16_t const none = sel_null;
            __asm__ volatile("movw %0, %%fs\n\t"
                             "movw %0, %%gs"
                             ::"r"(none)
                             : "memory");
        }

        void load_idt(void)
        {
            desc_ptr p;
            p.limit = static_cast<uint16_t>(sizeof(g_idt) - 1);
            p.base = reinterpret_cast<uint64_t>(&g_idt);
            __asm__ volatile("lidt %0" ::"m"(p) : "memory");
        }
    }

    void desc_init(void)
    {
        clear_entry(gdt_index_null);
        set_entry(gdt_index_kernel_code, access_code, flags_code64);
        set_entry(gdt_index_kernel_data, access_data, flags_data);
        set_entry(gdt_index_user_data, access_data_user, flags_data);
        set_entry(gdt_index_user_code, access_code_user, flags_code64);
        build_tss();
        build_tss_descriptor();
        build_idt();

        load_gdt();

        uint16_t const tr = sel_tss;
        __asm__ volatile("ltr %0" ::"r"(tr) : "memory");

        load_idt();
    }

    desc_ptr read_gdtr(void)
    {
        desc_ptr p;
        p.limit = 0;
        p.base = 0;
        __asm__ volatile("sgdt %0" : "=m"(p)::"memory");
        return p;
    }

    desc_ptr read_idtr(void)
    {
        desc_ptr p;
        p.limit = 0;
        p.base = 0;
        __asm__ volatile("sidt %0" : "=m"(p)::"memory");
        return p;
    }

    uint16_t read_tr(void)
    {
        uint16_t v = 0;
        __asm__ volatile("str %0" : "=r"(v));
        return v;
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

    uintptr_t gdt_address(void)
    {
        return reinterpret_cast<uintptr_t>(&g_gdt);
    }

    uintptr_t idt_address(void)
    {
        return reinterpret_cast<uintptr_t>(&g_idt);
    }

    uintptr_t tss_address(void)
    {
        return reinterpret_cast<uintptr_t>(&g_tss);
    }

    uint64_t tss_rsp0(void)
    {
        return g_tss.rsp0;
    }

    void tss_set_rsp0(uint64_t top)
    {
        g_tss.rsp0 = top;
    }

    uint64_t tss_ist(unsigned slot)
    {
        if (slot < 1 or slot > 7)
        {
            return 0;
        }
        return g_tss.ist[slot - 1];
    }

    uintptr_t fault_stack_lo(void)
    {
        return reinterpret_cast<uintptr_t>(g_fault_stack);
    }

    uintptr_t fault_stack_hi(void)
    {
        return reinterpret_cast<uintptr_t>(g_fault_stack) + stack_bytes;
    }

    void desc_report(void)
    {
        desc_ptr const gdtr = read_gdtr();
        desc_ptr const idtr = read_idtr();

        com1_puts("  gdt=");
        com1_hex64(gdt_address());
        com1_puts(" gdtr=");
        com1_hex64(gdtr.base);
        com1_puts(" limit=");
        com1_hex64(gdtr.limit);
        com1_puts("\n");

        com1_puts("  idt=");
        com1_hex64(idt_address());
        com1_puts(" idtr=");
        com1_hex64(idtr.base);
        com1_puts(" limit=");
        com1_hex64(idtr.limit);
        com1_puts("\n");

        com1_puts("  tss=");
        com1_hex64(tss_address());
        com1_puts(" tr=");
        com1_hex64(read_tr());
        com1_puts(" rsp0=");
        com1_hex64(tss_rsp0());
        com1_puts(" ist1=");
        com1_hex64(tss_ist(ist_double_fault));
        com1_puts("\n");

        com1_puts("  cs=");
        com1_hex64(read_cs());
        com1_puts(" ss=");
        com1_hex64(read_ss());
        com1_puts("\n");
    }
}
