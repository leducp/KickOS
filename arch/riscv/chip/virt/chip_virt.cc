// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// QEMU `virt` (RISC-V RV32IMAC, M-mode bare metal) chip backend -- the runnable
// rv32imac verification target, the RISC-V analog of arch/arm/chip/mps2. The
// hardware edges the rv32imac arch layer leaves to the chip: reset/C-runtime
// bring-up, arch_init (CLINT base + trap/mie install), the debug console + exit,
// and the tickless clock/timer. Like mps2, RISC-V semihosting stands in for a UART
// (console + exit code), so this target needs no peripheral driver; the ESP32-C6
// chip swaps semihosting for a real UART + SYSTIMER.
//
// Standard-RISC-V facts (Privileged ISA v1.10 + the QEMU `virt` memory map): CLINT
// at 0x0200_0000 -- msip (hart 0) @+0x0000, mtimecmp @+0x4000, mtime @+0xBFF8; the
// `virt` mtime runs at 10 MHz. Run with: qemu-system-riscv32 -M virt -bios none
// -nographic -semihosting -kernel <elf>.

#include <kickos/arch/arch.h>

#include <stdint.h>

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    void kickos_rv32_init(void);

    // The arch layer's deferred-switch software-interrupt register (CLINT msip),
    // set here so switch.S can pend/clear it.
    extern volatile uint32_t* g_clint_msip;

    // Linker-script symbols (virt.ld).
    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    // DWARF EH frame table (virt.ld) + the libgcc registrar. RISC-V exceptions use
    // DWARF .eh_frame, normally registered by crtbegin's frame_dummy -- but our
    // -nostartfiles link drops crtbegin, so a full-C++ app registers the table by hand
    // at boot (Reset_Handler). WEAK ref: a freestanding image references no _Unwind_*,
    // so __register_frame's libgcc object (unwind-dw2-fde) is never pulled -- the ref
    // stays null and the call is skipped, keeping the FDE machinery + newlib malloc +
    // its 64 KB heap arena OUT of freestanding images. A FULL_CXX app pulls
    // _Unwind_Find_FDE (same libgcc object), defining __register_frame, so the ref
    // resolves and registration runs.
    extern uint32_t __eh_frame_start;
    void __register_frame(void*) __attribute__((weak));
#if KICKOS_HAVE_MPU
    // App-data NAPOT region (virt.ld). .appdata holds the app + C++-runtime .data and
    // the gp small-data window; its VMA jumps to the pow2 window base above the NOLOAD
    // kernel .bss, so LMA != VMA and it needs a copy (like .data) before .appbss + pad
    // are zeroed. All through the RW grant, so an unprivileged thread never reads stale
    // bytes from its data region.
    extern uint32_t _appdata_lma, __kickos_appdata_start, __kickos_appbss_start,
        __kickos_appdata_end;
#endif

    // Nominal core clock (Hz). QEMU's rdcycle is not cycle-accurate, so this only
    // feeds the bench's cycles->ns print (a smoke number here; the C6 gives the
    // real one). Match the `virt` mtime rate so the two clocks are consistent.
    uint32_t SystemCoreClock = 10000000u;
}

namespace
{
    inline volatile uint32_t* r32p(uintptr_t a) { return reinterpret_cast<volatile uint32_t*>(a); }

    // QEMU `virt` CLINT (hart 0).
    constexpr uintptr_t CLINT_BASE = 0x02000000;
    constexpr uintptr_t CLINT_MSIP = CLINT_BASE + 0x0000;
    constexpr uintptr_t CLINT_MTIMECMP = CLINT_BASE + 0x4000; // 64-bit
    constexpr uintptr_t CLINT_MTIME = CLINT_BASE + 0xBFF8;     // 64-bit
    constexpr uint64_t MTIME_HZ = 10000000ull;                 // `virt` mtime = 10 MHz
    constexpr uint64_t NS_PER_TICK = 1000000000ull / MTIME_HZ; // 100 ns

    // --- RISC-V semihosting (QEMU implements the host side; the magic sequence is
    //     slli x0,x0,0x1f / ebreak / srai x0,x0,7 -- must NOT be compressed). ---
    // .balign 16 (not 4): QEMU's magic-sequence check reads the words at ebreak-4 and
    // ebreak+4; if the 12-byte sequence straddles a 4K page it fails to recognize the
    // call and the ebreak traps as a plain breakpoint. 16 divides 4096, so the block
    // never crosses a page. (A layout shift can otherwise land srai on a page start.)
    inline long semihost(long op, void* arg)
    {
        register long a0 __asm("a0") = op;
        register void* a1 __asm("a1") = arg;
        __asm volatile(".option push\n"
                       ".option norvc\n"
                       ".balign 16\n"
                       "slli x0, x0, 0x1f\n"
                       "ebreak\n"
                       "srai x0, x0, 7\n"
                       ".option pop\n"
                       : "+r"(a0)
                       : "r"(a1)
                       : "memory");
        return a0;
    }

    constexpr long SYS_WRITEC = 0x03;
    constexpr long SYS_EXIT_EXTENDED = 0x20;
    constexpr uint32_t ADP_Stopped_ApplicationExit = 0x20026u;
}

extern "C"
{

void arch_init(void)
{
    g_clint_msip = r32p(CLINT_MSIP);
    // mtimecmp = max: mtip stays low until arch_timer_arm programs a deadline.
    r32p(CLINT_MTIMECMP)[0] = 0xFFFFFFFFu;
    r32p(CLINT_MTIMECMP)[1] = 0xFFFFFFFFu;
    kickos_rv32_init(); // mtvec + mie(MSIE|MTIE|SSIE) + mcounteren + PMP + reset state
    // arch_irq_* (inject/mask via the SSIP software channel) are arch-provided --
    // no chip external controller is wired on virt (the console is semihosting).
}

// --- Tickless clock: the 64-bit CLINT mtime (10 MHz) -> ns ------------------
uint64_t arch_clock_now(void)
{
    volatile uint32_t* mt = r32p(CLINT_MTIME);
    uint32_t hi, lo, hi2;
    // Re-read the high half to guard against a low-half rollover mid-read.
    do
    {
        hi = mt[1];
        lo = mt[0];
        hi2 = mt[1];
    } while (hi != hi2);
    uint64_t t = (static_cast<uint64_t>(hi) << 32) | lo;
    return t * NS_PER_TICK;
}

// --- One-shot next-event timer: CLINT mtimecmp (absolute) -------------------
void arch_timer_arm(uint64_t deadline_ns)
{
    uint64_t ticks = deadline_ns / NS_PER_TICK;
    volatile uint32_t* cmp = r32p(CLINT_MTIMECMP);
    // Safe 64-bit write on RV32: park the high half so no spurious match can fire
    // between the two 32-bit stores, then commit lo then hi.
    cmp[1] = 0xFFFFFFFFu;
    cmp[0] = static_cast<uint32_t>(ticks);
    cmp[1] = static_cast<uint32_t>(ticks >> 32);
}

void arch_timer_disarm(void)
{
    volatile uint32_t* cmp = r32p(CLINT_MTIMECMP);
    cmp[0] = 0xFFFFFFFFu;
    cmp[1] = 0xFFFFFFFFu;
}

// --- Debug console + exit via semihosting (the mps2 model) ------------------
void arch_console_write(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        char c = buf[i];
        semihost(SYS_WRITEC, &c);
    }
}

void arch_shutdown(int status)
{
    uint32_t block[2];
    block[0] = ADP_Stopped_ApplicationExit;
    block[1] = static_cast<uint32_t>(status);
    semihost(SYS_EXIT_EXTENDED, block);
    // If semihosting exit returns, mask interrupts and park.
    __asm volatile("csrci mstatus, 0x8" ::: "memory"); // clear MIE
    while (true)
    {
        __asm volatile("wfi");
    }
}

// --- C-runtime bring-up (called by _start in startup.S) ---------------------
// A fault/panic on this QEMU target must EXIT with a status so a CTest run
// catches it (no LED; the weak blink terminal would spin to a harness timeout).
void kfault_terminate(void)
{
    arch_shutdown(132);
}

void Reset_Handler(void)
{
    // .data image lives at its VMA in RAM (QEMU loads segments in place), so the
    // copy is a no-op (LMA == VMA); kept for uniformity with the ARM ports.
    uint32_t* src = &_sidata;
    uint32_t* dst = &_sdata;
    while (dst < &_edata)
    {
        *dst++ = *src++;
    }
    for (uint32_t* b = &_sbss; b < &_ebss; b++)
    {
        *b = 0;
    }
#if KICKOS_HAVE_MPU
    uint32_t* asrc = &_appdata_lma;
    uint32_t* adst = &__kickos_appdata_start;
    while (adst < &__kickos_appbss_start) // .appdata: LMA != VMA (see decl)
    {
        *adst++ = *asrc++;
    }
    for (uint32_t* b = &__kickos_appbss_start; b < &__kickos_appdata_end; b++)
    {
        *b = 0;
    }
#endif
    if (__register_frame != nullptr) // weak: null in a freestanding image (see decl)
    {
        __register_frame(&__eh_frame_start); // DWARF EH: register before ctors/throws
    }
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}

}
