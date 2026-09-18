// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Linker-defined user RAM and trace helpers shared by MCU backends.
// The simulator uses its own mmap-backed arena.

#include <kickos/arch/arch.h>
#include <kickos/klink.h>

#include <stddef.h>
#include <stdint.h>

extern "C"
{
    extern unsigned char __kickos_ram_start[];
    extern unsigned char __kickos_ram_end[];
    // Required linker bounds for user-pointer validation without descriptors.
    extern unsigned char __kickos_rom_start[];
    extern unsigned char __kickos_rom_end[];
    extern unsigned char __kickos_sram_start[];
    // Optional separate app windows. Zero bounds admit no extra addresses.
    extern unsigned char __kickos_app_rom_start[] KICKOS_LINK_OPTIONAL;
    extern unsigned char __kickos_app_rom_end[] KICKOS_LINK_OPTIONAL;
    extern unsigned char __kickos_app_sram_start[] KICKOS_LINK_OPTIONAL;
    extern unsigned char __kickos_app_sram_end[] KICKOS_LINK_OPTIONAL;
}

namespace
{
    // Bump-allocated; freed only wholesale (matches the sim arena's model).
    // Read-modify-written under arch_irq_save/restore in arch_ram_alloc.
    uint32_t g_ram_used = 0;

#if !KICKOS_MEMORY_ENFORCED
    bool range_within(uintptr_t ptr, uintptr_t end, uintptr_t start, uintptr_t stop)
    {
        // Use integer addresses to avoid comparing extern arrays directly.
        return stop > start and ptr >= start and end <= stop;
    }

    // Static RAM includes app data, BSS and heap. Arena allocations are checked
    // against the caller's regions instead.
    bool in_static_ram(uintptr_t ptr, uintptr_t end)
    {
        if (range_within(ptr, end, reinterpret_cast<uintptr_t>(__kickos_sram_start),
                         reinterpret_cast<uintptr_t>(__kickos_ram_start)))
        {
            return true;
        }
        return range_within(ptr, end,
                            reinterpret_cast<uintptr_t>(__kickos_app_sram_start),
                            reinterpret_cast<uintptr_t>(__kickos_app_sram_end));
    }

    // Code + rodata: the chip's own extent, plus the app's where the image is split.
    bool in_code(uintptr_t ptr, uintptr_t end)
    {
        if (range_within(ptr, end, reinterpret_cast<uintptr_t>(__kickos_rom_start),
                         reinterpret_cast<uintptr_t>(__kickos_rom_end)))
        {
            return true;
        }
        return range_within(ptr, end, reinterpret_cast<uintptr_t>(__kickos_app_rom_start),
                            reinterpret_cast<uintptr_t>(__kickos_app_rom_end));
    }
#endif
}

extern "C"
{

uintptr_t arch_ram_base(void)
{
    return reinterpret_cast<uintptr_t>(__kickos_ram_start);
}

size_t arch_ram_size(void)
{
    return static_cast<size_t>(__kickos_ram_end - __kickos_ram_start);
}

void* arch_ram_alloc(size_t size)
{
    if (size == 0)
    {
        return nullptr;
    }
    size_t const rsz = arch_ram_region_size(size);
    size_t const ralign = arch_ram_region_align(size);
    size_t const total = arch_ram_size();
    uintptr_t const base = reinterpret_cast<uintptr_t>(__kickos_ram_start);
    arch_irq_state_t s = arch_irq_save();
    void* p = nullptr;
    uintptr_t const cur = base + g_ram_used;
    // Natural (absolute) alignment: PMSA/NAPOT require base aligned to size.
    uintptr_t const aligned = (cur + (ralign - 1)) & ~static_cast<uintptr_t>(ralign - 1);
    size_t const off = static_cast<size_t>(aligned - base);
    if (aligned >= cur and off <= total and rsz <= total - off)
    {
        p = reinterpret_cast<void*>(aligned);
        g_ram_used = static_cast<uint32_t>(off + rsz);
    }
    arch_irq_restore(s);
    return p;
}

#if defined(KICKOS_TELEMETRY) && KICKOS_TELEMETRY
void arch_trace_stamp_id(struct arch_context* ctx, uint16_t id)
{
    ctx->trace_tid = id;
}
#endif

// Kernel word used to test fault isolation. It is inaccessible to userspace
// under either MPU or MMU enforcement. Return zero when unenforced.
// The simulator uses an mprotect-protected arena page instead.
uintptr_t arch_mpu_probe_addr(void)
{
#if KICKOS_MEMORY_ENFORCED
    static volatile uint32_t guard_word = 0;
    return reinterpret_cast<uintptr_t>(&guard_word);
#else
    return 0;
#endif
}

bool arch_user_text_readable(uintptr_t ptr, size_t len)
{
#if KICKOS_MEMORY_ENFORCED
    // Under enforcement, validate against the caller's regions or mapped ranges.
    (void)ptr;
    (void)len;
    return false;
#else
    // Without enforcement, accept only linker-defined memory windows.
    // The kernel must not dereference holes or device addresses.
    if (len == 0)
    {
        return true;
    }
    uintptr_t const end = ptr + len;
    if (end < ptr)
    {
        return false; // wrap
    }
    if (in_code(ptr, end))
    {
        return true; // code + rodata (empty on a chip that executes from RAM)
    }
    return in_static_ram(ptr, end);
#endif
}

bool arch_user_data_writable(uintptr_t ptr, size_t len)
{
#if KICKOS_MEMORY_ENFORCED
    (void)ptr;
    (void)len;
    return false;
#else
    // Only static RAM is writable here. Arena ranges require a region check.
    if (len == 0)
    {
        return true;
    }
    uintptr_t const end = ptr + len;
    if (end < ptr)
    {
        return false; // wrap
    }
    return in_static_ram(ptr, end);
#endif
}

}
