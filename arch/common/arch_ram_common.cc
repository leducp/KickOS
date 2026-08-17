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
    // The chip's two memories, for the non-enforcing user-pointer admission below.
    // NOT weak: a new chip whose linker script omits them must fail the LINK, because a
    // silently-absent extent would refuse every flash pointer at runtime instead.
    extern unsigned char __kickos_rom_start[];
    extern unsigned char __kickos_rom_end[];
    extern unsigned char __kickos_sram_start[];
}

namespace
{
    // Bump-allocated; freed only wholesale (matches the sim arena's M0 model).
    // Read-modify-written under arch_irq_save/restore in arch_ram_alloc.
    uint32_t g_ram_used = 0;

#if !KICKOS_HAVE_MPU
    bool range_within(uintptr_t ptr, uintptr_t end, uintptr_t start, uintptr_t stop)
    {
        // Decayed to uintptr_t by the callers: comparing two array-typed externs
        // directly trips -Warray-compare (gcc 12+).
        return stop > start and ptr >= start and end <= stop;
    }

    // Static RAM: the chip RAM origin up to the arena base. Covers .data/.bss (kernel
    // and app: the no-MPU layout does not split them) and the .userheap. The ARENA is
    // deliberately excluded: a thread stack or a granted block is a region the syscall's
    // own region check answers for, so a later enforcing build of the same backend stays
    // sound.
    bool in_static_ram(uintptr_t ptr, uintptr_t end)
    {
        return range_within(ptr, end, reinterpret_cast<uintptr_t>(__kickos_sram_start),
                            reinterpret_cast<uintptr_t>(__kickos_ram_start));
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
    // Only its ADDRESS escapes: the word an unprivileged thread is meant to fault on.
    static volatile uint32_t guard_word = 0;
    return reinterpret_cast<uintptr_t>(&guard_word);
#else
    return 0;
#endif
}

bool arch_user_text_readable(uintptr_t ptr, size_t len)
{
#if KICKOS_HAVE_MPU
    // Enforcing backend: the unprivileged thread's code/rodata/.data are real MPU
    // regions, so the syscall's region check already admits them; anything not in
    // the set is unreachable and must be rejected.
    (void)ptr;
    (void)len;
    return false;
#else
    // No MPU enforcement: there is no isolation to launder, but the kernel dereferences
    // this range PRIVILEGED, so it must be MAPPED. A WHITELIST of the chip's linker-
    // defined memories, never "anything outside the arena": an address in an
    // unimplemented hole or in device space passed that older test and hard-faulted the
    // kernel inside kaccess_from_user.
    if (len == 0)
    {
        return true;
    }
    uintptr_t const end = ptr + len;
    if (end < ptr)
    {
        return false; // wrap
    }
    if (range_within(ptr, end, reinterpret_cast<uintptr_t>(__kickos_rom_start),
                     reinterpret_cast<uintptr_t>(__kickos_rom_end)))
    {
        return true; // code + rodata (empty on a chip that executes from RAM)
    }
    return in_static_ram(ptr, end);
#endif
}

bool arch_user_data_writable(uintptr_t ptr, size_t len)
{
#if KICKOS_HAVE_MPU
    // Enforcing backend: .appdata/.appbss is a real MPU region, already admitted by
    // the syscall's region check; anything outside the set is genuinely unwritable.
    (void)ptr;
    (void)len;
    return false;
#else
    // No enforcement, but the kernel STORES here privileged, so the range must be mapped
    // and writable: static RAM only. Flash/ROM is excluded (an out-pointer there is a
    // caller bug the kernel must not turn into a discarded or faulting store), and an
    // arena range falls through to the region check.
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
