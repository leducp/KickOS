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
#include "regs/clocks.h"
#include "regs/resets.h"
#include "regs/sio.h"
#include "regs/timer.h"
#include "../rp2xxx/rp2xxx.h"
#include "console_claim.h"
#include "regs.h"

namespace reg = kickos::rp2350::reg;
namespace irq = kickos::rp2350::irq;

using kickos::rp2xxx::ATOMIC_CLR;
using kickos::rp2xxx::unreset;
using kickos::rp2xxx::wait_mask;

namespace kickos
{
    int kmain(int argc, char** argv);
}

extern "C"
{
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

#if KICKOS_AMP_OWN_IMAGE
    // Published by the primary for peers, which run no clocks_init. RELAXED: the dsb in
    // arch_amp_release_peers orders it ahead of any peer's boot.
    KICKOS_AMP_SHARED("chip")
    kickos::Atomic<uint32_t, kickos::Order::RELAXED> g_amp_clk_sys_hz = 0;
#endif
}

#if KICKOS_AMP_OWN_IMAGE
namespace
{
    // Read by a peer whose own claim failed, so they live where both nodes look. Zeroed by the
    // primary before any peer runs. SPINLOCK31 serialises the owner's and deadline's writers and
    // the fences carry the ordering, so the fields carry none.
    KICKOS_AMP_SHARED("chip") kickos::rp2350::console::Word g_console_owner = 0;
    KICKOS_AMP_SHARED("chip") kickos::rp2350::console::Word g_console_deadline = 0;
    KICKOS_AMP_SHARED("chip")
    kickos::rp2350::console::Word g_console_waiting[KICKOS_AMP_NODES] = {};

    kickos::rp2350::console::Claim g_claim{KICKOS_AMP_NODE_ID + 1u, g_console_owner,
                                           g_console_deadline, g_console_waiting,
                                           KICKOS_AMP_NODES};
}

namespace kickos::rp2350::console
{
    uint32_t now(void)
    {
        return r32(reg::timer::TIMERAWL);
    }

    bool lock_take(void)
    {
        return r32(reg::sio::SPINLOCK31) != 0;
    }

    void lock_free(void)
    {
        r32(reg::sio::SPINLOCK31) = 1u;
    }

    void fence(void)
    {
        __asm volatile("dsb" ::: "memory");
    }

    void ring(uint32_t node)
    {
        arch_ipi_send(1u << kickos_amp_node_core(node));
    }
}

namespace kickos::rp2xxx
{
    bool console_claim(void)
    {
        return g_claim.claim();
    }

    // A kernel line keeps interrupts open between attempts: a peer may hold the UART for a
    // whole hold. A user writer makes one attempt and parks in the kernel instead.
    bool console_claim_open(bool spin)
    {
        using kickos::rp2350::console::HOLD_MAX_US;
        uint32_t const waited_from = r32(reg::timer::TIMERAWL);
        for (uint32_t i = 0; i < KICKOS_POLL_SPIN_MAX; i++)
        {
            arch_irq_state_t const irq = arch_irq_save();
            bool const held = g_claim.try_open();
            arch_irq_restore(irq);
            if (held)
            {
                return true;
            }
            if (not spin or (r32(reg::timer::TIMERAWL) - waited_from) >= HOLD_MAX_US)
            {
                return false;
            }
        }
        return false;
    }

    void console_drop(bool ended_line)
    {
        g_claim.drop(ended_line);
    }
}

extern "C" uint32_t arch_console_claim_wait(void)
{
    return g_claim.wait();
}
#endif

extern "C"
{

void arch_init(void)
{
    // Reset-release ordering is load-bearing: a RESET_DONE only asserts once the peripheral has a
    // running clock. IO_BANK0/PADS_BANK0/TIMER0 run off clk_sys/clk_ref, live off the ROSC at
    // reset; UART1 runs off clk_peri, OFF until clocks_init, so releasing it BEFORE that hangs the
    // boot.
    // The clock tree, the resets and the console UART are partition-wide, so only the PRIMARY
    // brings them up: a peer re-entering clocks_init stops clk_sys under a primary executing off
    // those PLLs, an unrecoverable lock-up, and resets UART1 mid-transmission.
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
        unreset(reg::resets::IO_BANK0 | reg::resets::PADS_BANK0 | reg::resets::TIMER0);
        kickos::rp2xxx::clocks_init();
        unreset(reg::resets::UART1);
        kickos::rp2xxx::console_uart_init();
        if (kickos_usb_device_console != 0)
        {
            kickos::rp2xxx::usb_clock_init();
        }
#if KICKOS_AMP_OWN_IMAGE
        g_amp_clk_sys_hz = SystemCoreClock;
        // Before any peer exists, so the release cannot land on a claim in progress.
        r32(reg::sio::SPINLOCK31) = 1u;
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
#if KICKOS_AMP_OWN_IMAGE
    // No buffered path on a partition: "TX IRQ enabled whenever the ring is non-empty" holds of
    // ONE ring, and IMSC.TXIM is one bit over two rings. UART1_IRQ reaches both cores' controllers
    // (3.2), so whichever node's drain empties first clears the bit the other's bytes wait on.
    (void)irq_line;
    return nullptr;
#else
    *irq_line = irq::UART1_IRQ;
    return &kickos::rp2xxx::console_backend;
#endif
}

// Must stay in this TU: arch_init anchors it here, and anywhere else the arch/common fallback
// resolves the call first and the link still succeeds.
//
// UART0's RESET_DONE asserts only while clk_peri runs (7.5).
int arch_periph_enable(uintptr_t base)
{
    if (base != kickos::rp2350::mmap::UART0_BASE)
    {
        return -KOS_EINVAL;
    }
    r32(reg::resets::RESET + ATOMIC_CLR) = reg::resets::UART0;
    if (not wait_mask(reg::resets::RESET_DONE, reg::resets::UART0))
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
