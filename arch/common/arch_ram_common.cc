// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

// Arch-independent user-RAM seam shared by every MCU backend (arm/riscv/rx/xtensa):
// the bounds come from the chip linker script (__kickos_ram_start/_end) and the
// bump allocator is pure arithmetic over the arch_ram_region_size/align seam, so
// there is nothing per-arch to specialise. arch_trace_stamp_id likewise is a plain
// write of the owning tid into the saved context (read back on the switch emit path).
// The host sim's RAM is an mmap arena, not linker symbols, so sim.cc keeps its own
// copies and this file is NOT compiled into the sim backend.

#include <kickos/arch/arch.h>

#include <stddef.h>
#include <stdint.h>

extern "C"
{
    extern unsigned char __kickos_ram_start[];
    extern unsigned char __kickos_ram_end[];
}

namespace
{
    // Bump-allocated; freed only wholesale (matches the sim arena's M0 model).
    volatile uint32_t g_ram_used = 0;
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

// A word that faults on UNPRIVILEGED access but is privileged-RW, for the isolation
// self-test's guard page. It lives in kernel-side .bss (this file is an arch object,
// not under user/, so it is neither in the app-data grant nor the arena), so an
// unprivileged thread has NO region covering it while the privileged background does
// (SYSMPU RGD0 / PMSA background / PMP). No enforced MPU -> 0 (the guard test is
// compiled out there). Shared by every MPU backend; the sim overrides with an
// mprotect'd arena page.
uintptr_t arch_mpu_probe_addr(void)
{
#if KICKOS_HAVE_MPU
    static volatile uint32_t guard_word = 0;
    return reinterpret_cast<uintptr_t>(&guard_word);
#else
    return 0;
#endif
}

}
