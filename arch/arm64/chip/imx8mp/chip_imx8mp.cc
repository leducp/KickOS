// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX 8M Plus EVK: a quad Cortex-A53 cluster, UART1 at 0x3086_0000, a GIC-500 at
// 0x3880_0000, and the architected generic timer as the timebase off an 8 MHz system counter.
//
// Entry is at EL3 with no firmware under it, and startup.S stands in for the BL31 that would
// normally have run. Everything here executes at Non-secure EL1.

#include <kickos/arch/arch.h>
#include <kickos/console_tx.h>

#include <kickos/chip_limits.h> // KICKOS_MAX_IRQ: this GIC's interrupt-ID count

#include "a53.h"   // arch/arm64/common: the A53 facts and the timer seams both chips share
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
    using kickos::arm64::ADP_Stopped_ApplicationExit;
    using kickos::arm64::halt_masked;
    using kickos::arm64::r32p;
    using kickos::arm64::semihost;
    using kickos::arm64::SYS_EXIT;

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

    // --- Buffered console TX backend (console_tx.h). No TX interrupt is wired here, so
    // irq_line is -1 and the producer drains. ---
    int imx_tx_slot_free(void) { return (*r32p(UART_UTS) & UTS_TXFULL) == 0; }
    void imx_tx_push(uint8_t b) { *r32p(UART_UTXD) = static_cast<uint32_t>(b); }
    void imx_tx_irq_enable(void) {}
    void imx_tx_irq_disable(void) {}

    char console_tx_buf[KICKOS_CONSOLE_TX_SIZE];
    console_tx_backend const imx_console_backend = {
        imx_tx_slot_free, imx_tx_push, imx_tx_irq_enable, imx_tx_irq_disable};

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
    kickos::arm64::PPI_EL1_PHYS_TIMER,
};

void arch_init(void)
{
    uart_enable(r32p(UART_UCR1), r32p(UART_UCR2));

    // CNTFRQ_EL0 is firmware-programmed and startup.S is what programmed it here, so a zero
    // read means that block was skipped on an EL1 handover whose firmware left it alone.
    uint64_t const freq = kickos_armv8a_timebase_init();
    if (freq == 0)
    {
        arch_console_write(BAD_CNTFRQ, sizeof(BAD_CNTFRQ) - 1);
        kfault_terminate();
    }
    SystemCoreClock = static_cast<uint32_t>(freq);

    kickos_armv8a_gic_dist_init();
    kickos_armv8a_percore_init();

    // PSTATE.I stays SET: interrupts first reach the core through the initial thread's SPSR.
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

int arch_console_write(char const* buf, size_t n)
{
    return console_tx_insert_line(buf, n, KICKOS_CONSOLE_CRLF);
}

void arch_console_write_sync(char const* buf, size_t n)
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

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = KICKOS_CONSOLE_TX_SIZE;
    *irq_line = -1; // no TX line is routed here; the producer drains
    return &imx_console_backend;
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

    halt_masked();
}

// ANCHORS THIS ARCHIVE MEMBER (arch/CMakeLists.txt, "Seam fallbacks"). startup.S branches here,
// so the link extracts this object while scanning the chip archive, which the group scans
// before the arch archive that carries arch_console_flush_sync's fallback.
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
