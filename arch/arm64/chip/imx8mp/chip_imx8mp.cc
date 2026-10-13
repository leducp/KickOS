// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX 8M Plus EVK: a quad Cortex-A53 cluster, UART1 at 0x3086_0000, a GIC-500 at
// 0x3880_0000, and the architected generic timer as the timebase off an 8 MHz system counter.
//
// Entry is at EL3 with no firmware under it, and startup.S stands in for the BL31 that would
// normally have run. Everything here executes at Non-secure EL1.

#include <kickos/arch/arch.h>
#include <arch_reboot_tail.h>
#include <kickos/diag.h>
#include <kickos/arch/console_poll.h>
#include "crt_tail.h"
#include <kickos/console_tx.h>

#include <kickos/chip_limits.h>
#include <kickos/chip_mmap.h>

#include <chip_layout.h>

#include "a53.h"
#include "gic.h"
#include "gicv3.h"

#include <kickos/sys/abi.h>

#include <stdint.h>

#if KICKOS_ARM64_GIC_VERSION != 3
#error "imx8mp wires a GIC-500 and implements no other controller: select ARM64_GIC_V3. \
The GICv2 backend would drive a memory-mapped CPU interface this part does not decode, so \
the image would boot and take no interrupt."
#endif

// On silicon the release of a secondary is the SRC's, at 0x3039_0000, with no PSCI under this
// image: core 1's reset vector base is {SRC_GPR3[15:0], SRC_GPR4[21:2]} at offsets 0x7C and
// 0x80, its enable is SRC_A53RCR1 bit 1 at offset 0x08, and its reset is released through
// SRC_A53RCR0 at offset 0x04 (IMX8MPRM rev 3 sections 6.5.5.2, 6.5.5.3 and 6.5.5.26-6.5.5.33).
// A released core arrives at EL3 with its own vector base, so the release owes the handover
// startup.S does for the primary.
#if KICKOS_NUM_CORES > 1
#error "imx8mp brings up one core: this port has no secondary release. On silicon that is \
SRC_A53RCR1's reset bits plus the entry-point pair in the SRC general-purpose registers, and \
QEMU's imx8mp-evk models the SRC as an unimplemented device and supplies no PSCI, so nothing \
can release a core on that machine."
#endif

namespace kickos
{
    void kputs(char const* s);
}

extern "C"
{
    void kfault_terminate(void) __attribute__((noreturn));
    void kpanic_enter(void);
}

// The EL refusal path is linked into the identity-mapped boot span (imx8mp.ld) and runs with
// SCTLR_EL1.M clear: it names the UART by its physical base rather than through dev_va and may
// not call out of the section, ld's veneer for an out-of-range branch still targeting the
// callee's high address.
#define KICKOS_BOOT_TEXT __attribute__((section(".text.init"), noinline, used))
#define KICKOS_BOOT_RODATA __attribute__((section(".rodata.init"), used))

namespace
{
    using kickos::arm64::r32p;

    // UART1 (IMX8MPRM rev 3 section 2.5) is the one QEMU backs with its first chardev; the
    // other three (UART3 at 0x3088_0000, UART2 at 0x3089_0000, UART4 at 0x30A6_0000) are
    // connected to nothing, so a console on one is silent rather than faulting. Which UART
    // carries the EVK's debug header is a schematic fact the RM does not carry.
    using kickos::imx8mp::mmap::UART1_BASE;
    constexpr uintptr_t UART_UTXD = UART1_BASE + 0x40;
    constexpr uintptr_t UART_UCR1 = UART1_BASE + 0x80;
    constexpr uintptr_t UART_UCR2 = UART1_BASE + 0x84;
    constexpr uintptr_t UART_USR2 = UART1_BASE + 0x98;
    constexpr uintptr_t UART_UTS = UART1_BASE + 0xB4;

    constexpr uint32_t UCR1_UARTEN = 1u << 0;
    // Held set: SRST is an active-low reset (IMX8MPRM rev 3 section 17.2.14.4), so writing
    // zero resets the module and drops the byte in flight.
    constexpr uint32_t UCR2_SRST = 1u << 0;
    constexpr uint32_t UCR2_RXEN = 1u << 1;
    constexpr uint32_t UCR2_TXEN = 1u << 2;
    constexpr uint32_t UCR2_WS_8BIT = 1u << 5;
    constexpr uint32_t USR2_TXDC = 1u << 3;
    constexpr uint32_t UTS_TXFULL = 1u << 4;

    // The baud rate is whatever reset or a preceding boot stage left: RefFreq / (16 *
    // (UBMR+1)/(UBIR+1)) (IMX8MPRM rev 3 section 17.2.2) would need a clock tree.

    // A bound, not a timing: the flush and the writer sit on the panic and shutdown paths,
    // where a wedged UART must cost a dropped tail rather than a hang.
    constexpr uint32_t UART_POLL_BOUND = 100000;

    // QEMU's imx.serial emits on the UTXD write whatever UCR1 and UCR2 hold, so omitting this
    // goes unnoticed there; on silicon out of reset nothing prints.
    inline void uart_enable(volatile uint32_t* ucr1, volatile uint32_t* ucr2)
    {
        *ucr1 = UCR1_UARTEN;
        *ucr2 = UCR2_SRST | UCR2_TXEN | UCR2_RXEN | UCR2_WS_8BIT;
    }

    int imx_tx_slot_free(void) { return (*r32p(UART_UTS) & UTS_TXFULL) == 0; }
    void imx_tx_push(uint8_t b) { *r32p(UART_UTXD) = static_cast<uint32_t>(b); }
    void imx_tx_irq_enable(void) {}
    void imx_tx_irq_disable(void) {}

    console_tx_backend const imx_console_backend = {
        imx_tx_slot_free, imx_tx_push, imx_tx_irq_enable, imx_tx_irq_disable,
        &kickos_console_tx_storage};

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

    // File scope: as locals the compiler may stage them on the stack through memcpy, and this
    // path runs before the C runtime. In the boot span because the reader runs there.
    KICKOS_BOOT_RODATA char const BAD_EL_HEAD[] = "KickOS: imx8mp-evk entered at EL";
    KICKOS_BOOT_RODATA char const BAD_EL_TAIL[] =
        ", and this port takes over at EL3 or runs at EL1\n";
    char const BAD_CNTFRQ[] =
        "KickOS: imx8mp-evk CNTFRQ_EL0 does not divide a second exactly\n";

    // IMX8MPRM rev 3 section 2.2 gives 0x3880_0000 to 0x388F_FFFF as "GIC REG" and no
    // sub-layout. The split below is the GIC-500's own integration, cross-checked against what
    // the machine decodes: the distributor is the 64 KB frame at the block base, the
    // redistributors a contiguous series 0x8_0000 above it, two 64 KB frames per core (IHI
    // 0069H.b section 12.10).
    using kickos::imx8mp::mmap::GICD_BASE;
    using kickos::imx8mp::mmap::GICR_BASE;
    constexpr uintptr_t GICR_STRIDE = 0x20000;
    constexpr int GICR_COUNT = KICKOS_LAYOUT_GICR_SIZE / GICR_STRIDE;
}

extern "C"
{

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

    // startup.S programs CNTFRQ_EL0 here; zero means that block was skipped on an EL1
    // handover whose firmware left it alone.
    uint64_t const freq = kickos_armv8a_timebase_init();
    if (freq == 0)
    {
        kpanic_enter();
        kickos::kputs(BAD_CNTFRQ);
        kfault_terminate();
    }

    kickos_armv8a_gic_dist_init();
    kickos_armv8a_percore_init();

    // PSTATE.I stays SET: interrupts first reach the core through the initial thread's SPSR.
}

bool arch_console_write_sync(char const* buf, size_t n)
{
    return kickos::console_poll::write<imx_tx_slot_free, imx_tx_push>(buf, n);
}

console_tx_backend const* arch_console_tx_backend(int* irq_line)
{
    *irq_line = -1;
    return &imx_console_backend;
}

// USR2.TXDC says the transmitter has finished clocking a byte out, which is what arch_shutdown
// asks; QEMU raises it with the register write, so an unfinished transmission cannot be seen
// there. The wait stays because the real FIFO and shifter can outrun a shutdown.
void arch_console_flush_sync(void)
{
    uint32_t spin = 0;
    while ((*r32p(UART_USR2) & USR2_TXDC) == 0 and spin < UART_POLL_BOUND)
    {
        spin++;
    }
}

// WDOG1's software reset (IMX8MPRM rev 3, WDOG WCR): SRS is active low, written with WDE and WDA
// held high, three times over because one 32 kHz period must see two writes (ERR004346).
int arch_reboot(uint32_t outcome)
{
    if (outcome != KOS_SHUTDOWN_REBOOT)
    {
        return -KOS_ENOSYS;
    }
    constexpr uint16_t WCR_WDE = 1u << 2;
    constexpr uint16_t WCR_WDA = 1u << 5;
    volatile uint16_t* const wcr = reinterpret_cast<volatile uint16_t*>(
        kickos::arm64::dev_va(kickos::imx8mp::mmap::WDOG1_BASE));
    __asm volatile("msr daifset, #0xf" ::: "memory");
    for (int i = 0; i < 3; i++)
    {
        *wcr = WCR_WDE | WCR_WDA;
    }
    kickos::arch::reset_settle();
    kickos::arch::reboot_failed(kickos::diag::kResetNotTaken);
}

// Runs before .data and .bss, in the boot span: it reaches neither the console nor
// kfault_terminate, which are kernel text.
KICKOS_BOOT_TEXT void kickos_arm64_bad_el(unsigned long el)
{
    kickos::arm64::bad_el_exit(el, boot_console_write, BAD_EL_HEAD, sizeof(BAD_EL_HEAD) - 1,
                               BAD_EL_TAIL, sizeof(BAD_EL_TAIL) - 1);
}

// Anchors this archive member (arch/CMakeLists.txt, "Seam fallbacks"): startup.S branches
// here, so the link extracts this object while scanning the chip archive, before the arch
// archive that carries arch_console_flush_sync's fallback.
void Reset_Handler(void)
{
    kickos::arm64::crt_ranges_init();
    kickos_crt_tail();
}
}
