// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// UEFI's own GDT and IDT live in boot-services data, which becomes free for reuse the moment
// ExitBootServices returns; desc_init builds both in this image. Paging is adopted as handed
// over.

#ifndef KICKOS_ARCH_DESC_H
#define KICKOS_ARCH_DESC_H

#include <stdint.h>

namespace kickos::x86_64
{
    // Selectors into the table desc_init builds. Byte offsets, and the low two bits are the
    // requested privilege level. Their ORDER is fixed by SYSCALL and SYSRET, asserted below.
    constexpr uint16_t sel_null = 0x0000;
    constexpr uint16_t sel_kernel_code = 0x0008;
    constexpr uint16_t sel_kernel_data = 0x0010;
    constexpr uint16_t sel_user_data = 0x001b;
    constexpr uint16_t sel_user_code = 0x0023;
    constexpr uint16_t sel_tss = 0x0028;

    static_assert(sel_kernel_data == sel_kernel_code + 8,
                  "SYSCALL loads SS from IA32_STAR[47:32] + 8");
    static_assert((sel_user_data & ~3u) == (sel_user_code & ~3u) - 8,
                  "SYSRET loads SS from IA32_STAR[63:48] + 8 and CS from + 16");
    static_assert((sel_user_code & 3u) == 3 and (sel_user_data & 3u) == 3,
                  "an iretq to ring 3 needs both selectors' requested level at 3");

    // Every vector is present, so a vector this port never planned for still reports.
    constexpr unsigned idt_vectors = 256;

    // A double fault arrives because the interrupted stack could not take a frame, so it is
    // the one vector that cannot report on the stack it interrupted.
    constexpr unsigned ist_double_fault = 1;

    // The two vectors the interrupt flag cannot hold off, so they are the only ones that can
    // land in the syscall entry's window on the caller's own stack pointer (switch.S). Neither
    // handler returns, so a nested arrival on one slot is not a case.
    constexpr unsigned ist_nmi = 2;
    constexpr unsigned ist_machine_check = 3;

    struct __attribute__((packed)) gdt_entry
    {
        uint16_t limit_lo;
        uint16_t base_lo;
        uint8_t base_mid;
        uint8_t access;
        uint8_t flags_limit_hi;
        uint8_t base_hi;
    };

    struct __attribute__((packed)) tss64
    {
        uint32_t reserved0;
        uint64_t rsp0;
        uint64_t rsp1;
        uint64_t rsp2;
        uint64_t reserved1;
        uint64_t ist[7];
        uint64_t reserved2;
        uint16_t reserved3;
        uint16_t iomap_base;
    };

    struct __attribute__((packed)) idt_gate
    {
        uint16_t offset_lo;
        uint16_t selector;
        uint8_t ist;
        uint8_t type_attr;
        uint16_t offset_mid;
        uint32_t offset_hi;
        uint32_t reserved;
    };

    // The operand lgdt/lidt take and sgdt/sidt write back: limit first, then base.
    struct __attribute__((packed)) desc_ptr
    {
        uint16_t limit;
        uint64_t base;
    };

    void desc_init(void);
    void desc_report(void);

    desc_ptr read_gdtr(void);
    desc_ptr read_idtr(void);
    uint16_t read_tr(void);
    uint16_t read_cs(void);
    uint16_t read_ss(void);

    uintptr_t gdt_address(void);
    uintptr_t idt_address(void);
    uintptr_t tss_address(void);
    uint64_t tss_rsp0(void);

    // Bounds of the block tss.ist[ist_double_fault - 1] points past the TOP of.
    uintptr_t fault_stack_lo(void);
    uintptr_t fault_stack_hi(void);

    // The hardware reads this for a ring 3 to ring 0 transition; the syscall entry has a copy
    // of its own (ring3.h). Both are written on every switch.
    void tss_set_rsp0(uint64_t top);
    uint64_t tss_ist(unsigned slot);
}

#endif
