// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Firmware has already left boot services when this runs (arch/x86/x86_64/entry_x86_64.cc).
//
// Do not zero .bss here: the PE loader already zero-fills it, and the handover TU's own
// state is in .bss and still live at this call.

#include <kickos/chip_com1.h>
#include <kickos/chip_q35.h>

#include <kickos/arch/arch.h>
#include <kickos/arch/x86_64_trap_stack.h>

#include <stddef.h>
#include <stdint.h>

extern "C"
{
    // The kernel's constructors; a program's run from its own kickos_root_entry.
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();
    // The linker's own symbol for the image's first byte, taken PC-relative, so it is where
    // firmware LOADED the image.
    extern __attribute__((visibility("hidden"))) char __ImageBase[];
}

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    void kpanic_enter(void);
    [[noreturn]] void kfault_terminate(void);
}

// fault_x86_64.cc's report ends; nokernel_x86_64.cc defines the kernel-free pair.
extern "C" void kickos_x86_64_fault_report_open(void)
{
    kpanic_enter();
}

extern "C" [[noreturn]] void kickos_x86_64_fault_report_close(void)
{
    kfault_terminate();
}

namespace
{
    constexpr size_t boot_stack_bytes = KICKOS_X86_64_BOOT_STACK;
    alignas(16) uint8_t g_boot_stack[boot_stack_bytes];

    // The script's absolute __kickos_link_image_base, as an immediate: firmware rewrites the
    // loaded header's ImageBase to the address it chose, and relocates every other word.
    uintptr_t link_image_base(void)
    {
        uintptr_t v = 0;
        __asm__("movabsq $__kickos_link_image_base, %0" : "=r"(v));
        return v;
    }

    // Zero where firmware placed the image at its preferred base and relocated nothing.
    void report_load(void)
    {
        uintptr_t const base = reinterpret_cast<uintptr_t>(__ImageBase);
        kickos::q35::com1_puts("x86_64 image: base=");
        kickos::q35::com1_hex64(base);
        kickos::q35::com1_puts(" load_delta=");
        kickos::q35::com1_hex64(base - link_image_base());
        kickos::q35::com1_puts("\n");
    }
}

// The paging regime the kernel adopts: firmware's, unless a witness image moves it first
// (kickos_x86_64_paging_entry_default.cc).
extern "C" void kickos_x86_64_paging_entry(void);

// A named extern "C" symbol so the switch below reaches it with a direct relative call: an
// indirect call through its address is a GOT load, which tools/check-x86_64-no-got.sh refuses.
extern "C" [[noreturn]] void kickos_x86_64_kernel_main(void)
{
    kickos_x86_64_paging_entry();
    arch_init();
    report_load();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}

extern "C" void kickos_x86_64_landed(uintptr_t ram_base, uint64_t ram_size)
{
    kickos::q35::ram_publish(ram_base, static_cast<size_t>(ram_size));
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    // The return address `call` pushes is what gives the callee its psABI stack alignment.
    uintptr_t const top = reinterpret_cast<uintptr_t>(g_boot_stack) + boot_stack_bytes;
    __asm__ volatile("movq %0, %%rsp\n\t"
                     "xorl %%ebp, %%ebp\n\t"
                     "call kickos_x86_64_kernel_main"
                     :
                     : "r"(top)
                     : "memory");
    __builtin_unreachable();
}
