// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The first SMP bring-up witness: a second processor enters our code after
// ExitBootServices, using an allocated SIPI page and the BSP's live CR3.

#include <kickos/arch/apic.h>
#include <kickos/arch/arch.h>
#include <kickos/arch/desc.h>
#include <kickos/arch/regs.h>
#include <kickos/chip_com1.h>

#include <stdint.h>
#include <stddef.h>

extern "C" int kickos_x86_64_ap_prepare(void (*entry)(void), uintptr_t stack_top);
extern "C" void kickos_x86_64_ap_start(uint32_t apic_id);

// X6 has no kernel. These bodies are never entered in the AP-arrival arm.
extern "C" void kickos_isr_timer(void) {}
extern "C" void kickos_isr_irq(int) {}
extern "C" [[noreturn]] void kickos_thread_return(void)
{
    while (true)
    {
        __asm__ volatile("cli\n\thlt");
    }
}

namespace
{
    alignas(16) uint8_t g_ap_stack[16384];
    uint32_t g_ap_online = 0;
    uint32_t g_ap_id = UINT32_MAX;

    [[noreturn]] void ap_main(void)
    {
        uint32_t a = 0;
        uint32_t b = 0;
        uint32_t c = 0;
        uint32_t d = 0;
        __asm__ volatile("cpuid"
                         : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                         : "a"(1u), "c"(0u));
        g_ap_id = b >> 24;
        __atomic_store_n(&g_ap_online, 1u, __ATOMIC_RELEASE);
        while (true)
        {
            __asm__ volatile("cli\n\thlt");
        }
    }
}

extern "C" void kickos_x86_64_landed(uintptr_t ram_base, uint64_t ram_size)
{
    (void)ram_base;
    (void)ram_size;
    kickos::q35::com1_puts("KICKOS-X6 landing\n");
    kickos::x86_64::desc_init();
    kickos::q35::com1_puts("KICKOS-X6 descriptors\n");
    kickos::x86_64::apic_init();
    kickos::q35::com1_puts("KICKOS-X6 APIC\n");
    if (kickos_x86_64_ap_prepare(ap_main,
            reinterpret_cast<uintptr_t>(g_ap_stack) + sizeof(g_ap_stack)) == 0)
    {
        kickos::q35::com1_puts("KICKOS-X6 FAIL AP prepare\n");
        arch_shutdown(1);
    }
    kickos_x86_64_ap_start(1);
    uint64_t const deadline = kickos::x86_64::tsc_now()
                              + kickos::x86_64::apic_tsc_hz() * 5u;
    while (__atomic_load_n(&g_ap_online, __ATOMIC_ACQUIRE) == 0)
    {
        if (kickos::x86_64::tsc_now() > deadline)
        {
            kickos::q35::com1_puts("KICKOS-X6 FAIL AP did not arrive\n");
            arch_shutdown(1);
        }
        __asm__ volatile("pause" ::: "memory");
    }
    if (g_ap_id != 1u)
    {
        kickos::q35::com1_puts("KICKOS-X6 FAIL APIC ID mismatch\n");
        arch_shutdown(1);
    }
    kickos::q35::com1_puts("KICKOS-X6 AP online id=1\n");
    arch_shutdown(0);
}
