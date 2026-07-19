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
    void _boot_entry(void); // startup.S: sets MSP, jumps to Reset_Handler

    extern void (*__init_array_start[])();
    extern void (*__init_array_end[])();

    extern uint32_t g_isr_vector[];       // startup.S: vector table @ 0x6000_2000
    extern char __boot_image_length[];    // linker: on-flash image extent

    // Core clock in Hz (CMSIS convention), owned by the chip. PLACEHOLDER: the
    // CCM/PLL bring-up is deferred, so this does NOT reflect the true post-ROM core
    // frequency (the boot ROM leaves the part at ~396-600 MHz). 24 MHz is a stand-in;
    // being ~16x too low means SysTick periods computed from it are ~16x SHORT, so
    // timed sleeps expire early. Set this from the real core-clock root once the CCM
    // bring-up lands.
    uint32_t SystemCoreClock = 24000000u;
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

    // Reset UART clock root (CSCDR1 defaults, RM 14): pll3_80m / 1 = 80 MHz.
    // ASSUMED for the first bring-up -- validate against the real UART root once
    // the CCM bring-up lands (baud tracks this).
    constexpr uint32_t UART_CLK_ROOT_HZ = 80000000u;

    // NVIC: LPUART6 combined TX/RX = IRQ 25 (RM Table 4-2).
    constexpr int LPUART6_IRQ = 25;

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
        uint32_t const baud = 115200u;
        uint32_t const osr = 15u;
        uint32_t sbr = UART_CLK_ROOT_HZ / (baud * (osr + 1u));
        if (sbr == 0)
        {
            sbr = 1;
        }
        r32(LPUART6_BAUD) = (osr << 24) | (sbr & 0x1FFFu);
        r32(LPUART6_CTRL) = CTRL_TE | CTRL_RE; // TIE stays clear; the ring primes it
    }

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
    clock_init();
    uart6_init();
    kickos_armv7m_init();
}

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
            if (++spin > 1000000u)
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

void arch_shutdown(int status)
{
    (void)status; // no exit on bare metal
    __asm volatile("cpsid i" ::: "memory");
    while (true)
    {
        __asm volatile("wfi");
    }
}

void Reset_Handler(void)
{
    // MSP was set by _boot_entry (the ROM enters via IVT.entry, not the reset
    // vector). Point VTOR at our table (@ 0x6000_2000, not flash base) before any
    // interrupt path runs.
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
