// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX 8M Plus EVK: a quad Cortex-A53 cluster, UART1 at 0x3086_0000, a GIC-500 at
// 0x3880_0000, and the architected generic timer as the timebase off an 8 MHz system counter.
//
// Entry is at EL3 with no firmware under it, and startup.S stands in for the BL31 that would
// normally have run. Everything here executes at Non-secure EL1.

#include <kickos/arch/arch.h>

#include <kickos/arch/clk_q32.h> // KICKOS_NS_PER_SEC (canonical 1e9 ns/sec)
#include <kickos/chip_limits.h>  // KICKOS_MAX_IRQ: this GIC's interrupt-ID count

#include "gic.h"   // arch/arm64/common: the architected half of this machine's controller
#include "gicv3.h" // arch/arm64/common: which controller this part has, and where

#include <fatal_status.ld.h>

#include <stdint.h>

#if KICKOS_ARM64_GIC_VERSION != 3
#error "imx8mp wires a GIC-500 and implements no other controller: select ARM64_GIC_V3. \
The GICv2 backend would drive a memory-mapped CPU interface this part does not decode, so \
the image would boot and take no interrupt."
#endif

// A count above one would size every per-core array for cores that never arrive. This part
// carries no PSCI of its own (an EVK gets one from the ATF that boots it, and nothing boots
// this image), so on silicon the release is the SRC's, at 0x3039_0000: core 1's reset vector
// base is {SRC_GPR3[15:0], SRC_GPR4[21:2]} at offsets 0x7C and 0x80, its enable is
// SRC_A53RCR1 bit 1 at offset 0x08, and its reset is released through SRC_A53RCR0 at offset
// 0x04 (IMX8MPRM rev 3 sections 6.5.5.2, 6.5.5.3 and 6.5.5.26-6.5.5.33). A released core
// arrives at EL3 with its own vector base, so the release owes the handover startup.S does
// for the primary.
#if KICKOS_NUM_CORES > 1
#error "imx8mp brings up one core: this port has no secondary release. On silicon that is \
SRC_A53RCR1's reset bits plus the entry-point pair in the SRC general-purpose registers, and \
QEMU's imx8mp-evk models the SRC as an unimplemented device and supplies no PSCI, so nothing \
can release a core on that machine."
#endif

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
    // Linker-script symbols (imx8mp.ld).
    extern uint32_t _sidata, _sdata, _edata, _sbss, _ebss;
    // VA - PA for the kernel's half; see dev_va below.
    extern unsigned char __kickos_arm64_va_base[];
    // The app's .bss lives in the low window, outside _sbss.._ebss, so the boot zeroing
    // covers it separately. Its .data needs no copy: the low window links VMA == LMA.
    extern uint32_t __kickos_appbss_start, __kickos_appbss_end;
    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    // Nominal core clock (Hz).
    uint32_t SystemCoreClock = 0;

    void kfault_terminate(void) __attribute__((noreturn));
}

// THE EL REFUSAL PATH IS LINKED INTO THE IDENTITY-MAPPED BOOT SPAN (imx8mp.ld).
// startup.S branches to it with SCTLR_EL1.M still 0, where no walk resolves a high-half
// address, so it names the UART by its physical base rather than through dev_va and may
// not call out of the section: ld's veneer for an out-of-range branch still targets the
// callee's high address.
#define KICKOS_BOOT_TEXT __attribute__((section(".text.init"), noinline, used))
#define KICKOS_BOOT_RODATA __attribute__((section(".rodata.init"), used))

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

    // UART1 (IMX8MPRM rev 3 section 2.5, AIPS3 memory map), which is the one the machine backs
    // with its first chardev. The other three (UART3 at 0x3088_0000, UART2 at 0x3089_0000,
    // UART4 at 0x30A6_0000) are decoded and connected to nothing, so a console written against
    // one of them is silent rather than faulting. WHICH UART CARRIES THE EVK'S DEBUG HEADER IS
    // A SCHEMATIC FACT THE RM DOES NOT CARRY, so on real hardware this is the machine's answer
    // rather than the board's.
    constexpr uintptr_t UART1_BASE = 0x30860000;
    constexpr uintptr_t UART_UTXD = UART1_BASE + 0x40;
    constexpr uintptr_t UART_UCR1 = UART1_BASE + 0x80;
    constexpr uintptr_t UART_UCR2 = UART1_BASE + 0x84;
    constexpr uintptr_t UART_USR2 = UART1_BASE + 0x98;
    constexpr uintptr_t UART_UTS = UART1_BASE + 0xB4;

    constexpr uint32_t UCR1_UARTEN = 1u << 0;
    // SRST IS HELD SET, not written clear: the field is an active-low reset (IMX8MPRM rev 3
    // section 17.2.14.4), so writing zero resets the module and drops the byte in flight. It
    // is also the one bit UCR2 comes out of reset with.
    constexpr uint32_t UCR2_SRST = 1u << 0;
    constexpr uint32_t UCR2_RXEN = 1u << 1;
    constexpr uint32_t UCR2_TXEN = 1u << 2;
    constexpr uint32_t UCR2_WS_8BIT = 1u << 5;
    constexpr uint32_t USR2_TXDC = 1u << 3;
    constexpr uint32_t UTS_TXFULL = 1u << 4;

    // NO BAUD RATE IS PROGRAMMED HERE. It is RefFreq / (16 * (UBMR+1)/(UBIR+1)) with RefFreq
    // the module clock after UFCR.RFDIV (IMX8MPRM rev 3 section 17.2.2), so setting it means
    // bringing up a clock tree this port configures nothing else in; the rate is whatever
    // reset or a preceding boot stage left.

    // A BOUND, not a timing: arch.h requires the flush to be bounded because it sits on the
    // panic and shutdown paths, where a wedged UART must cost a dropped tail rather than a
    // hang. The same reasoning binds the writer below.
    constexpr uint32_t UART_POLL_BOUND = 100000;

    // UNWITNESSABLE ON THIS MACHINE AND CORRECT ON THE PART: QEMU's imx.serial emits on the
    // UTXD write whatever UCR1 and UCR2 hold, so a build that never enabled the transmitter
    // prints exactly the same. On silicon out of reset it prints nothing.
    inline void uart_enable(volatile uint32_t* ucr1, volatile uint32_t* ucr2)
    {
        *ucr1 = UCR1_UARTEN;
        *ucr2 = UCR2_SRST | UCR2_TXEN | UCR2_RXEN | UCR2_WS_8BIT;
    }

    // The refusal path's own writer, with the UART at the address the bus sees: the MMU
    // is off, so dev_va's high alias translates through nothing.
    KICKOS_BOOT_TEXT void boot_console_write(char const* buf, size_t n)
    {
        volatile uint32_t* uts = reinterpret_cast<volatile uint32_t*>(UART_UTS);
        volatile uint32_t* utxd = reinterpret_cast<volatile uint32_t*>(UART_UTXD);
        uart_enable(reinterpret_cast<volatile uint32_t*>(UART_UCR1),
                    reinterpret_cast<volatile uint32_t*>(UART_UCR2));
        for (size_t i = 0; i < n; i++)
        {
            uint32_t spin = 0;
            while ((*uts & UTS_TXFULL) != 0 and spin < UART_POLL_BOUND)
            {
                spin++;
            }
            *utxd = static_cast<uint32_t>(static_cast<unsigned char>(buf[i]));
        }
    }

    // File scope, so they are .rodata for certain: as locals the compiler is free to
    // stage them on the stack through memcpy, and this path runs before the C runtime.
    // In the boot span because the reader runs there.
    KICKOS_BOOT_RODATA char const BAD_EL_HEAD[] = "KickOS: imx8mp-evk entered at EL";
    KICKOS_BOOT_RODATA char const BAD_EL_TAIL[] =
        ", and this port takes over at EL3 or runs at EL1\n";
    char const BAD_CNTFRQ[] =
        "KickOS: imx8mp-evk CNTFRQ_EL0 does not divide a second exactly\n";

    // AArch64 semihosting: `hlt #0xF000` with the operation in x0 and the parameter in x1.
    // ALWAYS inlined: a caller in the boot span can call nothing outside it.
    inline __attribute__((always_inline)) long semihost(long op, void* arg)
    {
        register long x0 __asm("x0") = op;
        register void* x1 __asm("x1") = arg;
        __asm volatile("hlt #0xF000" : "+r"(x0) : "r"(x1) : "memory");
        return x0;
    }

    constexpr long SYS_EXIT = 0x18;
    constexpr uint64_t ADP_Stopped_ApplicationExit = 0x20026u;

    // THE RM GIVES THE 1 MB BLOCK AND NOT THE SUB-LAYOUT: IMX8MPRM rev 3 section 2.2 states
    // 0x3880_0000 to 0x388F_FFFF as "GIC REG" and documents no distributor or redistributor
    // offset anywhere. The split below is the GIC-500's own integration, cross-checked against
    // what the machine decodes: the distributor is the 64 KB frame at the block base, and the
    // redistributors are a contiguous series 0x8_0000 above it. The stride counts the frames
    // the implementation ships, two 64 KB frames per core under GICv3 (IHI 0069H.b section
    // 12.10); a GIC-500 adds no virtual LPI pair, which is what would make it 0x4_0000.
    constexpr uintptr_t GICD_BASE = 0x38800000;
    constexpr uintptr_t GICR_BASE = 0x38880000;
    constexpr uintptr_t GICR_STRIDE = 0x20000;
    // FOUR, BECAUSE THE DIE CARRIES FOUR A53s: one redistributor per core the part implements,
    // decoded whatever KICKOS_NUM_CORES this image was built for. The four frame pairs fill the
    // block from GICR_BASE to its top, which is the one cross-check the RM's 1 MB affords.
    constexpr int GICR_COUNT = 4;
    constexpr uintptr_t GIC_BLOCK_SIZE = 0x100000;
    static_assert(GICR_BASE - GICD_BASE + GICR_COUNT * GICR_STRIDE == GIC_BLOCK_SIZE,
                  "the redistributor series must fill the RM's GIC block above GICR_BASE");

    // The Non-secure EL1 physical timer, and it is that one rather than the Secure 29 because
    // startup.S erets with SCR_EL3.NS set. ARCHITECTURALLY ASSIGNED AND NOT A CHIP FACT: the
    // RM documents no PPI number anywhere, so this rests on the GIC architecture rather than
    // on the part. It is NOT a kernel IRQ line: kickos_isr_timer takes no line and the timer
    // is in no dispatch table.
    constexpr int PPI_EL1_PHYS_TIMER = 30;

    constexpr uint64_t CNTP_CTL_ENABLE = 1u << 0;

    // ns per tick of the architected counter, from CNTFRQ_EL0 at bring-up. The system counter
    // divides its 24 MHz base by three, and CNTFID0 reads 0x007A_1200 for it (IMX8MPRM rev 3
    // section 4.11.4.1.1), so this is exactly 125 and the conversions are lossless.
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

// This part's interrupt controller, which the linked backend reads.
struct kickos_gicv3_map const kickos_gicv3 = {
    GICD_BASE,
    GICR_BASE,
    GICR_STRIDE,
    GICR_COUNT,
    KICKOS_MAX_IRQ,
    PPI_EL1_PHYS_TIMER,
};

// This core's hardware edge alone: the distributor's shared half runs once for the machine.
//
// A core reaches this with PSTATE.DAIF masked, startup.S having eret'd with every bit set.
void kickos_armv8a_percore_init(void)
{
    // CNTP_CTL_EL0's reset value is architecturally UNKNOWN, so an already-asserted timer
    // would fire the moment this core's PPI and DAIF open.
    __asm volatile("msr cntp_ctl_el0, %0" ::"r"(uint64_t(0)));
    // The disable governs the timer's output only past a context synchronisation event. Without
    // this, an output still asserted when the GIC below enables this core's PPI pends the line
    // the write above exists to silence.
    __asm volatile("isb" ::: "memory");

    kickos_armv8a_gic_percore_init();
}

void arch_init(void)
{
    uart_enable(r32p(UART_UCR1), r32p(UART_UCR2));

    uint64_t freq = 0;
    __asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    // CNTFRQ_EL0 is firmware-programmed and startup.S is what programmed it here, so a zero
    // read means that block was skipped on an EL1 handover whose firmware left it alone. A
    // frequency that does not divide a second exactly makes g_ns_per_tick lossy.
    if (freq == 0 or (kickos::KICKOS_NS_PER_SEC % freq) != 0)
    {
        arch_console_write(BAD_CNTFRQ, sizeof(BAD_CNTFRQ) - 1);
        kfault_terminate();
    }
    g_ns_per_tick = kickos::KICKOS_NS_PER_SEC / freq;
    SystemCoreClock = static_cast<uint32_t>(freq);

    kickos_armv8a_gic_dist_init();
    kickos_armv8a_percore_init();

    // PSTATE.I stays SET: interrupts first reach the core through the initial thread's SPSR.
}

// A pure read, as the seam requires: the counter is 64-bit and monotonic in hardware, so
// there is no wrap to extend and no anchor to keep, and ticks*125 needs 74 years to overflow.
uint64_t arch_clock_now(void)
{
    return counter_now() * g_ns_per_tick;
}

// CNTP_CVAL_EL0 is an absolute compare, so the write is idempotent and no armed-deadline
// dedup is owed. The division is what keeps a UINT64_MAX deadline from overflowing, where
// a multiply would not.
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
    // Between the two: the disable reaches the timer's output only past a context
    // synchronisation event, and the Device write below is not ordered against a system-register
    // write by anything else, so a level still asserted re-pends the line behind the clear.
    __asm volatile("isb" ::: "memory");
    kickos_armv8a_gic_clear_pending(PPI_EL1_PHYS_TIMER);
}

// Rule 7. Only the GIC is here: the timebase is the architected generic timer, reached
// through system registers, and so are the translation controls, so neither is nameable by
// a grant. THIS PART HAS CLOCK AND RESET GATES AND THEY ARE ABSENT FROM THIS LIST, which is a
// gap rather than a judgement that granting them is safe: the CCM at 0x3038_0000 and the SRC
// at 0x3039_0000 reach every peripheral on the die, and a domain handed either could stop the
// core it does not own.
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max)
{
    static constexpr struct arch_reserved_block blocks[] = {
        {GICD_BASE, 0x10000u}, // distributor
        // EVERY FRAME PAIR THE DIE CARRIES, not one per core this image drives: the frames a
        // peer's banked interrupt state lives in are exactly what a grant of this window would
        // hand over, and the die decodes all four whether or not a core was released into them.
        {GICR_BASE, GICR_COUNT * GICR_STRIDE},
    };
    // Read back off the entry rather than restated: an entry sized on the image's core count
    // leaves the remaining frame pairs grantable.
    static_assert(blocks[1].base == GICR_BASE
                      and blocks[1].size >= GICR_COUNT * GICR_STRIDE,
                  "the reserved redistributor window must cover every frame pair "
                  "kickos_gicv3.rdist_count declares");
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

void arch_console_write(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = 0;
        while ((*r32p(UART_UTS) & UTS_TXFULL) != 0 and spin < UART_POLL_BOUND)
        {
            spin++;
        }
        *r32p(UART_UTXD) = static_cast<uint32_t>(static_cast<unsigned char>(buf[i]));
    }
}

// TXFULL says the FIFO can take a byte; USR2.TXDC says the transmitter has finished clocking
// one out, which is what arch_shutdown actually asks. UNWITNESSABLE HERE, and kept anyway:
// QEMU hands each byte to its chardev on the register write and raises TXDC with it, so an
// unfinished transmission cannot be produced.
void arch_console_flush_sync(void)
{
    uint32_t spin = 0;
    while ((*r32p(UART_USR2) & USR2_TXDC) == 0 and spin < UART_POLL_BOUND)
    {
        spin++;
    }
}

// The exit status is what lets the harness tell a fault from a hang: a spin here makes every
// gate that should FAIL time out instead. AArch64 SYS_EXIT takes a POINTER to a two-field
// block where AArch32 passes the reason in the register. ON A REAL EVK nothing listens and the
// masked halt below is where this ends.
void arch_shutdown(int status)
{
    uint64_t block[2];
    block[0] = ADP_Stopped_ApplicationExit;
    block[1] = static_cast<uint64_t>(static_cast<unsigned>(status));
    semihost(SYS_EXIT, block);
    __asm volatile("msr daifset, #0xf" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
}

// startup.S branches here when the handover was at neither EL3 nor EL1. Runs before .data and
// .bss, so it touches neither, and inside the boot span, so it reaches neither
// arch_console_write nor kfault_terminate: both are kernel text and only a walk names those.
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
    block[1] = KICKOS_FATAL_STATUS;
    semihost(SYS_EXIT, block);

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
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}
}
