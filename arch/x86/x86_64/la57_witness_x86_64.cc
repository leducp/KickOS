// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A witness image's paging entry: where firmware left four-level paging live on a processor that
// offers five, the kernel moves to five levels before it adopts the regime, as firmware that
// enables them leaves it: a five-level root whose first entry is firmware's four-level root.
// CR4.LA57 changes only with paging off, so the switch leaves long mode through a 32-bit code
// segment and comes back; the code, the stack and the root must then sit below 4 GiB, which an
// image at its preferred base does (Intel SDM Vol 3, section 10.8.5).

#include <kickos/arch/regs.h>
#include <kickos/chip_com1.h>

#include <stddef.h>
#include <stdint.h>

extern "C"
{
    void kfault_terminate(void) __attribute__((noreturn));
    // Leaves long mode, sets CR4.LA57 and CR3 to `root`, re-enters long mode (below).
    void kickos_x86_64_la57_switch(uint32_t root, void const* gdtr);
}

namespace
{
    constexpr uint64_t CR4_PCIDE = 1ull << 17;
    constexpr uint64_t CR4_LA57 = 1ull << 12;
    constexpr uint32_t CPUID_7_ECX_LA57 = 1u << 16;
    constexpr uint64_t PTE_P = 1ull << 0;
    constexpr uint64_t PTE_RW = 1ull << 1;
    constexpr uint64_t PTE_ADDR_MASK = 0x000ffffffffff000ull;
    constexpr uint64_t BELOW_4G = 1ull << 32;

    alignas(4096) uint64_t g_root5[512] = {};

    // Null, 64-bit code at 0x08, 32-bit code at 0x10, data at 0x18; arch_init loads the
    // kernel's own.
    alignas(16) uint64_t g_gdt[4] = {0, 0x00af9a000000ffffull, 0x00cf9a000000ffffull,
                                     0x00cf92000000ffffull};
    struct __attribute__((packed)) Gdtr
    {
        uint16_t limit;
        uint64_t base;
    };
    Gdtr g_gdtr = {};

    [[noreturn]] void refuse(char const* what)
    {
        kickos::q35::com1_puts("\nx86_64 la57 witness: ");
        kickos::q35::com1_puts(what);
        kickos::q35::com1_puts("\n");
        kfault_terminate();
    }

    bool offers_la57(void)
    {
        uint32_t a = 0;
        uint32_t b = 0;
        uint32_t c = 0;
        uint32_t d = 0;
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0u), "c"(0u));
        if (a < 7u)
        {
            return false;
        }
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7u), "c"(0u));
        return (c & CPUID_7_ECX_LA57) != 0;
    }
}

extern "C" void kickos_x86_64_paging_entry(void)
{
    if ((kickos::x86_64::read_cr4() & CR4_LA57) != 0)
    {
        kickos::q35::com1_puts("x86_64 la57 witness: firmware left five-level paging live\n");
        return;
    }
    if (not offers_la57())
    {
        refuse("this processor offers no five-level paging");
    }
    if ((kickos::x86_64::read_cr4() & CR4_PCIDE) != 0)
    {
        refuse("PCIDs are on, and paging cannot be turned off under them");
    }
    uintptr_t const sp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
    uintptr_t const code = reinterpret_cast<uintptr_t>(&kickos_x86_64_la57_switch);
    uintptr_t const root = reinterpret_cast<uintptr_t>(g_root5);
    if (sp >= BELOW_4G or code >= BELOW_4G or root >= BELOW_4G)
    {
        refuse("the image is not below 4 GiB, so it cannot run with paging off");
    }
    g_root5[0] = (kickos::x86_64::read_cr3() & PTE_ADDR_MASK) | PTE_P | PTE_RW;
    g_gdtr.limit = static_cast<uint16_t>(sizeof(g_gdt) - 1u);
    g_gdtr.base = reinterpret_cast<uint64_t>(g_gdt);
    kickos_x86_64_la57_switch(static_cast<uint32_t>(root), &g_gdtr);
    if ((kickos::x86_64::read_cr4() & CR4_LA57) == 0)
    {
        refuse("CR4.LA57 is still clear after the switch");
    }
    kickos::q35::com1_puts("x86_64 la57 witness: five-level paging entered\n");
}

// rdi: the root, rsi: the GDT pointer. Interrupts are off on this path. The upper halves of the
// registers are undefined after compatibility mode, so the callee-saved ones and rsp go through
// memory.
__asm__(".pushsection .bss.kickos_x86_64_la57_rsp,\"aw\",@nobits\n"
        ".balign 8\n"
        "kickos_x86_64_la57_rsp: .skip 8\n"
        ".popsection\n"
        ".pushsection .text.kickos_x86_64_la57_switch,\"ax\",@progbits\n"
        ".globl kickos_x86_64_la57_switch\n"
        ".type kickos_x86_64_la57_switch,@function\n"
        "kickos_x86_64_la57_switch:\n"
        "    pushq %rbx\n"
        "    pushq %rbp\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    movq %rsp, kickos_x86_64_la57_rsp(%rip)\n"
        "    lgdt (%rsi)\n"
        "    leaq 1f(%rip), %rax\n"
        "    pushq $0x10\n"
        "    pushq %rax\n"
        "    lretq\n"
        "    .code32\n"
        "1:  movw $0x18, %ax\n"
        "    movw %ax, %ds\n"
        "    movw %ax, %es\n"
        "    movw %ax, %ss\n"
        "    movl %cr0, %eax\n"
        "    andl $0x7fffffff, %eax\n"
        "    movl %eax, %cr0\n"
        "    movl %cr4, %eax\n"
        "    orl $0x1000, %eax\n"
        "    movl %eax, %cr4\n"
        "    movl %edi, %cr3\n"
        "    movl %cr0, %eax\n"
        "    orl $0x80000000, %eax\n"
        "    movl %eax, %cr0\n"
        "    call 2f\n"
        "2:  popl %eax\n"
        "    addl $(3f - 2b), %eax\n"
        "    pushl $0x08\n"
        "    pushl %eax\n"
        "    lretl\n"
        "    .code64\n"
        "3:  movw $0x18, %ax\n"
        "    movw %ax, %ds\n"
        "    movw %ax, %es\n"
        "    movw %ax, %ss\n"
        "    movq kickos_x86_64_la57_rsp(%rip), %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbp\n"
        "    popq %rbx\n"
        "    ret\n"
        ".size kickos_x86_64_la57_switch, . - kickos_x86_64_la57_switch\n"
        ".popsection\n");
