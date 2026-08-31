// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX RT1062 (Teensy 4.1) chip backend, Cortex-M7. Register addresses/fields
// are from the i.MX RT1060 Processor Reference Manual, Rev. 3 (IMXRT1060RM);
// hand-rolled (no vendor CMSIS/SDK), consistent with the arch layer's clean-room
// regs.h.
//
// Boots as a FlexSPI serial-NOR XIP image (RM 9.6/9.7): a 512-byte FlexSPI config
// block at flash offset 0, the IVT at 0x1000, code executing in place from
// 0x6000_0000, writable state in OCRAM2. The LPUART6 console (Teensy "Serial1",
// pins 0/1) baud assumes the reset UART clock root.

#include <kickos/arch/arch.h>
#include <kickos/config/limits.h>
#include <kickos/diag.h>
#include <kickos/arch/clk_q32.h> // shared Q32 tickless-clock reciprocal + multiply
#include <kickos/console_tx.h>
#include <kickos/sys/abi.h> // KOS_E* taxonomy (arch_pinmux_set)

#include "regs.h" // arch/arm/common: kickos_armv7m_enable_fpu + core SCB regs
#include <kickos/chip_mmap.h>
#include "irq.h"
#include "regs/ccm.h"
#include "regs/gpt.h"
#include "regs/iomuxc.h"
#include "regs/gpio.h"
#include "regs/aipstz.h"
#include "regs/lpuart.h"
#if defined(KICKOS_USB_CONSOLE)
#include "regs/usbphy.h"
#endif
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
#if defined(KICKOS_ENABLE_SELFTEST)
    void kpanic(char const* msg) __attribute__((noreturn)); // arch_reboot: the halt must not resume
#endif
}

extern "C"
{
    void kickos_armv7m_init(void);
    void kickos_armv7m_icache_enable(void); // arch/arm/armv7m/cache.cc
    void kickos_armv7m_dcache_enable(void); // (pre-M4)
    void kickos_arm_mpu_fixed_init(void);   // arch/arm/common (programs the fixed regions)
    void _boot_entry(void); // startup.S: sets MSP, jumps to Reset_Handler

    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    extern uint32_t g_isr_vector[];       // startup.S: vector table @ 0x6000_2000
    extern char __boot_image_length[];    // linker: on-flash image extent

    // Core clock (AHB_CLK_ROOT feeding the Cortex-M7 / SysTick / DWT), Hz, CMSIS
    // convention, owned by the chip. clock_init() is deferred, so KickOS inherits
    // the boot ROM's CCM tree, NOT the reset default. RM (IMXRT1060RM rev3) Table 9-7
    // "ROM Clock Setting" fixes that tree; Table 9-5 confirms 396 MHz is the default
    // boot frequency (BOOT_FREQ=0, LPB_BOOT=0). Field-by-field from Table 9-7:
    //   CCM_ANALOG_PLL_ARM = 0x80002042: LOCK|ENABLE, DIV_SELECT[6:0]=0x42=66
    //     -> PLL_ARM = 24 MHz * 66 / 2 = 792 MHz (RM 14.8.1 PLL_ARM formula).
    //   CCM_CACRR = 0x00000001: ARM_PODF[2:0]=1 -> /2 -> 396 MHz (RM 14.7.7).
    //   CCM_CBCMR = 0x75AE8104: PRE_PERIPH_CLK_SEL[19:18]=0b11 -> divided PLL1 (RM 14.7.5).
    //   CCM_CBCDR = 0x000A8200: PERIPH_CLK_SEL[25]=0 -> pre_periph; AHB_PODF[12:10]=0
    //     -> /1 -> AHB_CLK_ROOT = 396 MHz (RM 14.7.4, clock tree Fig 14-2).
    // Both timers count this clock, so 396 MHz makes SysTick (SYST_RVR from
    // SystemCoreClock) and the DWT ns<->cycle math coherent. The old 24 MHz stand-in
    // was ~16.5x low, so SysTick periods were ~16.5x short -> timed sleeps fired early.
    uint32_t SystemCoreClock = 396000000u;
}

// ===========================================================================
// FlexSPI serial-NOR boot header (RM chapter 9). ROM-consumed data placed at
// fixed flash offsets by imxrt1062.ld; KEEP + `used` retain it.
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

    // Serial-NOR configuration block (RM 9.6.3.2, 512 bytes total). The RM's
    // Table 9-18 tail is imprecise; this mirrors the ROM's flexspi_nor_config_t
    // so the whole block is exactly 512 B.
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

    // Single-pad (1-1-1) 0x03 normal read at 30 MHz; no quad-mode enable needed. LUT
    // instruction = (opcode<<10)|(pads<<8)|operand, two per 32-bit word (RM 9.6.3.1
    // note 2 / Table 9-16). seq0 = CMD 0x03 (1-pad) + 24-bit RADDR (1-pad) + READ (1-pad).
    // FLASH-SPECIFIC: validate this LUT + serialClkFreq against the Teensy's flash
    // before flashing (design doc DEFERRED note).
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
            0x00800000u,   // sflashA1Size = 8 MiB (Teensy 4.1 W25Q64)
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
        0x60000000u,                                        // start: image base
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(__boot_image_length)), // length
        0,                                                  // plugin flag
    };

    __attribute__((section(".boot_ivt"), used))
    const boot_ivt g_boot_ivt = {
        0x412000D1u,  // header: tag 0xD1, len 0x0020, version 0x41 (RM 9.7.1.1)
        // entry: &_boot_entry. The Thumb LSB is already set by the function-symbol
        // relocation; an explicit `| 1` would make this non-constant and demote the
        // whole IVT to a runtime initializer (a write to XIP flash -> a 0 entry in
        // the image). Keep it a pure address constant.
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&_boot_entry)),
        0,
        0,            // dcd: none (ROM defaults; no SDRAM/SEMC)
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_boot_data)),
        0x60001000u,  // self: IVT address
        0,            // csf: none (non-secure boot)
        0,
    };
}

// ===========================================================================
// Chip registers + bring-up
// ===========================================================================
namespace
{
    inline volatile uint32_t& r32(uintptr_t a) { return *reinterpret_cast<volatile uint32_t*>(a); }
    inline volatile uint16_t& r16(uintptr_t a) { return *reinterpret_cast<volatile uint16_t*>(a); }

    // --- Watchdogs (RM ch.57 WDOG1/2, ch.58 RTWDOG). The RT1062 hands the app ARMED
    // watchdogs: WDOG1/2 WMCR.PDE (reset 1) is a 16 s power-down counter, and the
    // RTWDOG (WDOG3) resets to CS.EN=1 and the boot ROM RE-ENABLES it on exit (RM
    // 58.4) with a short LPO timeout. KickOS services none of them, so the RTWDOG
    // reset-loops the board (the banner reprints every timeout). Disable all three
    // first thing at reset. ------------------------------------------------------
    void watchdog_disable()
    {
        // WDOG1/2 main timer is WDE=0 (off) at reset; only the 16 s power-down counter
        // needs clearing. 16-bit access ONLY (RM 57.8.1: a 32-bit access is illegal).
        r16(reg::wdog::WDOG1_WMCR) = 0;
        r16(reg::wdog::WDOG2_WMCR) = 0;
        // RTWDOG: an app reconfig only takes effect >= 2.5 LPO(32 kHz) clocks (~76 us)
        // after the ROM exits (RM 58.4); attempted earlier it is silently dropped. Spin
        // past that window, then unlock + clear EN (IRQs masked across the 128-bus-clock
        // window, TOVAL non-zero), and CONFIRM via CS.RCS; retry if the write missed.
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

    // DEFERRED: leave the boot-ROM clock tree untouched (no PLL bring-up). The
    // 600 MHz CCM/ARM-PLL config is a follow-up; see the design doc.
    void clock_init() {}

#if defined(KICKOS_USB_CONSOLE)
    constexpr uint32_t POLL_TIMEOUT_USB = 1000000u;
#endif

    // --- Monotonic clock: GPT1 free-running off the 24 MHz crystal oscillator ----
    // (RM ch.52). The armv7m arch provides NO clock fallback: the DWT is debug-domain
    // and unreliable on the M7 (lockable, absent under a debugger reset). We source
    // GPT1 from ipg_clk_24M (CLKSRC=0b101 + EN_24M, RM Table 52-3), so the counter is
    // fixed at 24 MHz and IMMUNE to any ARM-PLL retune (the 396->600 MHz follow-up),
    // so there is no re-anchor on cpu_clock_set. Free-run 32-bit counter (RM 52.7.1.2
    // FRR=1),
    // extended to 64-bit monotonic ns in software (wraps every 2^32/24e6 ~= 179 s;
    // the scheduler reads far more often, and clocksoak validates multi-wrap).
    uint32_t g_gpt_hi = 0;   // software high word; read/updated under the crit section
    uint32_t g_gpt_last = 0;

    uint64_t gpt_ticks()
    {
        // Runs in thread and ISR context: the wrap-extend read must be atomic
        // against a concurrent reader, so run it under the crit section.
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
        r32(reg::ccm::CCGR1) |= reg::ccm::CCGR1_GPT1; // clock GPT1 (bus + serial)
        r32(reg::gpt::GPT1_CR) = 0;               // CLKSRC only changes while EN=0 (RM 52.4)
        r32(reg::gpt::GPT1_CR) = reg::gpt::CR_SWR; // software reset
        while ((r32(reg::gpt::GPT1_CR) & reg::gpt::CR_SWR) != 0)
        {
        }
        r32(reg::gpt::GPT1_IR) = 0;               // polled clock: no compare/rollover IRQs
        r32(reg::gpt::GPT1_SR) = 0x3Fu;           // W1C: clear any latched status
        r32(reg::gpt::GPT1_PR) = 0;               // PRESCALER=/1, PRESCALER24M=/1 -> 24 MHz
        // Program all config with EN=0, then set EN last (RM 52.6.1).
        uint32_t const cr = reg::gpt::CR_CLKSRC_24M | reg::gpt::CR_EN_24M | reg::gpt::CR_FRR
                          | reg::gpt::CR_ENMOD | reg::gpt::CR_DBGEN | reg::gpt::CR_WAITEN
                          | reg::gpt::CR_DOZEEN | reg::gpt::CR_STOPEN;
        r32(reg::gpt::GPT1_CR) = cr;
        r32(reg::gpt::GPT1_CR) = cr | reg::gpt::CR_EN;
    }

    void uart6_init()
    {
        r32(reg::ccm::CCGR3) |= reg::ccm::CCGR3_LPUART6; // clock LPUART6 (reset already enables it)

        r32(reg::iomuxc::SW_MUX_AD_B0_02) = reg::iomuxc::MUX_ALT2; // TX
        r32(reg::iomuxc::SW_MUX_AD_B0_03) = reg::iomuxc::MUX_ALT2; // RX
        r32(reg::iomuxc::LPUART6_RX_SELECT_INPUT) = reg::iomuxc::RX_DAISY_AD_B0_03;

        r32(reg::lpuart::LPUART6_CTRL) = 0;                     // disable TX/RX while configuring
        r32(reg::lpuart::LPUART6_GLOBAL) = reg::lpuart::GLOBAL_RST; // module software reset
        r32(reg::lpuart::LPUART6_GLOBAL) = 0;

        r32(reg::lpuart::LPUART6_BAUD) = reg::lpuart::BAUD_CONSOLE;
        r32(reg::lpuart::LPUART6_CTRL) = reg::lpuart::CTRL_TE | reg::lpuart::CTRL_RE; // TIE stays clear; the ring primes it
    }

#if defined(KICKOS_USB_CONSOLE)
    // The USB1 clock tree and PHY: the CCGR6 gate, PLL_USB1 and USBPHY1, none of which the
    // unprivileged driver can reach (CCM is in arch_reserved_blocks, the PHY is outside the
    // granted window). RM Table 9-6 lists CCM_CCGR6_CG0 among the gates the boot ROM leaves
    // DISABLED, so without the first write below every register in the driver's window is
    // dead.
    void usb_clock_init()
    {
        r32(reg::usbphy::CCGR6) |= reg::usbphy::CCGR6_USBOH3;

        // PLL_USB1 to 480 MHz. DIV_SELECT stays 0 (Fref * 20). RM Table 9-7 says the boot
        // ROM already leaves this at PLL_ROM_SETTING, so the lock wait normally falls
        // straight through.
        r32(reg::usbphy::PLL_USB1_SET) = reg::usbphy::PLL_POWER | reg::usbphy::PLL_ENABLE
                                         | reg::usbphy::PLL_EN_USB_CLKS;
        for (uint32_t spin = 0; spin < POLL_TIMEOUT_USB; spin++)
        {
            if ((r32(reg::usbphy::PLL_USB1) & reg::usbphy::PLL_LOCK) != 0u)
            {
                break;
            }
        }
        r32(reg::usbphy::PLL_USB1_CLR) = reg::usbphy::PLL_BYPASS;

        // USBPHY1 (RM 43.4.4): out of soft reset, then ungate the UTMI clocks, then power the
        // analog blocks up. RM 43.4.1 requires the PHY clocks to be running BEFORE PWD is
        // programmed, which is why that write is last.
        r32(reg::usbphy::PHY1_CTRL_CLR) = reg::usbphy::CTRL_SFTRST;
        r32(reg::usbphy::PHY1_CTRL_CLR) = reg::usbphy::CTRL_CLKGATE;
        r32(reg::usbphy::PHY1_PWD) = reg::usbphy::PWD_ALL_UP;

        // Over-current detection off: the RT1062 gates the port on that input and the Teensy
        // 4.1 routes no over-current sense to it, so a floating pad could hold the port down.
        // Read-modify-write, not an absolute store: RM 42.6 says to preserve reserved bits.
        r32(reg::usbphy::USBNC_OTG1_CTRL) |= reg::usbphy::OTG_CTRL_OVER_CUR_DIS;

        // Read back, because a gate that did not take is indistinguishable from one that did
        // until the driver's first register access bus-errors. Want: ccgr6 low 2 bits set,
        // pll1 bit31 LOCK set and bit16 BYPASS clear, phyctrl bits 31/30 (SFTRST/CLKGATE)
        // clear, phypwd zero, id 0xE4A1FA05 (the read-only USB1 ID register, RM 42.7.1).
        kickos::kprintf("[usbclk] ccgr6=%x pll1=%x phyctrl=%x phypwd=%x id=%x\n",
                        r32(reg::usbphy::CCGR6), r32(reg::usbphy::PLL_USB1),
                        r32(reg::usbphy::PHY1_CTRL), r32(reg::usbphy::PHY1_PWD),
                        r32(kickos::imxrt1062::mmap::USB1_BASE));
    }
#endif

#ifdef KICKOS_UART_BEACON
    // Baud-beacon diagnostic. Programs
    // LPUART6 BAUD with a FIXED SBR (independent of UART_CLK_ROOT_HZ) and transmits
    // 0x55 ('U', alternating bits) forever. Flash once, then sweep the host reader
    // baud; the reader baud that reads clean 0x55 IS the on-wire baud, so
    //   real_uart_clk = clean_reader_baud * (OSR+1) * BEACON_SBR = clean_reader_baud * 176.
    // BEACON_SBR=11, OSR=15 (16x): at the RM-derived 20 MHz root this is 20e6/(16*11) =
    // 113636 baud, which reads clean at host 115200 (-1.36%, within receiver tolerance).
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

    // --- Buffered console TX backend (console_tx.h) ---
    int lp6_tx_slot_free(void) { return (r32(reg::lpuart::LPUART6_STAT) & reg::lpuart::STAT_TDRE) != 0; }
    void lp6_tx_push(uint8_t b) { r32(reg::lpuart::LPUART6_DATA) = b; }
    void lp6_tx_irq_enable(void) { r32(reg::lpuart::LPUART6_CTRL) |= reg::lpuart::CTRL_TIE; }
    void lp6_tx_irq_disable(void) { r32(reg::lpuart::LPUART6_CTRL) &= ~reg::lpuart::CTRL_TIE; }

    constexpr uint32_t CONSOLE_TX_SIZE = 512; // power of two; > kprintf's 256B buffer
    char console_tx_buf[CONSOLE_TX_SIZE];
    console_tx_backend const lp6_console_backend = {
        lp6_tx_slot_free, lp6_tx_push, lp6_tx_irq_enable, lp6_tx_irq_disable};

    // The window arch_console_reclaim reports: LPUART6's whole AIPS-2 slot, 4019_8000 to
    // 4019_BFFF (RM Table 3-3), not its 0x30 register block.
    constexpr uintptr_t CONSOLE_WIN_BASE = mmap::LPUART6_BASE;
    constexpr size_t CONSOLE_WIN_SIZE = reg::lpuart::LPUART6_WINDOW;

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

// The M7 caches OCRAM, and the arena lives there (imxrt1062.ld RAM at 0x20200000); DTCM
// would not be cached. arch_init enables the D-cache only inside its KICKOS_HAVE_MPU guard,
// so moving either that enable or the arena must change this answer too.
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
    // M7 XIP anti-speculation + L1 caches (ERR011573). ORDER IS LOAD-BEARING: the fixed
    // MPU regions mark the unbacked external Normal bands (FlexSPI beyond the 8 MiB
    // image + the SEMC aperture) as Device, so the M7 cannot speculatively prefetch
    // into an AHB slave that never responds. They must be LIVE BEFORE the cache is
    // enabled; a cache is what arms that speculation.
    kickos_arm_mpu_fixed_init();
    // The D-cache defaults ON (KICKOS_IMXRT_DCACHE, arch/CMakeLists.txt); the coherency
    // obligation arrives with DMA.
    kickos_armv7m_icache_enable();
#if defined(KICKOS_IMXRT_DCACHE) && KICKOS_IMXRT_DCACHE
    kickos_armv7m_dcache_enable();
#endif
#endif
    clock_init();
    gpt_clock_init(); // monotonic clock up before the scheduler reads it
    uart6_init();
#if defined(KICKOS_USB_CONSOLE)
    usb_clock_init(); // after uart6_init: a refusal here must still be able to print
#endif
#ifdef KICKOS_UART_BEACON
    uart6_beacon(); // never returns: raw 0x55 stream for host baud sweep
#endif
    kickos_armv7m_init();
}

#if KICKOS_HAVE_MPU
// Chip fixed (thread-invariant) MPU regions, programmed once into the LOW slots by the
// shared kickos_arm_mpu_fixed_init; per-thread grants sit above them (higher slot wins).
// ERR011573 / Arm 1013783-B: the M7 speculatively prefetches Normal memory, and the
// ARMv7-M default map leaves 0x6000_0000-0x9FFF_FFFF Normal, so speculation past the
// populated 8 MiB of flash, or into the unbacked SEMC aperture, hits an AHB slave that
// never responds and stalls the core with NO fault. Wrap both external Normal bands
// Device + XN + no-access; overlay the real 8 MiB as Normal cacheable priv-RO+X.
// PRIVDEFENA stays on for RAM/peripherals. The row type mirrors arch/arm/common/mpu.h.
extern "C"
{
    struct kickos_arm_mpu_fixed_region
    {
        uint32_t base;
        uint32_t rasr;
    };

    size_t kickos_arm_mpu_fixed(struct kickos_arm_mpu_fixed_region const** out)
    {
        // PMSAv7 RASR: ENABLE | size_field<<1 | AP<<24 | TEX/C/B | XN.
        constexpr uint32_t EN = 1u;
        constexpr uint32_t XN = 1u << 28;
        constexpr uint32_t AP_NONE = 0x0u << 24; // no access (priv + unpriv)
        constexpr uint32_t AP_PRO = 0x5u << 24;  // priv RO, unpriv none
        constexpr uint32_t DEVICE = (1u << 18) | (1u << 16); // shareable Device (non-speculatable)
        constexpr uint32_t NORMAL = (1u << 17) | (1u << 16); // Normal WB cacheable
        constexpr uint32_t SZ_512M = (29u - 1u) << 1;
        constexpr uint32_t SZ_8M = (23u - 1u) << 1; // == LENGTH(FLASH), the populated image
        static kickos_arm_mpu_fixed_region const rows[] = {
            {0x60000000u, EN | SZ_512M | AP_NONE | XN | DEVICE}, // FlexSPI aperture wrap
            {0x60000000u, EN | SZ_8M | AP_PRO | NORMAL},         // populated-flash overlay (RO+X)
            {0x80000000u, EN | SZ_512M | AP_NONE | XN | DEVICE}, // SEMC aperture wrap
        };
        *out = rows;
        return sizeof(rows) / sizeof(rows[0]);
    }
}
#endif

void arch_console_write(char const* buf, size_t n)
{
    console_tx_write(buf, n); // buffered; the routing guard keeps this thread-only
}

void arch_console_write_sync(char const* buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
    {
        uint32_t spin = 0;
        while ((r32(reg::lpuart::LPUART6_STAT) & reg::lpuart::STAT_TDRE) == 0)
        {
            if (++spin > KICKOS_POLL_SPIN_MAX)
            {
                return; // bounded: a wedged UART must not hang the panic path (drop)
            }
        }
        r32(reg::lpuart::LPUART6_DATA) = static_cast<uint8_t>(buf[i]);
    }
}

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = CONSOLE_TX_SIZE;
    *irq_line = irq::LPUART6_IRQ;
    return &lp6_console_backend;
}

void arch_console_reclaim_window(uintptr_t* base, size_t* size)
{
    *base = CONSOLE_WIN_BASE;
    *size = CONSOLE_WIN_SIZE;
}

// Panic-path reclaim (console.cc D6): force LPUART6 back to a polled-ready 8N1 TX channel
// after a userspace driver may have garbled every writable register in the window. Runs with
// IRQs masked, privileged; MUST be idempotent and re-entrant, so straight-line ABSOLUTE
// stores only, no read-modify-write and no loops.
// RM 49.6.1.4: GLOBAL[RST] "resets all internal logic and registers, except the Global
// Register", immediately and with "no minimum delay required before clearing", so one store
// restores the whole register file and discards whatever a dead driver left queued.
// The pin mux (arch_pinmux_set refuses GPIO1.IO02/03) and the CCGR3 gate in CCM are both out
// of any driver's reach, so neither needs restoring.
void arch_console_reclaim(void)
{
    // Silence and stop first: a stale TIE storms the console IRQ through the whole dump, and
    // RM 49.6.1.8 requires CTRL to be altered only with transmitter and receiver disabled.
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

// Console coherence (arch.h): block until LPUART6 is transmission-complete. STAT[TDRE]
// clear only means the holding register took the byte; STAT[TC] stays clear until the last
// stop bit has left the shift register and the pin has gone idle (RM 49.6.1.7).
void arch_console_flush_sync(void)
{
    uint32_t spin = 0;
    while ((r32(reg::lpuart::LPUART6_STAT) & reg::lpuart::STAT_TC) == 0)
    {
        if (++spin > KICKOS_POLL_SPIN_MAX)
        {
            return; // bounded, as arch.h requires: a wedged UART drops the tail, never hangs
        }
    }
}

// Kernel diagnostic LED: GPIO2.IO03, pad GPIO_B0_03 at ALT5, active-high.
void arch_diag_led_init(void)
{
    r32(reg::ccm::CCGR0) |= reg::ccm::CCGR0_GPIO2;
    // GPIO2 and GPIO7 drive the SAME pad and GPR27 bit n picks which (RM 11.3.28); a write
    // to the instance that does not own the bit is silently ignored. Bit clear = GPIO2, the
    // instance regs/gpio.h maps.
    r32(reg::iomuxc::GPR27) &= ~reg::gpio::DIAG_LED_BIT;
    r32(reg::iomuxc::SW_MUX_B0_03) = reg::iomuxc::MUX_ALT5;
    // Dark BEFORE the pin becomes an output, so bring-up never flashes it.
    r32(reg::gpio::GPIO2_DR_CLEAR) = reg::gpio::DIAG_LED_BIT;
    r32(reg::gpio::GPIO2_GDIR) |= reg::gpio::DIAG_LED_BIT;
}

// One absolute store to a write-only register: re-entrant, so it is callable from the dead
// end kfault_terminate reaches after a fault.
void arch_diag_led_set(int on)
{
    if (on != 0)
    {
        r32(reg::gpio::GPIO2_DR_SET) = reg::gpio::DIAG_LED_BIT;
    }
    else
    {
        r32(reg::gpio::GPIO2_DR_CLEAR) = reg::gpio::DIAG_LED_BIT;
    }
}

// Pad-mux table for KOS_SYS_PINMUX_SET. Selector is the datasheet-natural pair
// (port = GPIO bank 1..5, pin = bit within the bank); a 1:1 pad<->GPIO position.
// Value = the pad's SW_MUX_CTL_PAD address. 0 = hole (unbonded / not tabled) ->
// EINVAL. The table is INTENTIONALLY PARTIAL: only GPIO1.IO00..05 and GPIO2.IO00..03
// are named (regs/iomuxc.h). An un-tabled pad hard-fails EINVAL, never a silent write.
static uintptr_t const imxrt_pad_mux[6][32] = {
    {}, // bank 0 unused (GPIO banks are 1-based)
    {   // GPIO1 = GPIO_AD_B0_xx (IO00..IO15) / GPIO_AD_B1_xx (IO16..IO31)
        reg::iomuxc::SW_MUX_AD_B0_00, reg::iomuxc::SW_MUX_AD_B0_01,
        reg::iomuxc::SW_MUX_AD_B0_02, reg::iomuxc::SW_MUX_AD_B0_03,
        reg::iomuxc::SW_MUX_AD_B0_04, reg::iomuxc::SW_MUX_AD_B0_05,
    },
    {   // GPIO2 = GPIO_B0_xx (IO00..IO15) / GPIO_B1_xx (IO16..IO31)
        reg::iomuxc::SW_MUX_B0_00, reg::iomuxc::SW_MUX_B0_01,
        reg::iomuxc::SW_MUX_B0_02, reg::iomuxc::SW_MUX_B0_03,
    },
};

// Daisy (SELECT_INPUT) table, keyed by func's OWN index bits[15:8], NOT parallel to
// the pad table: a SELECT_INPUT belongs to a (pad, MUX_MODE) pair, not to a pad.
static uintptr_t const imxrt_daisy[] = {
    reg::iomuxc::LPUART6_RX_SELECT_INPUT, // index 0
};

// Kernel-owned pads arch_pinmux_set refuses for life, so a board map cannot dark the console
// or steal the diagnostic LED. GPIO1.IO02/03 (= GPIO_AD_B0_02/03) are the LPUART6 console
// pads; GPIO2.IO03 (= GPIO_B0_03) is the diag LED.
static bool imxrt_pin_kernel_owned(uint32_t port, uint32_t pin)
{
    if (port == 1u and (pin == 2u or pin == 3u))
    {
        return true;
    }
    return port == 2u and pin == 3u;
}

// One-shot pin-function config (KOS_SYS_PINMUX_SET). func encoding:
//   bits[4:0]  = MUX_MODE | SION (SION = bit4), written to SW_MUX_CTL_PAD
//   bit[16]    = has-daisy
//   bits[15:8] = daisy-table index (imxrt_daisy)
//   bits[23:20]= daisy value written to the SELECT_INPUT register
// SW_PAD_CTL is left at reset defaults. That is fine for the console-class route and
// this narrow exercise, but is NOT safe generically (drive/pull/hysteresis
// depend on the pad + net). All range/ownership/index validation happens BEFORE any
// register write (no half-applied pad on a rejected request).
int arch_pinmux_set(uint32_t port, uint32_t pin, uint32_t func)
{
    if (port < 1u or port > 5u or pin > 31u)
    {
        return -KOS_EINVAL;
    }
    uintptr_t const pad = imxrt_pad_mux[port][pin];
    if (pad == 0u)
    {
        return -KOS_EINVAL; // hole: unbonded or not tabled
    }
    if (imxrt_pin_kernel_owned(port, pin))
    {
        return -KOS_EBUSY;
    }
    bool const has_daisy = (func & (1u << 16)) != 0u;
    uint32_t const daisy_idx = (func >> 8) & 0xFFu;
    if (has_daisy and daisy_idx >= sizeof(imxrt_daisy) / sizeof(imxrt_daisy[0]))
    {
        return -KOS_EINVAL;
    }
    r32(pad) = func & reg::iomuxc::MUX_FIELD_MASK;
    if (has_daisy)
    {
        r32(imxrt_daisy[daisy_idx]) = (func >> 20) & 0xFu;
    }
    return 0;
}

// Monotonic clock (arch.h contract; the armv7m layer provides no fallback). GPT1
// 24 MHz ticks -> ns. Fixed 24 MHz, so the reciprocal-multiply constant is compile-
// time (no per-read divide, no re-anchor across an ARM-PLL retune).
uint64_t arch_clock_now(void)
{
    uint64_t ticks = gpt_ticks();
    // ns = ticks * 1e9 / 24e6, the divide folds at build time (GPT_HZ is constant).
    constexpr uint64_t MULT = kickos::arch_clk_recip_q32(reg::gpt::GPT_HZ);
    return kickos::arch_clk_mul_q32(ticks, MULT);
}

// Replaces the WFI idle fallback. The tickless wakeup timer is SysTick, clocked off the
// core clock, which the RT106x halts under WFI: SysTick stops counting and a sleep with
// every thread idle never wakes (the GPT monotonic clock keeps running, but it is not
// the wakeup source). Spin so the core clock, and thus SysTick, stays alive. A GPT
// output-compare wakeup would allow WFI (GPT counts through WAIT via CR_WAITEN).
void arch_idle_wait(void)
{
    __asm volatile("nop");
}

// See arch.h. Bridge access only; usb_clock_init already did USB1's clock half. Exact base
// match, never a range: a base earns an entry only once the rest of its 16 KiB AIPS slot is
// reserved in arch_reserved_blocks.
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

#if defined(KICKOS_ENABLE_SELFTEST)
// Reboot into HalfKay (Teensy firmware-download mode): the MKL02 companion owns this
// chip's SWD port, catches the halt, reprograms the flash and presents HalfKay itself.
// Not vendor-documented (evidence: PJRC's _reboot_Teensyduino_ plus a third-party
// bare-metal Rust port).
// On non-Teensy RT1062 hardware (no MKL02, no armed debug host) the bkpt escalates to a
// fault instead of rebooting.
int arch_reboot(void)
{
    __asm volatile("cpsid i" ::: "memory"); // dispatch runs in thread mode with IRQs live
    __asm volatile("bkpt #251");
    kickos::kpanic(kickos::diag::kRebootImxrt);
}
#endif

#if KICKOS_HAVE_MPU
// Rule 7 reserved set (RT1060 RM). Owns-for-life: the GPT1 monotonic time base and
// the CCM (CCGR clock-gate roots). Bases are the constants above; sizes are the 4 KB
// register block of each peripheral (an AIPS slot itself is 16 KiB). M7 has NO
// bit-band, so arch_bitband_present keeps the fallback 0.
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max)
{
    static struct arch_reserved_block const blocks[] = {
        {mmap::GPT1_BASE, 0x1000u}, // GPT1: monotonic time base (RM ch.52, Table 3-3)
        {mmap::CCM_BASE, 0x1000u},  // CCM: CCGR clock-gate roots (RM ch.14)
        // One cleared Supervisor-Protect nibble opens a whole 16 KiB peripheral slot to every
        // unprivileged thread, and these config blocks are not themselves OPAC-gated.
        {mmap::AIPSTZ1_BASE, 0x1000u}, // AIPS-1: MPR + OPACR0..4 (RM ch.32)
        {mmap::AIPSTZ2_BASE, 0x1000u}, // AIPS-2
        {mmap::AIPSTZ3_BASE, 0x1000u}, // AIPS-3, the bridge USB1 sits behind
        {mmap::AIPSTZ4_BASE, 0x1000u}, // AIPS-4
        // The rest of USB1's AIPS slot: the bridge's unit is 16 KiB and holds OTG2 at +0x200
        // and USBNC at +0x800, so arch_periph_enable necessarily opens those too.
        {mmap::USB1_BASE + 0x200u, 0x3E00u},
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

void Reset_Handler(void)
{
    // MSP was set by _boot_entry (the ROM enters via IVT.entry, not the reset
    // vector). Point VTOR at our table (@ 0x6000_2000, not flash base) before any
    // interrupt path runs.
    watchdog_disable(); // FIRST: the ROM hands off a running RTWDOG (RM 58.4)
    kickos_armv7m_enable_fpu(); // before ANY later code that could emit FP (softfp ABI)
    r32(0xE000ED08) = reinterpret_cast<uintptr_t>(g_isr_vector); // SCB->VTOR
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");

    kickos_ranges_init(); // init .data (copy from FlexSPI LMA); zero .bss
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}

}
