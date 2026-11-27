// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX 8M Plus EVK: a quad Cortex-A53 cluster, the UART the board file names as its console,
// a GIC-500 at 0x3880_0000, and the architected generic timer as the timebase off an 8 MHz system
// counter.
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

#include "board_pins.h"
#include "pin_guard.h"
#include "pinmux.h"

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

    namespace mmap = kickos::imx8mp::mmap;

    // A UART's clock gate, CCM_CCGR73 to CCM_CCGR76 (IMX8MPRM rev 3 Table 5-9, p.284), its
    // clock root, UART1_CLK_ROOT to UART4_CLK_ROOT (Table 5-1, p.241), and its RXD daisy
    // register, the one of its RXD pad.
    struct uart_clock
    {
        uintptr_t base;
        uint32_t ccgr;
        uint32_t root;
        uintptr_t rxd_select;
    };

    constexpr uart_clock UARTS[] = {
        {mmap::UART1_BASE, 73u, 94u, kickos::imx8mp::pinmux::rxd_select(22u)},
        {mmap::UART2_BASE, 74u, 95u, kickos::imx8mp::pinmux::rxd_select(24u)},
        {mmap::UART3_BASE, 75u, 96u, kickos::imx8mp::pinmux::rxd_select(26u)},
        {mmap::UART4_BASE, 76u, 97u, kickos::imx8mp::pinmux::rxd_select(28u)},
    };

    constexpr uart_clock uart_clock_of(uintptr_t base)
    {
        for (uart_clock const& u : UARTS)
        {
            if (u.base == base)
            {
                return u;
            }
        }
        return {0, 0, 0, 0};
    }

    using kickos::imx8mp::pinmux::sw_mux;
    using kickos::imx8mp::pinmux::sw_pad;
#define KICKOS_IMX8MP_MUXED_PAD(port, bit) and (port) == 5u and (bit) >= 22u and (bit) <= 29u
    static_assert(true KICKOS_CHIP_PINS(KICKOS_IMX8MP_MUXED_PAD), "sw_mux addresses GPIO5's pads 22 to 29 alone");
#undef KICKOS_IMX8MP_MUXED_PAD

    constexpr bool pad_listed(uint32_t port, uint32_t pin)
    {
#define KICKOS_CHIP_PIN(p, bit) or (port == (p) and pin == (bit))
        return false KICKOS_CHIP_PINS(KICKOS_CHIP_PIN);
#undef KICKOS_CHIP_PIN
    }

    // CCM_CCGRn_SET (section 5.1.7.7) and CCM_TARGET_ROOTn (section 5.1.7.10, p.412).
    constexpr uintptr_t ccgr_set(uint32_t n)
    {
        return mmap::CCM_BASE + 0x4000u + 0x10u * n + 0x4u;
    }
    constexpr uintptr_t target_root(uint32_t n)
    {
        return mmap::CCM_BASE + 0x8000u + 0x80u * n;
    }
    // SETTING0, the A53 domain's gate: clocks needed all the time.
    constexpr uint32_t CCGR_DOMAIN0_ALWAYS = 0x3u;
    constexpr uint32_t ROOT_ENABLE = 1u << 28;
    constexpr uint32_t ROOT_MUX_SHIFT = 24;
    constexpr uint32_t ROOT_MUX_MASK = 0x7u;
    constexpr uint32_t ROOT_PRE_SHIFT = 16;
    constexpr uint32_t ROOT_PRE_MASK = 0x7u;
    constexpr uint32_t ROOT_POST_MASK = 0x3Fu;
    constexpr uint32_t REF_24M_HZ = 24000000u;

    // A register by its physical address, for the boot span, which runs with the MMU off.
    [[gnu::always_inline]] inline volatile uint32_t* phys(uintptr_t a)
    {
        return reinterpret_cast<volatile uint32_t*>(a);
    }

    // The rate of a UART's root, read live; 0 for a root off any source but 24M_REF_CLK, whose
    // PLLs this port does not model.
    template <volatile uint32_t* (*R)(uintptr_t) = r32p>
    [[gnu::always_inline]] inline uint32_t uart_root_hz(uint32_t root)
    {
        uint32_t const r = *R(target_root(root));
        if ((r & ROOT_ENABLE) == 0u or ((r >> ROOT_MUX_SHIFT) & ROOT_MUX_MASK) != 0u)
        {
            return 0u;
        }
        uint32_t const pre = ((r >> ROOT_PRE_SHIFT) & ROOT_PRE_MASK) + 1u;
        uint32_t const post = (r & ROOT_POST_MASK) + 1u;
        return REF_24M_HZ / pre / post;
    }

    // The console is the UART the board file names. QEMU backs UART1 alone with a chardev, so a
    // console on another UART is silent there rather than faulting.
    constexpr uintptr_t CONSOLE_BASE = KICKOS_BOARD_CONSOLE_BASE;
    constexpr uart_clock CONSOLE_CLOCK = uart_clock_of(CONSOLE_BASE);
    static_assert(CONSOLE_CLOCK.base != 0u, "the board's console is not a UART of this chip");
    static_assert(CONSOLE_CLOCK.rxd_select
                      == kickos::imx8mp::pinmux::rxd_select(KICKOS_BOARD_CONSOLE_RX_BIT),
                  "the console's RX pad is its own UART's");
    static_assert(KICKOS_BOARD_CONSOLE_RX_INPUT_SELECT
                          >> kickos::imx8mp::pinmux::rxd_select_bits(KICKOS_BOARD_CONSOLE_RX_BIT)
                      == 0u,
                  "the console's input select fits its DAISY field");
    constexpr uintptr_t UART_UTXD = CONSOLE_BASE + 0x40;
    constexpr uintptr_t UART_UCR1 = CONSOLE_BASE + 0x80;
    constexpr uintptr_t UART_UCR2 = CONSOLE_BASE + 0x84;
    constexpr uintptr_t UART_USR2 = CONSOLE_BASE + 0x98;
    constexpr uintptr_t UART_UTS = CONSOLE_BASE + 0xB4;

    constexpr uint32_t UCR1_UARTEN = 1u << 0;
    // Held set: SRST is an active-low reset (IMX8MPRM rev 3 section 17.2.14.4), so writing
    // zero resets the module and drops the byte in flight.
    constexpr uint32_t UCR2_SRST = 1u << 0;
    constexpr uint32_t UCR2_RXEN = 1u << 1;
    constexpr uint32_t UCR2_TXEN = 1u << 2;
    constexpr uint32_t UCR2_WS_8BIT = 1u << 5;
    constexpr uint32_t USR2_TXDC = 1u << 3;
    constexpr uint32_t UTS_TXFULL = 1u << 4;

    // UFCR, UBIR and UBMR (IMX8MPRM rev 3 sections 17.2.14.7, 17.2.14.12, 17.2.14.13).
    constexpr uintptr_t UART_UFCR = 0x90;
    constexpr uintptr_t UART_UBIR = 0xA4;
    constexpr uintptr_t UART_UBMR = 0xA8;
    // TXTL 2 and RXTL 1, their reset values, and RFDIV 101b: ref_clk is the root undivided.
    constexpr uint32_t UFCR_VALUE = (2u << 10) | (5u << 7) | 1u;
    constexpr uint32_t UART_BAUD = 115200u;
    // baud = ref_clk / (16 * (UBMR + 1) / (UBIR + 1)): UBIR + 1 = baud / 100 makes
    // UBMR + 1 = ref_clk / 1600, exact for a ref_clk that is a multiple of 1600 Hz.
    constexpr uint32_t BRM_INC = UART_BAUD / 100u - 1u;
    static_assert(UART_BAUD % 100u == 0u and REF_24M_HZ % 1600u == 0u, "the BRM ratio is exact");
    // PE, HYS, PUE and DSE X6. The reset 0x106 pulls the idle-high RXD line down (8.2.4.284).
    constexpr uint32_t UART_PAD_CTL = 0x1C6u;

    // A UART's root on 24M_REF_CLK undivided, its gate open, and 8N1 at UART_BAUD off that root.
    template <volatile uint32_t* (*R)(uintptr_t) = r32p>
    [[gnu::always_inline]] inline void uart_clock_up(uart_clock const& u)
    {
        *R(target_root(u.root)) = ROOT_ENABLE;
        *R(ccgr_set(u.ccgr)) = CCGR_DOMAIN0_ALWAYS;
        uint32_t const ref_hz = uart_root_hz<R>(u.root);
        *R(u.base + UART_UFCR) = UFCR_VALUE;
        // A root that reads back off (QEMU's CCM) leaves the baud as it was.
        if (ref_hz < 1600u)
        {
            return;
        }
        // UBIR first: the BRM takes the pair on the UBMR write.
        *R(u.base + UART_UBIR) = BRM_INC;
        *R(u.base + UART_UBMR) = ref_hz / 1600u - 1u;
    }

    // A bound, not a timing: the flush and the writer sit on the panic and shutdown paths,
    // where a wedged UART must cost a dropped tail rather than a hang.
    constexpr uint32_t UART_POLL_BOUND = 100000;

    // QEMU's imx.serial emits on the UTXD write whatever UCR1 and UCR2 hold, so omitting this
    // goes unnoticed there; on silicon out of reset nothing prints.
    [[gnu::always_inline]] inline void uart_enable(volatile uint32_t* ucr1, volatile uint32_t* ucr2)
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

    // The console's pads, input select, clock and baud, then its transmitter and receiver.
    template <volatile uint32_t* (*R)(uintptr_t)>
    [[gnu::always_inline]] inline void console_up()
    {
        *R(sw_mux(KICKOS_BOARD_CONSOLE_TX_BIT)) = KICKOS_BOARD_CONSOLE_TX_SELECT;
        *R(sw_pad(KICKOS_BOARD_CONSOLE_TX_BIT)) = UART_PAD_CTL;
        *R(sw_mux(KICKOS_BOARD_CONSOLE_RX_BIT)) = KICKOS_BOARD_CONSOLE_RX_SELECT;
        *R(sw_pad(KICKOS_BOARD_CONSOLE_RX_BIT)) = UART_PAD_CTL;
        *R(CONSOLE_CLOCK.rxd_select) = KICKOS_BOARD_CONSOLE_RX_INPUT_SELECT;
        uart_clock_up<R>(CONSOLE_CLOCK);
        uart_enable(R(UART_UCR1), R(UART_UCR2));
    }

    // Each call brings the console up again, so it ends on an idle transmitter: a baud rewrite
    // under a byte in flight would garble it.
    KICKOS_BOOT_TEXT void boot_console_write(char const* buf, size_t n)
    {
        console_up<phys>();
        for (size_t i = 0; i < n; i++)
        {
            uint32_t spin = 0;
            while ((*phys(UART_UTS) & UTS_TXFULL) != 0 and spin < UART_POLL_BOUND)
            {
                spin++;
            }
            *phys(UART_UTXD) = static_cast<uint32_t>(static_cast<unsigned char>(buf[i]));
        }
        uint32_t spin = 0;
        while ((*phys(UART_USR2) & USR2_TXDC) == 0 and spin < UART_POLL_BOUND)
        {
            spin++;
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
    console_up<r32p>();

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
    kickos_armv8a_direct_map_record();
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

// A UART's live root rate; 0 for every other block.
uint32_t arch_periph_clock_hz(uintptr_t base)
{
    uart_clock const u = uart_clock_of(base);
    if (u.base == 0u)
    {
        return 0u;
    }
    return uart_root_hz(u.root);
}

// A UART that is not the console, which arch_init brought up: its root, gate and baud.
int arch_periph_enable(uintptr_t base)
{
    uart_clock const u = uart_clock_of(base);
    if (u.base == 0u or base == CONSOLE_BASE)
    {
        return -KOS_EINVAL;
    }
    uart_clock_up(u);
    return 0;
}

// KOS_SYS_PINMUX_SET over the pads the chip file lists (pinmux.h holds the func encoding). Every
// check runs before any register write.
int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func)
{
    kickos::imx8mp::pinmux::Plan const p = kickos::imx8mp::pinmux::plan(
        port, pin, func, pad_listed(port, pin), kickos::board_pin_kernel_owned(port, pin));
    if (p.rc != 0)
    {
        return p.rc;
    }
    *r32p(p.mux) = p.mux_value;
    if (p.pad != 0u)
    {
        *r32p(p.pad) = p.pad_value;
    }
    if (p.daisy != 0u)
    {
        *r32p(p.daisy) = p.daisy_value;
    }
    return 0;
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
