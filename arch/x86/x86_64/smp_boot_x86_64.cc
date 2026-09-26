// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// q35 AP bootstrap. Reserve one SIPI page while UEFI owns its memory map, then
// copy and patch the real-mode trampoline after ExitBootServices. APs are
// released one at a time, so the page's destination and stack words are not
// rewritten until the preceding AP has left the trampoline.

#include <kickos/arch/apic.h>
#include <kickos/arch/regs.h>
#include <kickos/arch/uefi.h>

#include <stdint.h>
#include <stddef.h>

extern "C"
{
    extern __attribute__((visibility("hidden"))) uint8_t kickos_x86_64_ap_begin[];
    extern __attribute__((visibility("hidden"))) uint8_t kickos_x86_64_ap_end[];
    extern __attribute__((visibility("hidden"))) uint8_t kickos_x86_64_ap_long[];
    extern __attribute__((visibility("hidden"))) uint8_t kickos_x86_64_ap_jump_target[];
    extern __attribute__((visibility("hidden"))) uint8_t kickos_x86_64_ap_gdt[];
    extern __attribute__((visibility("hidden"))) uint8_t kickos_x86_64_ap_gdt_base[];
    extern __attribute__((visibility("hidden"))) uint8_t kickos_x86_64_ap_cr3[];
    extern __attribute__((visibility("hidden"))) uint8_t kickos_x86_64_ap_cr4[];
    extern __attribute__((visibility("hidden"))) uint8_t kickos_x86_64_ap_stack[];
    extern __attribute__((visibility("hidden"))) uint8_t kickos_x86_64_ap_entry[];
}

namespace
{
    using kickos::uefi::boot_services;
    using kickos::uefi::status_t;

    constexpr uintptr_t PAGE_BYTES = 4096;
    constexpr uintptr_t LOW_LIMIT = 0xa0000;
    constexpr uint32_t EFI_ALLOCATE_MAX_ADDRESS = 1;
    constexpr uint32_t EFI_LOADER_CODE = 1;
    constexpr uint32_t MSR_X2APIC_ICR = 0x830;
    constexpr uint32_t ICR_INIT_ASSERT = 0xc500;
    constexpr uint32_t ICR_INIT_DEASSERT = 0x8500;
    constexpr uint32_t ICR_STARTUP = 0x600;
    constexpr uint32_t CR4_PAE = 1u << 5;
    constexpr uint32_t CR4_LA57 = 1u << 12;

    using allocate_pages_fn = status_t(KICKOS_EFIAPI*)(uint32_t, uint32_t, uint64_t, uint64_t*);

    uintptr_t g_page = 0;

    void patch32(uint8_t* image, uint8_t const* field, uint32_t value)
    {
        size_t const offset = reinterpret_cast<uintptr_t>(field)
                              - reinterpret_cast<uintptr_t>(kickos_x86_64_ap_begin);
        for (unsigned i = 0; i < 4; ++i)
        {
            image[offset + i] = static_cast<uint8_t>(value >> (8u * i));
        }
    }

    void patch64(uint8_t* image, uint8_t const* field, uint64_t value)
    {
        size_t const offset = reinterpret_cast<uintptr_t>(field)
                              - reinterpret_cast<uintptr_t>(kickos_x86_64_ap_begin);
        for (unsigned i = 0; i < 8; ++i)
        {
            image[offset + i] = static_cast<uint8_t>(value >> (8u * i));
        }
    }

    void wait_ns(uint64_t ns)
    {
        uint64_t const ticks = (kickos::x86_64::apic_tsc_hz() / 1000000u) * (ns / 1000u);
        uint64_t const start = kickos::x86_64::tsc_now();
        while (kickos::x86_64::tsc_now() - start < ticks)
        {
            __asm__ volatile("pause" ::: "memory");
        }
    }

    void icr(uint32_t apic_id, uint32_t low)
    {
        uint64_t const value = (static_cast<uint64_t>(apic_id) << 32) | low;
        kickos::x86_64::write_msr(MSR_X2APIC_ICR, value);
    }
}

extern "C" int kickos_x86_64_ap_reserve(boot_services* bs)
{
    allocate_pages_fn const allocate = reinterpret_cast<allocate_pages_fn>(bs->allocate_pages);
    if (allocate == nullptr)
    {
        return 0;
    }
    uint64_t page = LOW_LIMIT - PAGE_BYTES;
    if (allocate(EFI_ALLOCATE_MAX_ADDRESS, EFI_LOADER_CODE, 1, &page)
        != kickos::uefi::status_success)
    {
        return 0;
    }
    if (page < PAGE_BYTES or page >= LOW_LIMIT or (page & (PAGE_BYTES - 1)) != 0)
    {
        return 0;
    }
    g_page = static_cast<uintptr_t>(page);
    return 1;
}

extern "C" int kickos_x86_64_ap_prepare(void (*entry)(void), uintptr_t stack_top)
{
    if (g_page == 0 or not kickos::x86_64::apic_is_x2())
    {
        return 0;
    }
    size_t const bytes = reinterpret_cast<uintptr_t>(kickos_x86_64_ap_end)
                         - reinterpret_cast<uintptr_t>(kickos_x86_64_ap_begin);
    if (bytes >= PAGE_BYTES - 256)
    {
        return 0;
    }
    uint64_t const cr3 = kickos::x86_64::read_cr3();
    if (cr3 > UINT32_MAX or cr3 == 0)
    {
        return 0;
    }
    uint8_t* const image = reinterpret_cast<uint8_t*>(g_page);
    for (size_t i = 0; i < bytes; ++i)
    {
        image[i] = kickos_x86_64_ap_begin[i];
    }
    patch32(image, kickos_x86_64_ap_gdt_base,
            static_cast<uint32_t>(g_page + reinterpret_cast<uintptr_t>(kickos_x86_64_ap_gdt)
                                  - reinterpret_cast<uintptr_t>(kickos_x86_64_ap_begin)));
    patch32(image, kickos_x86_64_ap_cr3, static_cast<uint32_t>(cr3));
    patch32(image, kickos_x86_64_ap_cr4,
            CR4_PAE | (static_cast<uint32_t>(kickos::x86_64::read_cr4()) & CR4_LA57));
    patch32(image, kickos_x86_64_ap_jump_target,
            static_cast<uint32_t>(g_page + reinterpret_cast<uintptr_t>(kickos_x86_64_ap_long)
                                  - reinterpret_cast<uintptr_t>(kickos_x86_64_ap_begin)));
    patch64(image, kickos_x86_64_ap_stack, stack_top);
    patch64(image, kickos_x86_64_ap_entry, reinterpret_cast<uintptr_t>(entry));
    __asm__ volatile("mfence" ::: "memory");
    return 1;
}

extern "C" void kickos_x86_64_ap_start(uint32_t apic_id)
{
    icr(apic_id, ICR_INIT_ASSERT);
    wait_ns(10000000u);
    icr(apic_id, ICR_INIT_DEASSERT);
    wait_ns(200000u);
    uint32_t const vector = static_cast<uint32_t>(g_page >> 12);
    icr(apic_id, ICR_STARTUP | vector);
}
