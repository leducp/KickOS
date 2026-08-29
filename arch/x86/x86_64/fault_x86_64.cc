// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// CR2 is read FIRST: nothing else preserves the address of the last page fault, so a fault
// taken inside the report would overwrite the value the report exists to show.
//
// No pointer table names a vector: an array of string pointers needs a base relocation and
// this image carries none.

#include <kickos/arch/desc.h>
#include <kickos/arch/regs.h>
#include <kickos/arch/trap.h>
#include <kickos/arch/x2probe.h>
#include <kickos/chip_com1.h>

#include <stdint.h>

extern "C" bool kickos_fault_kill_thread(void* frame);

namespace
{
    using namespace kickos::q35;
    using kickos::x86_64::read_cr2;

    char const* vector_name(uint64_t vector)
    {
        if (vector == 0)
        {
            return "divide error";
        }
        if (vector == 1)
        {
            return "debug";
        }
        if (vector == 2)
        {
            return "nmi";
        }
        if (vector == 3)
        {
            return "breakpoint";
        }
        if (vector == 4)
        {
            return "overflow";
        }
        if (vector == 5)
        {
            return "bound range exceeded";
        }
        if (vector == 6)
        {
            return "invalid opcode";
        }
        if (vector == 7)
        {
            return "device not available";
        }
        if (vector == 8)
        {
            return "double fault";
        }
        if (vector == 9)
        {
            return "coprocessor segment overrun";
        }
        if (vector == 10)
        {
            return "invalid task-state segment";
        }
        if (vector == 11)
        {
            return "segment not present";
        }
        if (vector == 12)
        {
            return "stack fault";
        }
        if (vector == 13)
        {
            return "general protection";
        }
        if (vector == 14)
        {
            return "page fault";
        }
        if (vector == 16)
        {
            return "x87 floating-point error";
        }
        if (vector == 17)
        {
            return "alignment check";
        }
        if (vector == 18)
        {
            return "machine check";
        }
        if (vector == 19)
        {
            return "simd floating-point error";
        }
        if (vector == 20)
        {
            return "virtualisation exception";
        }
        if (vector == 21)
        {
            return "control protection exception";
        }
        if (vector == 28)
        {
            return "hypervisor injection exception";
        }
        if (vector == 29)
        {
            return "vmm communication exception";
        }
        if (vector == 30)
        {
            return "security exception";
        }
        if (vector < 32)
        {
            return "reserved exception";
        }
        return "unexpected interrupt vector";
    }

    void bit(char const* label, uint64_t word, unsigned position)
    {
        com1_puts(label);
        com1_dec((word >> position) & 1u);
    }

    // Intel SDM Vol 3, the page-fault error code.
    void report_page_fault(uint64_t error, uint64_t cr2)
    {
        com1_puts("  CR2=");
        com1_hex64(cr2);
        com1_puts("\n");
        com1_puts("  pf:");
        bit(" present=", error, 0);
        bit(" write=", error, 1);
        bit(" user=", error, 2);
        bit(" rsvd=", error, 3);
        bit(" ifetch=", error, 4);
        bit(" pkey=", error, 5);
        bit(" shadow=", error, 6);
        com1_puts("\n");
    }

    // Intel SDM Vol 3, the selector error code the segment and protection faults push.
    void report_selector(uint64_t error)
    {
        com1_puts("  sel:");
        bit(" ext=", error, 0);
        com1_puts(" tbl=");
        if ((error & 2u) != 0)
        {
            com1_puts("idt");
        }
        else if ((error & 4u) != 0)
        {
            com1_puts("ldt");
        }
        else
        {
            com1_puts("gdt");
        }
        com1_puts(" index=");
        com1_dec((error >> 3) & 0x1fffu);
        com1_puts("\n");
    }

    [[noreturn]] void report(kickos::x86_64::trap_frame* frame)
    {
        uint64_t const cr2 = read_cr2();

        com1_puts("\n=== X86_64 EXCEPTION ===\n");
        com1_puts("  token=" KICKOS_X2_TOKEN "\n");

        com1_puts("  vector=");
        com1_dec(frame->vector);
        com1_puts(" (");
        com1_puts(vector_name(frame->vector));
        com1_puts(")\n");

        com1_puts("  error=");
        com1_hex64(frame->error);
        com1_puts("\n");

        com1_puts("  RIP=");
        com1_hex64(frame->rip);
        com1_puts(" CS=");
        com1_hex64(frame->cs);
        com1_puts("\n");

        com1_puts("  RFLAGS=");
        com1_hex64(frame->rflags);
        com1_puts(" RSP=");
        com1_hex64(frame->rsp);
        com1_puts(" SS=");
        com1_hex64(frame->ss);
        com1_puts("\n");

        if (frame->vector == 14)
        {
            report_page_fault(frame->error, cr2);
        }
        if (frame->vector >= 10 and frame->vector <= 13)
        {
            report_selector(frame->error);
        }

        com1_puts("  ring=");
        com1_dec(frame->cs & 3u);
        bit(" if=", frame->rflags, 9);
        com1_puts("\n");

        // Which stack the report is running on: the double-fault gate names an
        // interrupt-stack-table slot.
        uintptr_t const here = reinterpret_cast<uintptr_t>(frame);
        com1_puts("  stack=");
        if (here >= kickos::x86_64::fault_stack_lo() and here < kickos::x86_64::fault_stack_hi())
        {
            com1_puts("ist1");
        }
        else
        {
            com1_puts("interrupted");
        }
        com1_puts("\n");

        kickos::x86_64::desc_report();

        com1_puts("  " KICKOS_X2_TOKEN " halting\n");
        while (true)
        {
            __asm__ volatile("cli\n\thlt");
        }
    }
}

// Containment is asked BEFORE anything prints: kickos_fault_kill_thread applies the kill rule
// and, where it holds, has already re-pointed this frame at kickos_thread_fault_exit. Printing
// first would enter kpanic_enter, whose console reclaim is permanent, and this fault is
// survivable.
extern "C" kickos::x86_64::trap_frame* kickos_x86_64_trap(kickos::x86_64::trap_frame* frame)
{
    kickos::x86_64::trap_frame* const resume = kickos_x86_64_isr(frame);
    if (resume != nullptr)
    {
        return resume;
    }
    if (kickos_fault_kill_thread(frame))
    {
        return frame;
    }
    report(frame);
}
