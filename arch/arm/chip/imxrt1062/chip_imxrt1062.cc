// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX RT1062 (Teensy 4.1) chip backend, Cortex-M7. RM numbers are IMXRT1060RM Rev. 3.
//
// Boots as a FlexSPI serial-NOR XIP image (RM 9.6/9.7): a 512-byte FlexSPI config block at
// flash offset 0, the IVT at 0x1000, code executing in place from 0x6000_0000, writable state
// in OCRAM2.

#include <kickos/arch/arch.h>
#include <arch_reboot_tail.h>
#include <kickos/arch/bounded_wait.h>
#include <kickos/arch/console_poll.h>
#include "pin_guard.h"
#include <kickos/diag.h>
#include <kickos/arch/clk_q32.h>
#include <kickos/console_tx.h>
#include <kickos/io/mmio.h>
#include <kickos/sys/abi.h>
#include <kickos/usb_console.h>

#include "mpu.h"
#include "regs.h"
#include <kickos/chip_mmap.h>
#include "board_pins.h"
#include "chip_layout.h"
#include "irq.h"
#include "regs/ccm.h"
#include "regs/gpt.h"
#include "regs/iomuxc.h"
#include "regs/gpio.h"
#include "regs/aipstz.h"
#include "regs/lpuart.h"
#include "regs/usbphy.h"
#include "regs/wdog.h"

#include <stddef.h>
#include <stdint.h>

namespace mmap = kickos::imxrt1062::mmap;
namespace reg = kickos::imxrt1062::reg;
namespace irq = kickos::imxrt1062::irq;

namespace kickos
{
    int kmain(int argc, char** argv);
    void kprintf(char const* fmt, ...);
}

extern "C"
{
    void kickos_armv7m_init(void);
    void kickos_armv7m_icache_enable(void);
    void kickos_armv7m_dcache_enable(void);
    void _boot_entry(void);

    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    extern uint32_t g_isr_vector[];       // @ 0x6000_2000, not the flash base
    extern char __boot_image_length[];    // on-flash image extent

    // Core clock (AHB_CLK_ROOT), Hz. clock_init() leaves the boot ROM's CCM tree in place
    // (RM Table 9-7, "ROM Clock Setting"): PLL_ARM 792 MHz, ARM_PODF /2, AHB_PODF /1. SysTick
    // and the DWT both count this clock, so a wrong value skews every timed sleep.
    uint32_t SystemCoreClock = 396000000u;
}

// ===========================================================================
// FlexSPI serial-NOR boot header (RM chapter 9): ROM-consumed data at fixed flash offsets set
// by imxrt1062.ld.
// ===========================================================================
namespace
{
    // FlexSPI configuration block, common part (RM Table 9-15, 448 bytes).
    struct flexspi_mem_config
    {
        uint32_t tag;                     // 0x000 'FCFB'
        uint32_t version;                 // 0x004
        uint32_t reserved0;               // 0x008
        uint8_t readSampleClkSrc;         // 0x00C
        uint8_t csHoldTime;               // 0x00D
        uint8_t csSetupTime;              // 0x00E
        uint8_t columnAddressWidth;       // 0x00F
        uint8_t deviceModeCfgEnable;      // 0x010
        uint8_t reserved1;                // 0x011
        uint16_t waitTimeCfgCommands;     // 0x012
        uint32_t deviceModeSeq;           // 0x014
        uint32_t deviceModeArg;           // 0x018
        uint8_t configCmdEnable;          // 0x01C
        uint8_t reserved2[3];             // 0x01D
        uint32_t configCmdSeqs[3];        // 0x020
        uint32_t reserved3;               // 0x02C
        uint32_t cfgCmdArgs[3];           // 0x030
        uint32_t reserved4;               // 0x03C
        uint32_t controllerMiscOption;    // 0x040
        uint8_t deviceType;               // 0x044
        uint8_t sflashPadType;            // 0x045
        uint8_t serialClkFreq;            // 0x046
        uint8_t lutCustomSeqEnable;       // 0x047
        uint32_t reserved5[2];            // 0x048
        uint32_t sflashA1Size;            // 0x050
        uint32_t sflashA2Size;            // 0x054
        uint32_t sflashB1Size;            // 0x058
        uint32_t sflashB2Size;            // 0x05C
        uint32_t csPadSettingOverride;    // 0x060
        uint32_t sclkPadSettingOverride;  // 0x064
        uint32_t dataPadSettingOverride;  // 0x068
        uint32_t dqsPadSettingOverride;   // 0x06C
        uint32_t timeoutInMs;             // 0x070
        uint32_t commandInterval;         // 0x074
        uint32_t dataValidTime;           // 0x078
        uint16_t busyOffset;              // 0x07C
        uint16_t busyBitPolarity;         // 0x07E
        uint32_t lookupTable[64];         // 0x080
        uint32_t lutCustomSeq[12];        // 0x180
        uint32_t reserved6[4];            // 0x1B0
    };
    static_assert(sizeof(flexspi_mem_config) == 0x1C0, "FCB memConfig must be 448 B");
    static_assert(offsetof(flexspi_mem_config, deviceType) == 0x044, "deviceType @0x44");
    static_assert(offsetof(flexspi_mem_config, serialClkFreq) == 0x046, "serialClkFreq @0x46");
    static_assert(offsetof(flexspi_mem_config, lookupTable) == 0x080, "lookupTable @0x80");

    // Serial-NOR configuration block (RM 9.6.3.2). The RM's Table 9-18 tail is imprecise; this
    // mirrors the ROM's flexspi_nor_config_t so the block is exactly 512 B.
    struct flexspi_nor_config
    {
        flexspi_mem_config memConfig;     // 0x000
        uint32_t pageSize;                // 0x1C0
        uint32_t sectorSize;              // 0x1C4
        uint8_t ipCmdSerialClkFreq;       // 0x1C8
        uint8_t isUniformBlockSize;       // 0x1C9
        uint8_t reserved0[2];             // 0x1CA
        uint8_t serialNorType;            // 0x1CC
        uint8_t needExitNoCmdMode;        // 0x1CD
        uint8_t halfClkForNonReadCmd;     // 0x1CE
        uint8_t needRestoreNoCmdMode;     // 0x1CF
        uint32_t blockSize;               // 0x1D0
        uint32_t reserved1[11];           // 0x1D4
    };
    static_assert(sizeof(flexspi_nor_config) == 0x200, "serial-NOR FCB must be 512 B");

    // Single-pad 0x03 normal read at 30 MHz, no quad-mode enable. LUT instruction =
    // (opcode<<10)|(pads<<8)|operand, two per word (RM Table 9-16). The LUT and serialClkFreq
    // are specific to the board's NOR part.
    __attribute__((section(".boot_fcb"), used))
    const flexspi_nor_config g_flexspi_config = {
        // memConfig
        {
            0x42464346u,   // tag 'FCFB'
            0x56010000u,   // version 'V' 1.0.0
            0,
            0,             // readSampleClkSrc = internal loopback
            3,             // csHoldTime
            3,             // csSetupTime
            0,             // columnAddressWidth
            0, 0, 0,       // deviceModeCfgEnable, reserved1, waitTimeCfgCommands
            0, 0,          // deviceModeSeq, deviceModeArg
            0, {0, 0, 0},  // configCmdEnable, reserved2
            {0, 0, 0}, 0, {0, 0, 0}, 0, // configCmdSeqs, cfgCmdArgs (+reserved)
            0,             // controllerMiscOption
            1,             // deviceType = Serial NOR
            1,             // sflashPadType = single pad
            1,             // serialClkFreq = 30 MHz
            0,             // lutCustomSeqEnable
            {0, 0},
            KICKOS_LAYOUT_FLEXSPI_FLASH_SIZE, // sflashA1Size, the board file's flash
            0, 0, 0,       // A2/B1/B2 size
            0, 0, 0, 0,    // pad-setting overrides
            0, 0, 0,       // timeoutInMs, commandInterval, dataValidTime
            0, 0,          // busyOffset, busyBitPolarity
            {
                0x08180403u, // [0] CMD_SDR 1p 0x03 | RADDR_SDR 1p 0x18(24b)
                0x00002404u, // [1] READ_SDR 1p 0x04 | STOP
            },
            {0},           // lutCustomSeq (unused)
            {0},           // reserved6
        },
        256,   // pageSize
        4096,  // sectorSize
        1,     // ipCmdSerialClkFreq = 30 MHz
        0, {0, 0},
        0, 0, 0, 0,
        0,     // blockSize
        {0},
    };

    // Image Vector Table @ flash+0x1000 (RM Table 9-37). entry -> _boot_entry.
    struct boot_ivt
    {
        uint32_t header;
        uint32_t entry;
        uint32_t reserved1;
        uint32_t dcd;
        uint32_t boot_data;
        uint32_t self;
        uint32_t csf;
        uint32_t reserved2;
    };

    // Boot Data @ flash+0x1020 (RM Table 9-38).
    struct boot_data
    {
        uint32_t start;
        uint32_t length;
        uint32_t plugin;
    };

    __attribute__((section(".boot_data"), used))
    const boot_data g_boot_data = {
        KICKOS_LAYOUT_FLEXSPI_FLASH_BASE,                   // start: image base
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__boot_image_length)), // length
        0,                                                  // plugin flag
    };

    __attribute__((section(".boot_ivt"), used))
    const boot_ivt g_boot_ivt = {
        0x412000D1u,  // header: tag 0xD1, len 0x0020, version 0x41 (RM 9.7.1.1)
        // A pure address constant: the Thumb LSB comes with the function symbol, and an explicit
        // `| 1` would demote the whole IVT to a runtime initializer, a write to XIP flash.
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&_boot_entry)),
        0,
        0,            // dcd: none (ROM defaults; no SDRAM/SEMC)
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_boot_data)),
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_boot_ivt)), // self
        0,            // csf: none (non-secure boot)
        0,
    };
}

namespace
{
    // The part hands over ARMED watchdogs: WDOG1/2 WMCR.PDE is a 16 s power-down counter, and
    // the boot ROM RE-ENABLES the RTWDOG on exit (RM 58.4) with a short LPO timeout. Nothing
    // services them, so all three are disabled first thing at reset.
    void watchdog_disable()
    {
        // 16-bit access ONLY (RM 57.8.1: a 32-bit access is illegal).
        r16(reg::wdog::WDOG1_WMCR) = 0;
        r16(reg::wdog::WDOG2_WMCR) = 0;
        // A reconfig earlier than ~76 us after the ROM exits is silently dropped (RM 58.4), so
        // spin past it, then unlock and clear EN with IRQs masked across the 128-bus-clock
        // window, and CONFIRM via CS.RCS, retrying if the write missed.
        for (volatile uint32_t d = 0; d < 200000u;)
        {
            d = d + 1;
        }
        for (int tries = 0; tries < 8; tries++)
        {
            uint32_t primask;
            __asm volatile("mrs %0, primask" : "=r"(primask));
            __asm volatile("cpsid i" ::: "memory");
            r32(reg::wdog::RTWDOG_CNT) = reg::wdog::RTWDOG_UNLOCK;
            r32(reg::wdog::RTWDOG_TOVAL) = 0x0000FFFFu;
            r32(reg::wdog::RTWDOG_CS) = r32(reg::wdog::RTWDOG_CS) & ~reg::wdog::RTWDOG_CS_EN;
            __asm volatile("msr primask, %0" ::"r"(primask) : "memory");
            uint32_t spin = 0;
            while ((r32(reg::wdog::RTWDOG_CS) & reg::wdog::RTWDOG_CS_RCS) == 0 and ++spin < 100000u)
            {
            }
            if ((r32(reg::wdog::RTWDOG_CS) & reg::wdog::RTWDOG_CS_RCS) != 0)
            {
                break;
            }
            for (volatile uint32_t d = 0; d < 20000u;)
            {
                d = d + 1;
            }
        }
    }

    // Leaves the boot-ROM clock tree untouched: SystemCoreClock and
    // lpuart::UART_CLK_ROOT_HZ are that tree's values.
    void clock_init() {}

    // Monotonic clock: GPT1 free-running, not the DWT, which is debug-domain on the M7. Sourced
    // from ipg_clk_24M, so it stays 24 MHz across an ARM-PLL retune. The 32-bit counter is
    // extended to 64 bits in software and must be read at least once per wrap (~179 s).
    uint32_t g_gpt_hi = 0;   // under the crit section
    uint32_t g_gpt_last = 0;

    uint64_t gpt_ticks()
    {
        // Thread and ISR context: the wrap extension must be atomic against a concurrent reader.
        arch_irq_state_t s = arch_irq_save();
        uint32_t cur = r32(reg::gpt::GPT1_CNT);
        if (cur < g_gpt_last)
        {
            g_gpt_hi++;
        }
        g_gpt_last = cur;
        uint64_t hi = g_gpt_hi;
        arch_irq_restore(s);
        return (hi << 32) | cur;
    }

    void gpt_clock_init()
    {
        r32(reg::ccm::CCGR1) |= reg::ccm::CCGR1_GPT1;
        r32(reg::gpt::GPT1_CR) = 0;               // CLKSRC only changes while EN=0 (RM 52.4)
        r32(reg::gpt::GPT1_CR) = reg::gpt::CR_SWR;
        while ((r32(reg::gpt::GPT1_CR) & reg::gpt::CR_SWR) != 0)
        {
        }
        r32(reg::gpt::GPT1_IR) = 0;
        r32(reg::gpt::GPT1_SR) = 0x3Fu;           // W1C
        r32(reg::gpt::GPT1_PR) = 0;             // PRESCALER=/1, PRESCALER24M=/1 -> 24 MHz
        // Program all config with EN=0, then set EN last (RM 52.6.1).
        uint32_t const cr = reg::gpt::CR_CLKSRC_24M | reg::gpt::CR_EN_24M | reg::gpt::CR_FRR
                          | reg::gpt::CR_ENMOD | reg::gpt::CR_DBGEN | reg::gpt::CR_WAITEN
                          | reg::gpt::CR_DOZEEN | reg::gpt::CR_STOPEN;
        r32(reg::gpt::GPT1_CR) = cr;
        r32(reg::gpt::GPT1_CR) = cr | reg::gpt::CR_EN;
    }

    static_assert(KICKOS_BOARD_CONSOLE_BASE == mmap::LPUART6_BASE,
                  "the board's console is not the LPUART this backend drives");

    constexpr bool pad_listed(uint32_t port, uint32_t pin)
    {
#define KICKOS_CHIP_PIN(p, bit) or (port == (p) and pin == (bit))
        return false KICKOS_CHIP_PINS(KICKOS_CHIP_PIN);
#undef KICKOS_CHIP_PIN
    }
#define KICKOS_CHIP_PIN(p, bit) and ((p) == 1 or (p) == 2)
    static_assert(true KICKOS_CHIP_PINS(KICKOS_CHIP_PIN), "sw_mux addresses the pads of GPIO banks 1 and 2 alone");
#undef KICKOS_CHIP_PIN
    static_assert(KICKOS_BOARD_LED_PORT == 2, "this backend clocks and drives the LED through GPIO2 alone");
    constexpr uint32_t LED_BIT = 1u << KICKOS_BOARD_LED_BIT;

    constexpr uintptr_t led_out(bool level)
    {
        if (level)
        {
            return reg::gpio::GPIO2_DR_SET;
        }
        return reg::gpio::GPIO2_DR_CLEAR;
    }

    constexpr bool LED_LIT = KICKOS_BOARD_LED_ACTIVE_LOW == 0;

    void uart6_init()
    {
        r32(reg::ccm::CCGR3) |= reg::ccm::CCGR3_LPUART6;

        r32(reg::iomuxc::sw_mux(KICKOS_BOARD_CONSOLE_TX_PORT, KICKOS_BOARD_CONSOLE_TX_BIT)) = KICKOS_BOARD_CONSOLE_TX_SELECT;
        r32(reg::iomuxc::sw_mux(KICKOS_BOARD_CONSOLE_RX_PORT, KICKOS_BOARD_CONSOLE_RX_BIT)) = KICKOS_BOARD_CONSOLE_RX_SELECT;
        r32(reg::iomuxc::LPUART6_RX_SELECT_INPUT) = KICKOS_BOARD_CONSOLE_RX_INPUT_SELECT;

        r32(reg::lpuart::LPUART6_CTRL) = 0;                     // disable TX/RX while configuring
        r32(reg::lpuart::LPUART6_GLOBAL) = reg::lpuart::GLOBAL_RST;
        r32(reg::lpuart::LPUART6_GLOBAL) = 0;

        r32(reg::lpuart::LPUART6_BAUD) = reg::lpuart::BAUD_CONSOLE;
        r32(reg::lpuart::LPUART6_CTRL) = reg::lpuart::CTRL_TE | reg::lpuart::CTRL_RE; // TIE stays clear; the ring primes it
    }

    // The USB1 clock tree and PHY, none of which the unprivileged driver can reach (CCM is in
    // arch_reserved_blocks, the PHY is outside the granted window). The boot ROM leaves the
    // CCGR6 gate DISABLED (RM Table 9-6), so without the first write below every register in
    // the driver's window is dead.
    void usb_clock_init()
    {
        r32(reg::usbphy::CCGR6) |= reg::usbphy::CCGR6_USBOH3;

        // PLL_USB1 to 480 MHz (DIV_SELECT 0); the boot ROM normally leaves it locked.
        r32(reg::usbphy::PLL_USB1_SET) = reg::usbphy::PLL_POWER | reg::usbphy::PLL_ENABLE
                                         | reg::usbphy::PLL_EN_USB_CLKS;
        (void)kickos::arch::wait_set(r32(reg::usbphy::PLL_USB1), reg::usbphy::PLL_LOCK);
        r32(reg::usbphy::PLL_USB1_CLR) = reg::usbphy::PLL_BYPASS;

        // The PHY clocks must be running BEFORE PWD is programmed (RM 43.4.1), so that write is
        // last.
        r32(reg::usbphy::PHY1_CTRL_CLR) = reg::usbphy::CTRL_SFTRST;
        r32(reg::usbphy::PHY1_CTRL_CLR) = reg::usbphy::CTRL_CLKGATE;
        r32(reg::usbphy::PHY1_PWD) = reg::usbphy::PWD_ALL_UP;

        // Over-current detection off: the port is gated on that input and the Teensy 4.1 routes
        // no sense to it, so a floating pad could hold the port down. Read-modify-write
        // preserves reserved bits (RM 42.6).
        r32(reg::usbphy::USBNC_OTG1_CTRL) |= reg::usbphy::OTG_CTRL_OVER_CUR_DIS;

        // Read back: a gate that did not take is indistinguishable from one that did until the
        // driver's first access bus-errors. Want ccgr6 low 2 bits set, pll1 LOCK set and BYPASS
        // clear, phyctrl SFTRST/CLKGATE clear, phypwd zero, id 0xE4A1FA05.
        kickos::kprintf("[usbclk] ccgr6=%x pll1=%x phyctrl=%x phypwd=%x id=%x\n",
                        r32(reg::usbphy::CCGR6), r32(reg::usbphy::PLL_USB1),
                        r32(reg::usbphy::PHY1_CTRL), r32(reg::usbphy::PHY1_PWD),
                        r32(kickos::imxrt1062::mmap::USB1_BASE));
    }

#ifdef KICKOS_UART_BEACON
    // Baud beacon: a FIXED SBR (independent of UART_CLK_ROOT_HZ) and 0x55 forever. The host
    // baud that reads clean 0x55 is the on-wire baud, so
    // real_uart_clk = clean_reader_baud * (OSR+1) * BEACON_SBR.
    constexpr uint32_t BEACON_SBR = 11u;
    void uart6_beacon(void)
    {
        r32(reg::lpuart::LPUART6_CTRL) = 0;
        r32(reg::lpuart::LPUART6_BAUD) = (15u << 24) | (BEACON_SBR & 0x1FFFu);
        r32(reg::lpuart::LPUART6_CTRL) = reg::lpuart::CTRL_TE;
        while (true)
        {
            while ((r32(reg::lpuart::LPUART6_STAT) & reg::lpuart::STAT_TDRE) == 0)
            {
            }
            r32(reg::lpuart::LPUART6_DATA) = 0x55u;
        }
    }
#endif

    int lp6_tx_slot_free(void) { return (r32(reg::lpuart::LPUART6_STAT) & reg::lpuart::STAT_TDRE) != 0; }
    void lp6_tx_push(uint8_t b) { r32(reg::lpuart::LPUART6_DATA) = b; }
    void lp6_tx_irq_enable(void) { r32(reg::lpuart::LPUART6_CTRL) |= reg::lpuart::CTRL_TIE; }
    void lp6_tx_irq_disable(void) { r32(reg::lpuart::LPUART6_CTRL) &= ~reg::lpuart::CTRL_TIE; }
    console_tx_backend const lp6_console_backend = {
        lp6_tx_slot_free, lp6_tx_push, lp6_tx_irq_enable, lp6_tx_irq_disable,
        &kickos_console_tx_storage};

    // The window arch_console_reclaim reports: LPUART6's whole AIPS-2 slot (RM Table 3-3), not
    // its 0x30 register block.
    constexpr uintptr_t CONSOLE_WIN_BASE = mmap::LPUART6_BASE;
    constexpr size_t CONSOLE_WIN_SIZE = KICKOS_BOARD_CONSOLE_SIZE;

    static_assert((CONSOLE_WIN_SIZE & (CONSOLE_WIN_SIZE - 1u)) == 0u
                      and (CONSOLE_WIN_BASE % CONSOLE_WIN_SIZE) == 0u,
                  "PMSAv7 needs a power-of-two size on a naturally aligned base");

    static_assert(reg::lpuart::LPUART6_CTRL >= CONSOLE_WIN_BASE
                      and reg::lpuart::LPUART6_CTRL < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                      and reg::lpuart::LPUART6_GLOBAL < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                      and reg::lpuart::LPUART6_BAUD < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE
                      and reg::lpuart::LPUART6_STAT < CONSOLE_WIN_BASE + CONSOLE_WIN_SIZE,
                  "arch_console_reclaim writes outside the window it reports");
}

extern "C"
{

// The M7 caches OCRAM, where the arena lives (DTCM would not be cached). arch_init enables the
// D-cache only under KICKOS_HAVE_MPU, so moving that enable or the arena must change this
// answer too.
int arch_mpu_nocache_support(void)
{
#if KICKOS_HAVE_MPU && defined(KICKOS_IMXRT_DCACHE) && KICKOS_IMXRT_DCACHE
    return ARCH_MPU_NOCACHE_PROGRAMMED;
#else
    return ARCH_MPU_NOCACHE_ALREADY;
#endif
}

void arch_init(void)
{
#if KICKOS_HAVE_MPU
    // ORDER IS LOAD-BEARING (ERR011573): the fixed MPU regions mark the unbacked external
    // bands as Device so the M7 cannot speculatively prefetch into an AHB slave that never
    // responds. They must be LIVE BEFORE the cache is enabled; a cache is what arms that
    // speculation.
    kickos_arm_mpu_fixed_init();
    kickos_armv7m_icache_enable();
#if defined(KICKOS_IMXRT_DCACHE) && KICKOS_IMXRT_DCACHE
    kickos_armv7m_dcache_enable();
#endif
#endif
    clock_init();
    gpt_clock_init();
    uart6_init();
    if (kickos_usb_device_console != 0)
    {
        usb_clock_init(); // after uart6_init: a refusal here must still be able to print
    }
#ifdef KICKOS_UART_BEACON
    uart6_beacon(); // never returns
#endif
    kickos_armv7m_init();
}

#if KICKOS_HAVE_MPU
// Chip fixed MPU regions, in the LOW slots; per-thread grants sit above them. ERR011573 /
// Arm 1013783-B: the default map leaves 0x6000_0000-0x9FFF_FFFF Normal, so M7 speculation
// past the populated flash, or into the unbacked SEMC aperture, hits an AHB slave that never
// responds and stalls the core with NO fault. Both external bands are wrapped Device + XN +
// no-access, and the populated flash overlaid as Normal cacheable priv-RO+X.
extern "C"
{
    size_t kickos_arm_mpu_fixed(struct kickos_arm_mpu_fixed_region const** out)
    {
        using namespace kickos::arm;
        constexpr uint32_t FLASH = KICKOS_LAYOUT_FLEXSPI_FLASH_BASE;
        static_assert((KICKOS_LAYOUT_FLEXSPI_FLASH_SIZE & (KICKOS_LAYOUT_FLEXSPI_FLASH_SIZE - 1)) == 0,
                      "a PMSAv7 region is a power of two");
        // RASR.SIZE: a region of 2^(SIZE + 1) bytes.
        constexpr uint32_t SZ_512M = (29u - 1u) << 1;
        constexpr uint32_t SZ_FLASH = (__builtin_ctz(KICKOS_LAYOUT_FLEXSPI_FLASH_SIZE) - 1u) << 1;
        constexpr uint32_t WRAP = MPU_RASR_ENABLE | SZ_512M | MPU_RASR_AP_NONE | MPU_RASR_XN | MPU_RASR_MEM_DEVICE;
        static kickos_arm_mpu_fixed_region const rows[] = {
            {FLASH, WRAP},                                                               // FlexSPI aperture wrap
            {FLASH, MPU_RASR_ENABLE | SZ_FLASH | MPU_RASR_AP_PRO | MPU_RASR_MEM_NORMAL}, // populated flash, RO+X
            {0x80000000u, WRAP},                                                         // SEMC aperture wrap
        };
        *out = rows;
        return sizeof(rows) / sizeof(rows[0]);
    }
}
#endif

bool arch_console_write_sync(char const* buf, size_t n)
{
    return kickos::console_poll::write<lp6_tx_slot_free, lp6_tx_push>(buf, n);
}

console_tx_backend const* arch_console_tx_backend(int* irq_line)
{
    *irq_line = irq::LPUART6_IRQ;
    return &lp6_console_backend;
}

void arch_console_reclaim_window(uintptr_t* base, size_t* size)
{
    *base = CONSOLE_WIN_BASE;
    *size = CONSOLE_WIN_SIZE;
}

// Panic-path reclaim after a userspace driver may have garbled every register in the window.
// Runs privileged with IRQs masked; MUST be idempotent and re-entrant, so straight-line
// ABSOLUTE stores only, no read-modify-write and no loops. GLOBAL[RST] resets every register
// at once with no minimum delay (RM 49.6.1.4). The pin mux and the CCGR3 gate are out of any
// driver's reach, so neither needs restoring.
void arch_console_reclaim(void)
{
    // Silence first: a stale TIE storms the console IRQ through the whole dump, and CTRL may be
    // altered only with transmitter and receiver disabled (RM 49.6.1.8).
    r32(reg::lpuart::LPUART6_CTRL) = 0;

    r32(reg::lpuart::LPUART6_GLOBAL) = reg::lpuart::GLOBAL_RST;
    r32(reg::lpuart::LPUART6_GLOBAL) = 0;

    // Reset leaves BAUD at 0x0F00_0004: the right OSR and the wrong divisor, so the rate has
    // to be restored explicitly.
    r32(reg::lpuart::LPUART6_BAUD) = reg::lpuart::BAUD_CONSOLE;

    // Last, and TIE stays clear: the reclaimed console is polled, and the ring re-primes the
    // interrupt itself.
    r32(reg::lpuart::LPUART6_CTRL) = reg::lpuart::CTRL_TE | reg::lpuart::CTRL_RE;
}

// Blocks until LPUART6 is transmission-complete: TDRE only means the holding register took the
// byte, TC waits for the last stop bit (RM 49.6.1.7).
void arch_console_flush_sync(void)
{
    (void)kickos::arch::wait_set(r32(reg::lpuart::LPUART6_STAT), reg::lpuart::STAT_TC);
}

void arch_diag_led_init(void)
{
    r32(reg::ccm::CCGR0) |= reg::ccm::CCGR0_GPIO2;
    // GPIO2 and GPIO7 drive the SAME pad and GPR27 bit n picks which (RM 11.3.28); a write to
    // the instance that does not own the bit is silently ignored. Clear = GPIO2.
    r32(reg::iomuxc::GPR27) &= ~LED_BIT;
    r32(reg::iomuxc::sw_mux(KICKOS_BOARD_LED_PORT, KICKOS_BOARD_LED_BIT)) = reg::iomuxc::MUX_ALT5;
    // Dark BEFORE the pin becomes an output, so bring-up never flashes it.
    r32(led_out(not LED_LIT)) = LED_BIT;
    r32(reg::gpio::GPIO2_GDIR) |= LED_BIT;
}

// One absolute store to a write-only register: re-entrant, so it is callable from the dead
// end kfault_terminate reaches after a fault.
void arch_diag_led_set(int on)
{
    if (on != 0)
    {
        r32(led_out(LED_LIT)) = LED_BIT;
    }
    else
    {
        r32(led_out(not LED_LIT)) = LED_BIT;
    }
}

// Daisy (SELECT_INPUT) table, keyed by func bits[15:8]: a SELECT_INPUT belongs to a
// (pad, MUX_MODE) pair, not to a pad.
static uintptr_t const imxrt_daisy[] = {
    reg::iomuxc::LPUART6_RX_SELECT_INPUT, // index 0
};

// KOS_SYS_PINMUX_SET. func encoding:
//   bits[4:0]  = MUX_MODE | SION (SION = bit4), written to SW_MUX_CTL_PAD
//   bit[16]    = has-daisy
//   bits[15:8] = daisy-table index (imxrt_daisy)
//   bits[23:20]= daisy value written to the SELECT_INPUT register
// SW_PAD_CTL stays at its reset default, NOT safe for every pad. All validation happens BEFORE
// any register write.
int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func)
{
    if (not pad_listed(port, pin))
    {
        return -KOS_EINVAL;
    }
    if (kickos::board_pin_kernel_owned(port, pin))
    {
        return -KOS_EBUSY;
    }
    bool const has_daisy = (func & (1u << 16)) != 0u;
    uint32_t const daisy_idx = (func >> 8) & 0xFFu;
    if (has_daisy and daisy_idx >= sizeof(imxrt_daisy) / sizeof(imxrt_daisy[0]))
    {
        return -KOS_EINVAL;
    }
    r32(reg::iomuxc::sw_mux(port, pin)) = func & reg::iomuxc::MUX_FIELD_MASK;
    if (has_daisy)
    {
        r32(imxrt_daisy[daisy_idx]) = (func >> 20) & 0xFu;
    }
    return 0;
}

uint64_t arch_clock_now(void)
{
    uint64_t ticks = gpt_ticks();
    constexpr uint64_t MULT = kickos::arch_clk_recip_q32(reg::gpt::GPT_HZ);
    return kickos::arch_clk_mul_q32(ticks, MULT);
}

// No WFI: the core clock halts under it, and with it SysTick, the wakeup source, so a sleep
// with every thread idle would never wake.
void arch_idle_wait(void)
{
    __asm volatile("nop");
}

// Bridge access only; usb_clock_init did USB1's clock half. Exact base match, never a range: a
// base earns an entry only once the rest of its 16 KiB AIPS slot is reserved in
// arch_reserved_blocks.
int arch_periph_enable(uintptr_t base)
{
    if (base != mmap::USB1_BASE)
    {
        return -KOS_EINVAL;
    }
    uintptr_t const bridge = reg::aipstz::bridge_of(base);
    if (bridge == 0u)
    {
        return -KOS_EINVAL; // outside all four AIPS regions: refuse rather than pick a word
    }
    r32(reg::aipstz::opacr_of(bridge, reg::aipstz::USB1_SLOT)) &=
        ~reg::aipstz::sp_bit_of(reg::aipstz::USB1_SLOT);
    return 0;
}

// The bootloader is HalfKay: the MKL02 companion owns the SWD port, catches the halt and
// presents HalfKay itself. Not vendor-documented. Without an MKL02 the bkpt escalates to a
// fault instead of rebooting.
int arch_reboot(uint32_t outcome)
{
    if (outcome == KOS_SHUTDOWN_REBOOT)
    {
        kickos::arm::system_reset_request();
        kickos::arch::reset_settle();
        kickos::arch::reboot_failed(kickos::diag::kResetNotTaken);
    }
    if (outcome != KOS_SHUTDOWN_BOOTLOADER)
    {
        return -KOS_ENOSYS;
    }
    __asm volatile("cpsid i" ::: "memory"); // dispatch runs in thread mode with IRQs live
    __asm volatile("bkpt #251");
    kickos::arch::reboot_failed(kickos::diag::kRebootImxrt);
}

void Reset_Handler(void)
{
    // MSP was set by _boot_entry: the ROM enters via IVT.entry, not the reset vector. VTOR must
    // name our table (0x6000_2000, not the flash base) before any interrupt path runs.
    watchdog_disable(); // FIRST: the ROM hands off a running RTWDOG (RM 58.4)
    kickos_armv7m_enable_fpu(); // before ANY later code that could emit FP
    r32(kickos::arm::SCB_VTOR) = reinterpret_cast<uintptr_t>(g_isr_vector);
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");

    kickos_ranges_init();
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}

}
