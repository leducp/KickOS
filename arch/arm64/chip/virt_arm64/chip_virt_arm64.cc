// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// QEMU `virt` for AArch64 (Cortex-A53, EL1 bare metal): PL011 UART at 0x0900_0000, GICv2,
// and the architected generic timer as the timebase. `-cpu cortex-a53` is required, the
// machine defaulting to a core that refuses an A64 image.

#include <kickos/arch/arch.h>
#include <kickos/arch/clk_q32.h> // KICKOS_NS_PER_SEC (canonical 1e9 ns/sec)
#include <kickos/chip_limits.h>  // KICKOS_MAX_IRQ: this GIC's interrupt-ID count

#include <stdint.h>

// THE EL REFUSAL PATH IS LINKED INTO THE IDENTITY-MAPPED BOOT SPAN (virt_arm64.ld).
// startup.S branches to it with SCTLR_EL1.M still 0, where no walk resolves a high-half
// address, so it names the PL011 by its physical base rather than through dev_va and may
// not call out of the section: ld's veneer for an out-of-range branch still targets the
// callee's high address.
#define KICKOS_BOOT_TEXT __attribute__((section(".text.init"), noinline, used))
#define KICKOS_BOOT_RODATA __attribute__((section(".rodata.init"), used))

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    // Linker-script symbols (virt_arm64.ld).
    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
    // VA - PA for the kernel's half; see dev_va below.
    extern unsigned char __kickos_arm64_va_base[];
    // The app's .bss lives in the low window, outside _sbss.._ebss, so the boot zeroing
    // covers it separately. Its .data needs no copy: the low window links VMA == LMA.
    extern uint32_t __kickos_appbss_start, __kickos_appbss_end;
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    // DWARF EH frame table (virt_arm64.ld) + the libgcc registrar. The -nostartfiles link
    // drops crtbegin, so its frame_dummy never registers .eh_frame and a full-C++ app must
    // register it by hand at boot. WEAK ref: a freestanding image references no _Unwind_*,
    // so the ref stays null and the call is skipped.
    extern uint32_t __eh_frame_start;
    void __register_frame(void*) __attribute__((weak));

    // Nominal core clock (Hz).
    uint32_t SystemCoreClock = 0;

    void kfault_terminate(void) __attribute__((noreturn));
}

namespace
{
    // EVERY DEVICE REGISTER IS REACHED THROUGH THE KERNEL'S OWN HALF. The device gigabyte
    // is mapped at PA + __kickos_arm64_va_base by TTBR1, which every address space shares;
    // TTBR0 carries a per-process root that maps no device at all, so a low literal here
    // would translate against whatever process happened to be running.
    inline uintptr_t dev_va(uintptr_t pa)
    {
        return pa + reinterpret_cast<uintptr_t>(__kickos_arm64_va_base);
    }

    inline volatile uint32_t* r32p(uintptr_t a)
    {
        return reinterpret_cast<volatile uint32_t*>(dev_va(a));
    }

    // QEMU `virt` PL011.
    constexpr uintptr_t UART0_BASE = 0x09000000;
    constexpr uintptr_t UART_DR = UART0_BASE + 0x00;
    constexpr uintptr_t UART_FR = UART0_BASE + 0x18;
    constexpr uint32_t UART_FR_TXFF = 1u << 5;
    constexpr uint32_t UART_FR_BUSY = 1u << 3;

    // A BOUND, not a timing: arch.h requires the flush to be bounded because it sits on the
    // panic and shutdown paths, where a wedged UART must cost a dropped tail rather than a
    // hang. The same reasoning binds the writer below.
    constexpr uint32_t UART_POLL_BOUND = 100000;

    // The refusal path's own writer, with the PL011 at the address the bus sees: the MMU
    // is off, so dev_va's high alias translates through nothing.
    KICKOS_BOOT_TEXT void boot_console_write(char const* buf, size_t n)
    {
        volatile uint32_t* fr = reinterpret_cast<volatile uint32_t*>(UART_FR);
        volatile uint32_t* dr = reinterpret_cast<volatile uint32_t*>(UART_DR);
        for (size_t i = 0; i < n; i++)
        {
            uint32_t spin = 0;
            while ((*fr & UART_FR_TXFF) != 0 and spin < UART_POLL_BOUND)
            {
                spin++;
            }
            *dr = static_cast<uint32_t>(static_cast<unsigned char>(buf[i]));
        }
    }

    // File scope, so they are .rodata for certain: as locals the compiler is free to
    // stage them on the stack through memcpy, and this path runs before the C runtime.
    // In the boot span because the reader runs there.
    KICKOS_BOOT_RODATA char const BAD_EL_HEAD[] = "KickOS: qemu-arm64 entered at EL";
    KICKOS_BOOT_RODATA char const BAD_EL_TAIL[] = ", and this port implements EL1 only\n";
    char const BAD_CNTFRQ[] =
        "KickOS: qemu-arm64 CNTFRQ_EL0 does not divide a second exactly\n";

    // AArch64 semihosting: `hlt #0xF000` with the operation in x0 and the parameter in x1.
    // The console does NOT go through here, this chip having a real PL011, so the only
    // callers are the two exits below.
    // ALWAYS inlined: one of those exits sits in the boot span and can call nothing outside
    // it.
    inline __attribute__((always_inline)) long semihost(long op, void* arg)
    {
        register long x0 __asm("x0") = op;
        register void* x1 __asm("x1") = arg;
        __asm volatile("hlt #0xF000" : "+r"(x0) : "r"(x1) : "memory");
        return x0;
    }

    constexpr long SYS_EXIT = 0x18;
    constexpr uint64_t ADP_Stopped_ApplicationExit = 0x20026u;

    volatile uint8_t* r8p(uintptr_t a)
    {
        return reinterpret_cast<volatile uint8_t*>(dev_va(a));
    }

    // GICv2 on QEMU `virt`. The distributor is global, the CPU interface is this core's.
    constexpr uintptr_t GICD_BASE = 0x08000000;
    constexpr uintptr_t GICC_BASE = 0x08010000;
    constexpr uintptr_t GICD_CTLR = GICD_BASE + 0x000;
    constexpr uintptr_t GICD_ISENABLER = GICD_BASE + 0x100;
    constexpr uintptr_t GICD_ICENABLER = GICD_BASE + 0x180;
    constexpr uintptr_t GICD_ICPENDR = GICD_BASE + 0x280;
    constexpr uintptr_t GICD_ISPENDR = GICD_BASE + 0x200;
    constexpr uintptr_t GICD_IPRIORITYR = GICD_BASE + 0x400;
    constexpr uintptr_t GICD_ITARGETSR = GICD_BASE + 0x800;
    constexpr uintptr_t GICC_CTLR = GICC_BASE + 0x000;
    constexpr uintptr_t GICC_PMR = GICC_BASE + 0x004;
    constexpr uintptr_t GICC_IAR = GICC_BASE + 0x00C;
    constexpr uintptr_t GICC_EOIR = GICC_BASE + 0x010;

    // THE KIND, as a value range rather than a separate field (roadmap.md's `(line, kind)`).
    // Below 32 the GIC banks its registers per core, so INTID 30 names THIS core's timer; at
    // or above 32 an interrupt is global and reaches no core until ITARGETSR names one. Every
    // arch_irq_* body branches on this boundary and nothing else does.
    constexpr int GIC_BANKED_INTIDS = 32;

    // INTID 1023 means "no pending interrupt" in an IAR read.
    constexpr uint32_t GICC_IAR_SPURIOUS = 1023;
    constexpr uint32_t GICC_IAR_ID_MASK = 0x3FF;

    // EL1 physical timer, an architecturally assigned PPI. It is NOT a kernel IRQ line:
    // kickos_isr_timer takes no line and the timer is in no dispatch table.
    constexpr int PPI_EL1_PHYS_TIMER = 30;

    constexpr uint64_t CNTP_CTL_ENABLE = 1u << 0;

    // ns per tick of the architected counter, from CNTFRQ_EL0 at bring-up. QEMU virt reports
    // 62.5 MHz, so this is exactly 16 and the conversions are lossless.
    uint64_t g_ns_per_tick = 0;

    uint64_t counter_now(void)
    {
        uint64_t t = 0;
        // The ISB is the architected ordered read: without it the counter may be sampled
        // out of order with the surrounding instructions, and every deadline derives from it.
        __asm volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
        return t;
    }
}

extern "C"
{

void arch_init(void)
{
    uint64_t freq = 0;
    __asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    // CNTFRQ_EL0 is firmware-programmed, so a board whose firmware never wrote it reads 0,
    // and a frequency that does not divide a second exactly makes g_ns_per_tick lossy.
    if (freq == 0 or (kickos::KICKOS_NS_PER_SEC % freq) != 0)
    {
        arch_console_write(BAD_CNTFRQ, sizeof(BAD_CNTFRQ) - 1);
        kfault_terminate();
    }
    g_ns_per_tick = kickos::KICKOS_NS_PER_SEC / freq;
    SystemCoreClock = static_cast<uint32_t>(freq);

    // CNTP_CTL_EL0's reset value is architecturally UNKNOWN, so an already-asserted timer
    // would fire the moment the GIC and DAIF open.
    __asm volatile("msr cntp_ctl_el0, %0" ::"r"(uint64_t(0)));

    // PMR wide open at 0xF0: everything here sits at priority 0, and the reset PMR of 0
    // blocks all of it.
    *r32p(GICD_CTLR) = 0;
    *r32p(GICC_CTLR) = 0;
    // arch.h's reset contract: every line starts MASKED.
    for (int intid = 0; intid < KICKOS_MAX_IRQ; intid += 32)
    {
        *r32p(GICD_ICENABLER + (intid / 32) * 4) = 0xFFFFFFFFu;
        *r32p(GICD_ICPENDR + (intid / 32) * 4) = 0xFFFFFFFFu;
    }
    *r32p(GICC_PMR) = 0xF0;
    *r32p(GICC_CTLR) = 1;
    *r32p(GICD_CTLR) = 1;

    // This port's mie.MTIE: the timer is in no dispatch table, so no driver unmasks it.
    // Banked, so no target is owed.
    *r8p(GICD_IPRIORITYR + PPI_EL1_PHYS_TIMER) = 0;
    *r32p(GICD_ISENABLER + (PPI_EL1_PHYS_TIMER / 32) * 4) =
        1u << (PPI_EL1_PHYS_TIMER % 32);

    // PSTATE.I stays SET: interrupts first reach the core through the initial thread's SPSR,
    // where RISC-V puts mstatus.MPIE.
}

// A pure read, as the seam requires: the counter is 64-bit and monotonic in hardware, so
// there is no wrap to extend and no anchor to keep, and ticks*16 needs 584 years to overflow.
uint64_t arch_clock_now(void)
{
    return counter_now() * g_ns_per_tick;
}

// CNTP_CVAL_EL0 is an absolute compare, so the write is idempotent and no armed-deadline
// dedup is owed: ktime_rearm calls this on every context switch with the same value. The
// division is what keeps a UINT64_MAX deadline from overflowing, where a multiply would not.
void arch_timer_arm(uint64_t deadline_ns)
{
    uint64_t const ticks = deadline_ns / g_ns_per_tick;
    __asm volatile("msr cntp_cval_el0, %0" ::"r"(ticks));
    __asm volatile("msr cntp_ctl_el0, %0" ::"r"(CNTP_CTL_ENABLE));
    // A deadline already past leaves the compare met, which asserts the timer's output now.
}

// Disarm has to mean no callback fires. Clearing ENABLE deasserts a level-driven output, so
// the GIC's pending state follows it down; ICPENDR covers a pend latched while masked.
void arch_timer_disarm(void)
{
    __asm volatile("msr cntp_ctl_el0, %0" ::"r"(uint64_t(0)));
    *r32p(GICD_ICPENDR + (PPI_EL1_PHYS_TIMER / 32) * 4) = 1u << (PPI_EL1_PHYS_TIMER % 32);
}

// --- Interrupt controller: the GIC behind the mask/unmask/clear triad -------
// Self-bracketed per arch.h at no cost: the enable and pending registers are write-1-to-ACT,
// so no body here read-modify-writes and every store is single and aligned. Unmask writes the
// ENABLE last, so a half-applied sequence leaves the line masked.
void arch_irq_mask(int line)
{
    if (line < 0 or line >= KICKOS_MAX_IRQ)
    {
        return;
    }
    *r32p(GICD_ICENABLER + (line / 32) * 4) = 1u << (line % 32);
}

void arch_irq_unmask(int line)
{
    if (line < 0 or line >= KICKOS_MAX_IRQ)
    {
        return;
    }
    // BYTE per INTID: a word index programs a different interrupt, and a 32-bit access is
    // also unaligned, which on Device memory faults.
    *r8p(GICD_IPRIORITYR + line) = 0;
    if (line >= GIC_BANKED_INTIDS)
    {
        // A global interrupt reaches no core until one is named; a banked one needs none.
        *r8p(GICD_ITARGETSR + line) = 0x01;
    }
    *r32p(GICD_ISENABLER + (line / 32) * 4) = 1u << (line % 32);
}

void arch_irq_clear_pending(int line)
{
    if (line < 0 or line >= KICKOS_MAX_IRQ)
    {
        return;
    }
    *r32p(GICD_ICPENDR + (line / 32) * 4) = 1u << (line % 32);
}

// Test scaffolding (arch.h). ISPENDR pends in the controller, so delivery takes the ordinary
// path and the latch-while-masked contract needs no software shadow.
void arch_irq_inject(int irq)
{
    if (irq < 0 or irq >= KICKOS_MAX_IRQ)
    {
        return;
    }
    *r32p(GICD_ISPENDR + (irq / 32) * 4) = 1u << (irq % 32);
}

// One interrupt per entry: the GIC signals again for anything still pending.
void kickos_armv8a_gic_dispatch(void)
{
    uint32_t const iar = *r32p(GICC_IAR);
    uint32_t const intid = iar & GICC_IAR_ID_MASK;
    if (intid == GICC_IAR_SPURIOUS)
    {
        return; // no EOI is owed for a spurious read
    }
    if (intid == static_cast<uint32_t>(PPI_EL1_PHYS_TIMER))
    {
        // The output is LEVEL, and kickos_isr_timer's re-arm or disarm is what lowers it;
        // an EOI alone would re-enter here forever.
        kickos_isr_timer();
    }
    else
    {
        kickos_isr_irq(static_cast<int>(intid));
    }
    *r32p(GICC_EOIR) = iar;
}

#if KICKOS_MEMORY_ENFORCED
// Rule 7. Only the GIC is here: the timebase is the architected generic timer, reached
// through system registers, and so are the translation controls, so neither is nameable by
// a grant. This machine has no clock or reset gates.
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max)
{
    static struct arch_reserved_block const blocks[] = {
        {GICD_BASE, 0x10000u}, // distributor
        {GICC_BASE, 0x10000u}, // CPU interface
    };
    size_t n = sizeof(blocks) / sizeof(blocks[0]);
    if (n > max)
    {
        n = max;
    }
    for (size_t i = 0; i < n; i++)
    {
        out[i] = blocks[i];
    }
    return n;
}
#endif

// The PL011 comes out of QEMU's reset already enabled at the machine's default baud, so
// the polled path needs no bring-up.
void arch_console_write(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = 0;
        while ((*r32p(UART_FR) & UART_FR_TXFF) != 0 and spin < UART_POLL_BOUND)
        {
            spin++;
        }
        *r32p(UART_DR) = static_cast<uint32_t>(static_cast<unsigned char>(buf[i]));
    }
}

// TXFF says the FIFO can take a byte; BUSY says the device is still clocking one out, which
// is what arch_shutdown and a clock retune actually ask. UNWITNESSABLE HERE, and kept anyway:
// QEMU's PL011 hands each byte to its chardev on the register write, so BUSY never reads set
// and a truncation cannot be produced. Taking the fallback instead would ASSERT that this
// console cannot outrun a shutdown, which the FIFO and shift register make false.
void arch_console_flush_sync(void)
{
    uint32_t spin = 0;
    while ((*r32p(UART_FR) & UART_FR_BUSY) != 0 and spin < UART_POLL_BOUND)
    {
        spin++;
    }
}

// The exit status is what lets the harness tell a fault from a hang: a spin here makes every
// gate that should FAIL time out instead. AArch64 SYS_EXIT takes a POINTER to a two-field
// block where AArch32 passes the reason in the register.
void arch_shutdown(int status)
{
    uint64_t block[2];
    block[0] = ADP_Stopped_ApplicationExit;
    block[1] = static_cast<uint64_t>(static_cast<unsigned>(status));
    semihost(SYS_EXIT, block);
    // Reached only when nothing is listening for semihosting calls.
    __asm volatile("msr daifset, #0xf" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
}

// The shared panic/fault dead-end (kernel.h).
void kfault_terminate(void)
{
    arch_shutdown(132);
}

// startup.S branches here when the handover was not at EL1. Runs before .data and .bss, so
// it touches neither, and inside the boot span, so it reaches neither arch_console_write nor
// kfault_terminate: both are kernel text and only a walk names those.
KICKOS_BOOT_TEXT void kickos_arm64_bad_el(unsigned long el)
{
    char const digit = static_cast<char>('0' + (el & 3));
    boot_console_write(BAD_EL_HEAD, sizeof(BAD_EL_HEAD) - 1);
    boot_console_write(&digit, 1);
    boot_console_write(BAD_EL_TAIL, sizeof(BAD_EL_TAIL) - 1);

    // 16-byte aligned: every access is Device-nGnRnE while the MMU is off, which faults on
    // an unaligned one whatever SCTLR_EL1.A says, and the compiler stores this pair wide.
    uint64_t block[2] __attribute__((aligned(16)));
    block[0] = ADP_Stopped_ApplicationExit;
    block[1] = 132;
    semihost(SYS_EXIT, block);

    // Reached only when nothing is listening for semihosting calls.
    __asm volatile("msr daifset, #0xf" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
}

void Reset_Handler(void)
{
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
    for (uint32_t* b = &__kickos_appbss_start; b < &__kickos_appbss_end; b++)
    {
        *b = 0;
    }
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
