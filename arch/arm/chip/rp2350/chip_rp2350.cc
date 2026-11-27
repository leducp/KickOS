// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Raspberry Pi RP2350 (Cortex-M33) chip backend. Section numbers cite the RP2350 datasheet
// (RP-008373-DS-2).
//
// clk_sys runs at 150 MHz off PLL_SYS (12 MHz XOSC x125 /5 /2, 8.6).

#include <kickos/arch/amp_shared.h>
#include <kickos/arch/arch.h>
#include <arch_reboot_tail.h>
#include <kickos/config/limits.h>
#include <kickos/diag.h>
#include <kickos/console_tx.h>
#include <kickos/sys/abi.h>
#include <kickos/sys/atomic.h>
#include <kickos/usb_console.h>

#include <stdint.h>

#include "board_pins.h"
#include "irq.h"
#include "accessctrl_rows.h"
#include "regs/accessctrl.h"
#include "regs/clocks.h"
#include "regs/resets.h"
#include "regs/sio.h"
#include "regs/timer.h"
#include "../rp2xxx/rp2xxx.h"
#include "regs.h"

namespace reg = kickos::rp2350::reg;
namespace irq = kickos::rp2350::irq;

#if defined(KICKOS_AMP_CONSOLE_UART)
#if KICKOS_AMP_CONSOLE_UART == 0
static_assert(KICKOS_BOARD_CONSOLE_BASE == kickos::rp2350::mmap::UART0_BASE,
              "KICKOS_AMP_CONSOLES gives this node /dev/uart0, which is not the board's console");
#elif KICKOS_AMP_CONSOLE_UART == 1
static_assert(KICKOS_BOARD_CONSOLE_BASE == kickos::rp2350::mmap::UART1_BASE,
              "KICKOS_AMP_CONSOLES gives this node /dev/uart1, which is not the board's console");
#else
#error "KICKOS_AMP_CONSOLES names a UART the RP2350 does not have: it has /dev/uart0 and /dev/uart1"
#endif
#endif

using kickos::rp2xxx::ATOMIC_CLR;
using kickos::rp2xxx::unreset;
using kickos::rp2xxx::wait_mask;

namespace
{
    namespace mmap = kickos::rp2350::mmap;

    constexpr uint32_t uart_reset(uintptr_t base)
    {
        if (base == mmap::UART0_BASE)
        {
            return reg::resets::UART0;
        }
        return reg::resets::UART1;
    }

    static_assert(KICKOS_BOARD_CONSOLE_BASE == mmap::UART0_BASE
                      or KICKOS_BOARD_CONSOLE_BASE == mmap::UART1_BASE,
                  "the board's console is not a UART of this chip");
    constexpr uint32_t CONSOLE_RESET = uart_reset(KICKOS_BOARD_CONSOLE_BASE);
}

namespace kickos
{
    int kmain(int argc, char** argv);
    void kpanic(char const* msg) __attribute__((noreturn));
}

extern "C"
{
void kickos_armv7m_ccr_init(void);
void kickos_armv7m_init(void);
#if KICKOS_HAVE_MPU
// LINK ANCHOR: this reference pulls arch_arm_pmsav8.cc, so its kickos_arch_mpu_commit /
// arch_mpu_region_encodable replace the v7-M fallback TUs.
void kickos_arm_pmsav8_init(void);
#endif

extern void (*__init_array_start[])();
extern void (*__init_array_end[])();
extern uint32_t g_isr_vector[];

// clk_sys as the bootrom leaves it; SysTick reads it live.
uint32_t SystemCoreClock = reg::clocks::ROSC_NOMINAL_HZ;
}

namespace
{
    using rom_lookup_fn = void* (*)(uint32_t, uint32_t);

    // False unless the bootrom header's magic matches; then the Arm lookup helper
    // (rom_table_lookup_val) is the halfword at 0x16, already carrying the Thumb bit. 0x18 is a
    // different entry, and the byte at 0x13 is a ROM build number the datasheet forbids using to
    // locate functions. GCC assumes the first bytes of the address space are unmapped; 0x0 is the
    // bootrom.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    bool rom_lookup(rom_lookup_fn* out)
    {
        if (r8(0x10u) != 'M' or r8(0x11u) != 'u' or r8(0x12u) != 0x02u)
        {
            return false;
        }
        *out = reinterpret_cast<rom_lookup_fn>(static_cast<uintptr_t>(r16(0x16u)));
        return true;
    }
#pragma GCC diagnostic pop

    // The bus ports' reset images admit Non-secure masters, a core or a DMA channel (10.6.2).
    void accessctrl_close_nonsecure(void)
    {
        namespace gate = reg::accessctrl;
        namespace accessctrl = kickos::rp2350::accessctrl;
        for (uint32_t off = gate::FIRST_BUS_REG; off <= gate::LAST_BUS_REG; off += 4u)
        {
            r32(gate::BASE + off) = accessctrl::closed_to_nonsecure(r32(gate::BASE + off));
            if (accessctrl::admits_nonsecure(r32(gate::BASE + off)))
            {
                kickos::kpanic(kickos::diag::kAccessctrlNonSecure);
            }
        }
    }

#if KICKOS_AMP_OWN_IMAGE
    // Published by the primary for peers, which run no clocks_init. RELAXED: the dsb in
    // arch_amp_release_peers orders it ahead of any peer's boot.
    KICKOS_AMP_SHARED("chip")
    kickos::Atomic<uint32_t, kickos::Order::RELAXED> g_amp_clk_sys_hz = 0;
#endif
}

extern "C"
{

void arch_init(void)
{
    // Reset-release ordering is load-bearing: a RESET_DONE only asserts once the peripheral has a
    // running clock. IO_BANK0/PADS_BANK0/TIMER0 run off clk_sys/clk_ref, live off the ROSC at
    // reset; a UART runs off clk_peri, OFF until clocks_init, so releasing it BEFORE that hangs
    // the boot.
    // The clock tree is partition-wide, so only the PRIMARY brings it up: a peer re-entering
    // clocks_init stops clk_sys under a primary executing off those PLLs, an unrecoverable lock-up.
    //
    // A peer still owes its own SystemCoreClock, installed from the primary's resolved answer and
    // never re-derived.
#if KICKOS_AMP_OWN_IMAGE
    if (KICKOS_AMP_NODE_ID != 0)
    {
        uint32_t const hz = g_amp_clk_sys_hz;
        if (hz != 0)
        {
            SystemCoreClock = hz;
        }
    }
    else
#endif
    {
        accessctrl_close_nonsecure();
        unreset(reg::resets::IO_BANK0 | reg::resets::PADS_BANK0 | reg::resets::TIMER0);
        kickos::rp2xxx::clocks_init();
    }
    // The console UART is brought up by the node whose console it is, its clk_peri by the
    // primary alone.
#if KICKOS_RP2XXX_CONSOLE
    unreset(CONSOLE_RESET);
    kickos::rp2xxx::console_uart_init();
#endif
#if KICKOS_AMP_OWN_IMAGE
    if (KICKOS_AMP_NODE_ID == 0)
#endif
    {
        if (kickos_usb_device_console != 0)
        {
            kickos::rp2xxx::usb_clock_init();
        }
#if KICKOS_AMP_OWN_IMAGE
        g_amp_clk_sys_hz = SystemCoreClock;
#endif
    }
#if KICKOS_HAVE_MPU
    kickos_arm_pmsav8_init();
#endif
    kickos_armv7m_init();
#if (KICKOS_NUM_CORES > 1 || KICKOS_AMP_NODE)
    // SIO_IRQ_BELL is CORE-LOCAL (3.1.6), so every node opens its own. Through arch_irq_unmask
    // for the priority: the NVIC IPR resets to 0, and a doorbell above the BASEPRI band would run
    // its service inside a section holding IrqLock.
    arch_irq_unmask(irq::SIO_IRQ_BELL);
#endif
}

console_tx_backend const* arch_console_tx_backend(int* irq_line)
{
#if KICKOS_RP2XXX_CONSOLE
    *irq_line = KICKOS_BOARD_CONSOLE_LINE_IRQ;
    return &kickos::rp2xxx::console_backend;
#else
    (void)irq_line;
    return nullptr;
#endif
}

// Must stay in this TU: arch_init anchors it here, and anywhere else the arch/common fallback
// resolves the call first and the link still succeeds.
//
// The UART that is not the console. Its RESET_DONE asserts only while clk_peri runs (7.5).
int arch_periph_enable(uintptr_t base)
{
    if (base != mmap::UART0_BASE and base != mmap::UART1_BASE)
    {
        return -KOS_EINVAL;
    }
    if (base == KICKOS_BOARD_CONSOLE_BASE)
    {
        return -KOS_EINVAL;
    }
    uint32_t const bit = uart_reset(base);
    r32(reg::resets::RESET + ATOMIC_CLR) = bit;
    if (not wait_mask(reg::resets::RESET_DONE, bit))
    {
        return -KOS_EIO;
    }
    return 0;
}

// Both outcomes are the bootrom `reboot` entry (5.4.8.24).
int arch_reboot(uint32_t outcome)
{
    constexpr uint32_t REBOOT2_FLAG_REBOOT_TYPE_NORMAL = 0x0000u;
    constexpr uint32_t REBOOT2_FLAG_REBOOT_TYPE_BOOTSEL = 0x0002u;
    uint32_t type = REBOOT2_FLAG_REBOOT_TYPE_NORMAL;
    if (outcome == KOS_SHUTDOWN_BOOTLOADER)
    {
        type = REBOOT2_FLAG_REBOOT_TYPE_BOOTSEL;
    }
    else if (outcome != KOS_SHUTDOWN_REBOOT)
    {
        return -KOS_ENOSYS;
    }
    rom_lookup_fn lookup;
    if (not rom_lookup(&lookup))
    {
        return -KOS_ENOSYS;
    }

    // 'RB' packs as (c2 << 8) | c1; RT_FLAG_FUNC_ARM_SEC picks the Secure Arm variant.
    constexpr uint32_t RT_FLAG_FUNC_ARM_SEC = 0x0004u;
    kickos_rp_rom_args const find = {reinterpret_cast<uintptr_t>(lookup), ('B' << 8) | 'R',
                                     RT_FLAG_FUNC_ARM_SEC, 0u, 0u};
    uintptr_t const rom_reboot = kickos_rp_rom_call(&find);
    if (rom_reboot == 0u)
    {
        return -KOS_ENOSYS;
    }

    constexpr uint32_t REBOOT2_FLAG_NO_RETURN_ON_SUCCESS = 0x0100u;
    __asm volatile("cpsid i" ::: "memory"); // dispatch runs in thread mode with IRQs live
    // delay_ms must never be 0: the bootrom mishandles a zero timeout.
    kickos_rp_rom_args const reboot = {rom_reboot, type | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, 10u,
                                       0u, 0u};
    (void)kickos_rp_rom_call(&reboot);
    kickos::arch::reboot_failed(kickos::diag::kRebootRp2350);
}

#if KICKOS_AMP_OWN_IMAGE
// Restores the reset QMI address translation, an identity map over the 16 MiB (12.14.4). Node 0
// reads its peer's vector table out of flash to launch it, and that address is the peer's only
// under an identity map. The bootrom reprograms ATRANS when the image sits in a flash partition
// (12.14.4.1) or node 0's IMAGE_DEF carries a ROLLING_WINDOW_DELTA item (5.9.3.5).
//
// BOTH CALLS MUST RUN BEFORE CORE 1 IS LAUNCHED. The flush "unpins pinned cache lines" (5.4.8.8)
// across the ONE cache both nodes share, so once a peer runs it would take that peer's
// cache-as-SRAM. Nothing is pinned yet: the bootrom invalidates every line on entering flash
// boot (4.4.1.2).
void kickos_rp2350_xip_identity(void)
{
    if (KICKOS_AMP_NODE_ID != 0)
    {
        return;
    }
    rom_lookup_fn lookup;
    if (not rom_lookup(&lookup))
    {
        return;
    }
    constexpr uint32_t RT_FLAG_FUNC_ARM_SEC = 0x0004u;
    using void_fn = void (*)(void);

    // rom_table_code(c1, c2) is (c2 << 8) | c1 (5.4.1). The datasheet does not say WHICH LISTED
    // CHARACTER IS c1, so the byte order matches arch_reboot's 'R','B' spelling.
    void_fn const reset_trans =
        reinterpret_cast<void_fn>(lookup(('A' << 8) | 'R', RT_FLAG_FUNC_ARM_SEC));
    void_fn const flush_cache =
        reinterpret_cast<void_fn>(lookup(('C' << 8) | 'F', RT_FLAG_FUNC_ARM_SEC));
    if (reset_trans != nullptr)
    {
        reset_trans();
    }
    // Required after a translation change, harmless when nothing changed.
    if (flush_cache != nullptr)
    {
        flush_cache();
    }
}
#endif

void Reset_Handler(void)
{
    kickos_armv7m_ccr_init();
    // A warm reboot or debugger entry may not have run the bootrom's VTOR setup (5.2.2). From the
    // SYMBOL, never a literal: a peer links at its own flash slice.
    r32(kickos::arm::SCB_VTOR) = reinterpret_cast<uintptr_t>(g_isr_vector);

    kickos_armv7m_enable_fpu(); // before any code a hard-float ABI might emit FP into

    kickos_ranges_init();
#if KICKOS_AMP_OWN_IMAGE
    kickos_rp2350_xip_identity();
    // Ahead of arch_init, which publishes into that region; later would erase the primary's own
    // publication. The region is outside .bss, so kickos_ranges_init does not reach it.
    arch_amp_shared_zero();
#endif
    for (void (**fn)() = __init_array_start; fn != __init_array_end; fn++)
    {
        (*fn)();
    }
    arch_init();
    kickos::kmain(0, nullptr);
    arch_shutdown(0);
}
}
