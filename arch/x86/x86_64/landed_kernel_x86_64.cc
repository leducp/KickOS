// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Firmware has already left boot services when this runs (arch/x86/x86_64/entry_x86_64.cc).
//
// Do not zero .bss here: the PE loader already zero-fills it, and the handover TU's own
// state is in .bss and still live at this call.

#include <kickos/chip_q35.h>

#include <kickos/arch/arch.h>

#include <stddef.h>
#include <stdint.h>

extern "C"
{
    // Only the kernel-owned ctors; pe_image.ld routes them here, and app and library ctors
    // run later from kickos_root_entry.
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();
}

namespace kickos
{
    int kmain(int argc, char** argv);
}

namespace
{
    // The one stack the boot path and the idle thread run on.
    constexpr size_t boot_stack_bytes = 128 * 1024;
    alignas(16) uint8_t g_boot_stack[boot_stack_bytes];
}

// A named extern "C" symbol so the switch below reaches it with a direct relative call: an
// indirect call through its address is a GOT load, which tools/check-x86_64-no-got.sh refuses.
extern "C" [[noreturn]] void kickos_x86_64_kernel_main(void)
{
    arch_init();
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
