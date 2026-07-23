// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// NXP i.MX RT1062 (Teensy 4.1) chip backend, Cortex-M7. Register addresses/fields
// are from the i.MX RT1060 Processor Reference Manual, Rev. 3 (IMXRT1060RM);
// hand-rolled (no vendor CMSIS/SDK), consistent with the arch layer's clean-room
// regs.h.
//
// FIRST-BRING-UP SCOPE (docs/design-teensy-rt1062.md): privilege + SVC only, no
// hardware MPU, no L1 cache, no PLL bring-up. Boots as a FlexSPI serial-NOR XIP
// image (RM 9.6/9.7): a 512-byte FlexSPI config block at flash offset 0, the IVT
// at 0x1000, code executing in place from 0x6000_0000, writable state in OCRAM2.
// clock_init() leaves whatever clock the boot ROM configured and reports a
// conservative SystemCoreClock; the 600 MHz CCM/PLL tree is a follow-up. The
// LPUART6 console (Teensy "Serial1", pins 0/1) baud assumes the reset UART clock
// root. NOT run in this environment (no board/QEMU model); verified by build +
// image inspection. Flash via HalfKay to confirm. Points to validate on bench if
// bring-up misbehaves: the FCB read LUT/speed (flash-specific), the UART clock
// root (baud), and the actual post-ROM core frequency (SystemCoreClock).

#include <kickos/arch/arch.h>
#include <kickos/config/limits.h>
#include <kickos/arch/clk_q32.h> // shared Q32 tickless-clock reciprocal + multiply
#include <kickos/console_tx.h>

#include <stddef.h>
#include <stdint.h>

namespace kickos
{
    int kmain(int argc, char** argv);
    void kprintf(char const* fmt, ...);
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

    // Single-pad (1-1-1) 0x03 normal read at 30 MHz -- the universally-compatible
    // read for a first bring-up (no quad-mode enable needed). LUT instruction =
    // (opcode<<10)|(pads<<8)|operand; two per 32-bit word (RM 9.6.3.1 note 2 /
    // Table 9-16). seq0 = CMD 0x03 (1-pad) + 24-bit RADDR (1-pad) + READ (1-pad).
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
        // relocation -- an explicit `| 1` would make this non-constant and demote
        // the whole IVT to a runtime initializer (a write to XIP flash -> a 0 entry
        // in the image). Keep it a pure address constant.
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

    // --- CCM clock gating (RM 14.7) ---
    constexpr uintptr_t CCM_CCGR3 = 0x400FC074;      // lpuart6 = CG3 [7:6]
    constexpr uint32_t CCGR3_LPUART6 = 3u << 6;

    // --- IOMUXC pin mux (RM ch.11) ---
    constexpr uintptr_t IOMUXC_SW_MUX_AD_B0_02 = 0x401F80C4; // Teensy pin 1 = TX1
    constexpr uintptr_t IOMUXC_SW_MUX_AD_B0_03 = 0x401F80C8; // Teensy pin 0 = RX1
    constexpr uint32_t MUX_ALT2 = 2u;                 // ALT2 = LPUART6_TX / _RX
    constexpr uintptr_t IOMUXC_LPUART6_RX_SELECT_INPUT = 0x401F84E4; // daisy (RM ch.11)
    constexpr uint32_t RX_DAISY_AD_B0_03 = 1u;

    // --- LPUART6 (RM: Low Power UART); base from AIPS-2 map (RM Table 3-3) ---
    constexpr uintptr_t LPUART6_BASE = 0x40198000;
    constexpr uintptr_t LPUART6_GLOBAL = LPUART6_BASE + 0x08;
    constexpr uintptr_t LPUART6_BAUD = LPUART6_BASE + 0x10;
    constexpr uintptr_t LPUART6_STAT = LPUART6_BASE + 0x14;
    constexpr uintptr_t LPUART6_CTRL = LPUART6_BASE + 0x18;
    constexpr uintptr_t LPUART6_DATA = LPUART6_BASE + 0x1C;
    constexpr uint32_t GLOBAL_RST = 1u << 1;
    constexpr uint32_t STAT_TDRE = 1u << 23;
    constexpr uint32_t CTRL_TIE = 1u << 23;
    constexpr uint32_t CTRL_TE = 1u << 19;
    constexpr uint32_t CTRL_RE = 1u << 18;

    // LPUART clock root: 20 MHz. clock_init() is deferred (we inherit the tree the
    // boot ROM / HalfKay left). The ROM's CCM handoff state is fixed: RM Table 9-7
    // "ROM Clock Setting" sets CCM_CSCDR1 = 0x06490B03, i.e. UART_CLK_SEL=0 (bit 6 ->
    // pll3_80m) and UART_CLK_PODF=0b000011 = divide-by-4 (RM 14.7.9). With PLL_USB1
    // up (RM Table 9-7 CCM_ANALOG_PLL_USB1 = 0x8000_3040: LOCK|POWER|ENABLE, DIV_SELECT=0
    // -> 480 MHz), pll3_80m = 480/6 = 80 MHz, so uart_clk_root = 80/4 = 20 MHz (clock
    // tree RM Fig 14-3). This is why BOTH earlier guesses gave garbage: neither the
    // reset-default 80 MHz (PODF ignored) nor 24 MHz (wrong mux) is what the ROM leaves.
    constexpr uint32_t UART_CLK_ROOT_HZ = 20000000u;

    // NVIC: LPUART6 combined TX/RX = IRQ 25 (RM Table 4-2).
    constexpr int LPUART6_IRQ = 25;

    inline volatile uint16_t& r16(uintptr_t a) { return *reinterpret_cast<volatile uint16_t*>(a); }

    // --- Watchdogs (RM ch.57 WDOG1/2, ch.58 RTWDOG). The RT1062 hands the app ARMED
    // watchdogs: WDOG1/2 WMCR.PDE (reset 1) is a 16 s power-down counter, and the
    // RTWDOG (WDOG3) resets to CS.EN=1 and the boot ROM RE-ENABLES it on exit (RM
    // 58.4) with a short LPO timeout. KickOS services none of them, so the RTWDOG
    // reset-loops the board (the banner reprints every timeout). Disable all three
    // first thing at reset. ------------------------------------------------------
    constexpr uintptr_t WDOG1_WMCR = 0x400B8008;
    constexpr uintptr_t WDOG2_WMCR = 0x400D0008;
    constexpr uintptr_t RTWDOG_CS = 0x400BC000;
    constexpr uintptr_t RTWDOG_CNT = 0x400BC004;
    constexpr uintptr_t RTWDOG_TOVAL = 0x400BC008;
    constexpr uint32_t RTWDOG_UNLOCK = 0xD928C520u; // RM 58.3.2.2.1 unlock key
    constexpr uint32_t RTWDOG_CS_EN = 1u << 7;
    constexpr uint32_t RTWDOG_CS_RCS = 1u << 10; // reconfig-success flag

    void watchdog_disable()
    {
        // WDOG1/2 main timer is WDE=0 (off) at reset; only the 16 s power-down counter
        // needs clearing. 16-bit access ONLY (RM 57.8.1: a 32-bit access is illegal).
        r16(WDOG1_WMCR) = 0;
        r16(WDOG2_WMCR) = 0;
        // RTWDOG: an app reconfig only takes effect >= 2.5 LPO(32 kHz) clocks (~76 us)
        // after the ROM exits (RM 58.4); attempted earlier it is silently dropped. Spin
        // past that window, then unlock + clear EN (IRQs masked across the 128-bus-clock
        // window, TOVAL non-zero), and CONFIRM via CS.RCS -- retry if the write missed.
        for (volatile uint32_t d = 0; d < 200000u; d++)
        {
        }
        for (int tries = 0; tries < 8; tries++)
        {
            uint32_t primask;
            __asm volatile("mrs %0, primask" : "=r"(primask));
            __asm volatile("cpsid i" ::: "memory");
            r32(RTWDOG_CNT) = RTWDOG_UNLOCK;
            r32(RTWDOG_TOVAL) = 0x0000FFFFu;
            r32(RTWDOG_CS) = r32(RTWDOG_CS) & ~RTWDOG_CS_EN;
            __asm volatile("msr primask, %0" ::"r"(primask) : "memory");
            uint32_t spin = 0;
            while ((r32(RTWDOG_CS) & RTWDOG_CS_RCS) == 0 and ++spin < 100000u)
            {
            }
            if ((r32(RTWDOG_CS) & RTWDOG_CS_RCS) != 0)
            {
                break;
            }
            for (volatile uint32_t d = 0; d < 20000u; d++)
            {
            }
        }
    }

    void enable_fpu()
    {
        // M7 CPACR CP10/CP11 full access. FP context is enabled even under the
        // softfp ABI (a hard-float TU could emit FP; CPACR-off FP faults).
        volatile uint32_t* cpacr = reinterpret_cast<volatile uint32_t*>(0xE000ED88);
        *cpacr |= (0xFu << 20);
        __asm volatile("dsb" ::: "memory");
        __asm volatile("isb" ::: "memory");
    }

    // DEFERRED: leave the boot-ROM clock tree untouched (no PLL bring-up). The
    // 600 MHz CCM/ARM-PLL config is a follow-up; see the design doc.
    void clock_init() {}

    // --- Monotonic clock: GPT1 free-running off the 24 MHz crystal oscillator ----
    // (RM ch.52). The armv7m arch provides NO clock fallback: the DWT is debug-domain
    // and unreliable on the M7 (lockable, absent under a debugger reset). We source
    // GPT1 from ipg_clk_24M (CLKSRC=0b101 + EN_24M, RM Table 52-3), so the counter is
    // fixed at 24 MHz and IMMUNE to any ARM-PLL retune (the 396->600 MHz follow-up) --
    // no re-anchor on cpu_clock_set. Free-run 32-bit counter (RM 52.7.1.2 FRR=1),
    // extended to 64-bit monotonic ns in software (wraps every 2^32/24e6 ~= 179 s;
    // the scheduler reads far more often, and clocksoak validates multi-wrap).
    constexpr uintptr_t GPT1_BASE = 0x401EC000;      // RM Table 3-3 (AIPS-2)
    constexpr uintptr_t GPT1_CR = GPT1_BASE + 0x00;
    constexpr uintptr_t GPT1_PR = GPT1_BASE + 0x04;
    constexpr uintptr_t GPT1_SR = GPT1_BASE + 0x08;
    constexpr uintptr_t GPT1_IR = GPT1_BASE + 0x0C;
    constexpr uintptr_t GPT1_CNT = GPT1_BASE + 0x24; // RM 52.7.1: main counter (RO)
    constexpr uint32_t CR_EN = 1u << 0;
    constexpr uint32_t CR_ENMOD = 1u << 1;           // reset counter to 0 on enable
    constexpr uint32_t CR_DBGEN = 1u << 2;           // keep counting in debug ...
    constexpr uint32_t CR_WAITEN = 1u << 3;          // ... wait ...
    constexpr uint32_t CR_DOZEEN = 1u << 4;          // ... doze ...
    constexpr uint32_t CR_STOPEN = 1u << 5;          // ... and stop mode
    constexpr uint32_t CR_CLKSRC_24M = 5u << 6;      // CLKSRC=0b101: 24 MHz osc
    constexpr uint32_t CR_FRR = 1u << 9;             // free-run (roll at 0xFFFFFFFF)
    constexpr uint32_t CR_EN_24M = 1u << 10;         // enable the 24 MHz osc input
    constexpr uint32_t CR_SWR = 1u << 15;            // software reset (self-clears)
    constexpr uintptr_t CCM_CCGR1 = 0x400FC06C;      // GPT1: CG10 [21:20], CG11 [23:22]
    constexpr uint32_t CCGR1_GPT1 = (3u << 20) | (3u << 22); // bus + serial, on
    constexpr uint32_t GPT_HZ = 24000000u;

    uint32_t g_gpt_hi = 0;   // software high word; read/updated under the crit section
    uint32_t g_gpt_last = 0;

    uint64_t gpt_ticks()
    {
        // Called from thread and ISR context: the wrap-extend read must be atomic
        // against a concurrent reader, so run it under the crit section.
        arch_irq_state_t s = arch_irq_save();
        uint32_t cur = r32(GPT1_CNT);
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
        r32(CCM_CCGR1) |= CCGR1_GPT1;   // clock GPT1 (bus + serial)
        r32(GPT1_CR) = 0;               // CLKSRC only changes while EN=0 (RM 52.4)
        r32(GPT1_CR) = CR_SWR;          // software reset
        while ((r32(GPT1_CR) & CR_SWR) != 0)
        {
        }
        r32(GPT1_IR) = 0;               // polled clock: no compare/rollover IRQs
        r32(GPT1_SR) = 0x3Fu;           // W1C: clear any latched status
        r32(GPT1_PR) = 0;               // PRESCALER=/1, PRESCALER24M=/1 -> 24 MHz
        // Program all config with EN=0, then set EN last (RM 52.6.1).
        uint32_t const cr = CR_CLKSRC_24M | CR_EN_24M | CR_FRR | CR_ENMOD
                          | CR_DBGEN | CR_WAITEN | CR_DOZEEN | CR_STOPEN;
        r32(GPT1_CR) = cr;
        r32(GPT1_CR) = cr | CR_EN;
    }

    void uart6_init()
    {
        r32(CCM_CCGR3) |= CCGR3_LPUART6; // clock LPUART6 (reset already enables it)

        r32(IOMUXC_SW_MUX_AD_B0_02) = MUX_ALT2; // TX
        r32(IOMUXC_SW_MUX_AD_B0_03) = MUX_ALT2; // RX
        r32(IOMUXC_LPUART6_RX_SELECT_INPUT) = RX_DAISY_AD_B0_03;

        r32(LPUART6_CTRL) = 0;               // disable TX/RX while configuring
        r32(LPUART6_GLOBAL) = GLOBAL_RST;    // module software reset
        r32(LPUART6_GLOBAL) = 0;

        // baud = uart_clk / ((OSR+1) * SBR). OSR=15 (16x oversample).
        // Round SBR to nearest, NOT truncate: at the 20 MHz root the ideal divisor is
        // 10.85, and truncation (->10 = 125000 baud, +8.5%) blows past receiver
        // tolerance; nearest (->11 = 113636 baud, -1.36%) is in tolerance.
        uint32_t const baud = 115200u;
        uint32_t const osr = 15u;
        uint32_t const div = baud * (osr + 1u);
        uint32_t sbr = (UART_CLK_ROOT_HZ + (div / 2u)) / div;
        if (sbr == 0)
        {
            sbr = 1;
        }
        r32(LPUART6_BAUD) = (osr << 24) | (sbr & 0x1FFFu);
        r32(LPUART6_CTRL) = CTRL_TE | CTRL_RE; // TIE stays clear; the ring primes it
    }

#ifdef KICKOS_UART_BEACON
    // Baud-beacon diagnostic (docs/design-teensy-rt1062.md bring-up note). Programs
    // LPUART6 BAUD with a FIXED SBR (independent of UART_CLK_ROOT_HZ) and transmits
    // 0x55 ('U', alternating bits) forever. Flash once, then sweep the host reader
    // baud; the reader baud that reads clean 0x55 IS the on-wire baud, so
    //   real_uart_clk = clean_reader_baud * (OSR+1) * BEACON_SBR = clean_reader_baud * 176.
    // BEACON_SBR=11, OSR=15 (16x): at the RM-derived 20 MHz root this is 20e6/(16*11) =
    // 113636 baud, which reads clean at host 115200 (-1.36%, within receiver tolerance).
    constexpr uint32_t BEACON_SBR = 11u;
    void uart6_beacon(void)
    {
        r32(LPUART6_CTRL) = 0;
        r32(LPUART6_BAUD) = (15u << 24) | (BEACON_SBR & 0x1FFFu);
        r32(LPUART6_CTRL) = CTRL_TE;
        while (true)
        {
            while ((r32(LPUART6_STAT) & STAT_TDRE) == 0)
            {
            }
            r32(LPUART6_DATA) = 0x55u;
        }
    }
#endif

    // --- Buffered console TX backend (console_tx.h) ---
    int lp6_tx_slot_free(void) { return (r32(LPUART6_STAT) & STAT_TDRE) != 0; }
    void lp6_tx_push(uint8_t b) { r32(LPUART6_DATA) = b; }
    void lp6_tx_irq_enable(void) { r32(LPUART6_CTRL) |= CTRL_TIE; }
    void lp6_tx_irq_disable(void) { r32(LPUART6_CTRL) &= ~CTRL_TIE; }

    constexpr uint32_t CONSOLE_TX_SIZE = 512; // power of two; > kprintf's 256B buffer
    char console_tx_buf[CONSOLE_TX_SIZE];
    console_tx_backend const lp6_console_backend = {
        lp6_tx_slot_free, lp6_tx_push, lp6_tx_irq_enable, lp6_tx_irq_disable};
}

extern "C"
{

void arch_init(void)
{
#if KICKOS_HAVE_MPU
    // M7 XIP anti-speculation + L1 caches (ERR011573; docs/design-teensy-mpu-hang.md).
    // ORDER IS LOAD-BEARING: the fixed MPU regions -- which mark the unbacked external
    // Normal bands (FlexSPI beyond the 8 MiB image + the SEMC aperture) as Device, so
    // the M7 cannot speculatively prefetch into an AHB slave that never responds -- must
    // be LIVE BEFORE the cache is enabled, because a cache is what arms that speculation.
    kickos_arm_mpu_fixed_init();
    // I-cache is the config the fix was silicon-proven with. The D-cache is a PRE-M4 step,
    // opt-in via -DKICKOS_IMXRT_DCACHE=1 until silicon-validated as the default (safe today:
    // single-core, no DMA; the only coherency obligation arrives with M4-era DMA). TODO.md.
    kickos_armv7m_icache_enable();
#if defined(KICKOS_IMXRT_DCACHE) && KICKOS_IMXRT_DCACHE
    kickos_armv7m_dcache_enable();
#endif
#endif
    clock_init();
    gpt_clock_init(); // monotonic clock up before the scheduler reads it
    uart6_init();
#ifdef KICKOS_UART_BEACON
    uart6_beacon(); // never returns: raw 0x55 stream for host baud sweep
#endif
    kickos_armv7m_init();
}

#if KICKOS_HAVE_MPU
// Chip fixed (thread-invariant) MPU regions, programmed once into the LOW slots by the
// shared kickos_arm_mpu_fixed_init; per-thread grants sit above them (higher slot wins).
// ERR011573 / Arm 1013783-B: the M7 speculatively prefetches Normal memory, and the
// ARMv7-M default map leaves 0x6000_0000-0x9FFF_FFFF Normal -- so speculation past the
// populated 8 MiB of flash, or into the unbacked SEMC aperture, hits an AHB slave that
// never responds and stalls the core with NO fault. Wrap both external Normal bands
// Device + XN + no-access; overlay the real 8 MiB as Normal cacheable priv-RO+X.
// (Option A: keep PRIVDEFENA for RAM/peripherals; the whole-map explicit "Option B"
// hardening is post-M6 -- TODO.md.) The row type mirrors arch/arm/common/mpu.h.
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
        while ((r32(LPUART6_STAT) & STAT_TDRE) == 0)
        {
            if (++spin > KICKOS_POLL_SPIN_MAX)
            {
                return; // bounded: a wedged UART must not hang the panic path (drop)
            }
        }
        r32(LPUART6_DATA) = static_cast<uint8_t>(buf[i]);
    }
}

console_tx_backend const* arch_console_tx_backend(char** storage, uint32_t* size, int* irq_line)
{
    *storage = console_tx_buf;
    *size = CONSOLE_TX_SIZE;
    *irq_line = LPUART6_IRQ;
    return &lp6_console_backend;
}

// Monotonic clock (arch.h contract; the armv7m layer provides no fallback). GPT1
// 24 MHz ticks -> ns. Fixed 24 MHz, so the reciprocal-multiply constant is compile-
// time (no per-read divide, no re-anchor across an ARM-PLL retune).
uint64_t arch_clock_now(void)
{
    uint64_t ticks = gpt_ticks();
    // ns = ticks * 1e9 / 24e6, the divide folds at build time (GPT_HZ is constant).
    constexpr uint64_t MULT = kickos::arch_clk_recip_q32(GPT_HZ);
    return kickos::arch_clk_mul_q32(ticks, MULT);
}

// Override the weak WFI idle. The tickless wakeup timer is SysTick, clocked off the
// core clock -- which the RT106x halts under WFI, so SysTick stops counting and a
// sleep with every thread idle never wakes (the GPT monotonic clock keeps running,
// but it is not the wakeup source). Spin so the core clock, and thus SysTick, stays
// alive. A GPT output-compare wakeup (GPT counts through WAIT via CR_WAITEN, so WFI
// would be safe) is the power-optimal follow-up; a busy idle is correct for now.
void arch_idle_wait(void)
{
    __asm volatile("nop");
}

void arch_shutdown(int status)
{
    (void)status; // no exit on bare metal
    __asm volatile("cpsid i" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
}

#if KICKOS_HAVE_MPU
// Rule 7 reserved set (RT1060 RM). Owns-for-life: the GPT1 monotonic time base and
// the CCM (CCGR clock-gate roots). Bases are the constants above; sizes one 4 KB AIPS
// slot each. M7 has NO bit-band, so arch_bitband_present keeps the weak 0 default.
size_t arch_reserved_blocks(struct arch_reserved_block* out, size_t max)
{
    static struct arch_reserved_block const blocks[] = {
        {0x401EC000u, 0x1000u}, // GPT1: monotonic time base (RM ch.52, Table 3-3)
        {0x400FC000u, 0x1000u}, // CCM: CCGR clock-gate roots (RM ch.14)
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
    enable_fpu(); // before ANY later code that could emit FP (softfp ABI)
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
