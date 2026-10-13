// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The two parts are on DIFFERENT ISAs (armv6m and armv7m) and share no arch archive, so
// chip_rp2xxx.cc is compiled into each chip's archive (its chip file's `family`) against its
// regs/ headers.
//
// A chip joins the family through its family_map.h, which aliases its register namespace into
// kickos::rp2xxx and defines the part:: hooks the shared bring-up calls, and by spelling the
// register names this unit uses.

#ifndef KICKOS_ARCH_ARM_CHIP_RP2XXX_RP2XXX_H
#define KICKOS_ARCH_ARM_CHIP_RP2XXX_RP2XXX_H

#include <stdint.h>
#include <kickos/console_tx.h>
#include <kickos/io/mmio.h>

// A bootrom routine and its four arguments; fn carries the Thumb bit the ROM table gives it.
struct kickos_rp_rom_args
{
    uintptr_t fn;
    uint32_t a0;
    uint32_t a1;
    uint32_t a2;
    uint32_t a3;
};

// rom_call.S: fn(a0..a3) on a stack reserved for it, interrupts masked, returning fn's r0.
extern "C" uintptr_t kickos_rp_rom_call(kickos_rp_rom_args const* args);

namespace kickos::rp2xxx
{
    // Every APB register is mirrored at these offsets from its own address; SIO is not.
    constexpr uintptr_t ATOMIC_SET = 0x2000u;
    constexpr uintptr_t ATOMIC_CLR = 0x3000u;

    // True once every bit of `mask` reads set, false once the poll's bound runs out.
    bool wait_mask(uintptr_t addr, uint32_t mask);
    void unreset(uint32_t mask);
    bool pll_sys_lock();

    // The crystal, clk_ref and clk_sys on PLL_SYS, falling back to the ROSC without the crystal and
    // to the crystal without the PLL; SystemCoreClock follows every switch, and the system
    // timer's tick starts off clk_ref. The chip releases IO_BANK0, PADS_BANK0 and its timer first.
    void clocks_init();
    // PLL_USB and clk_usb, then USBCTRL out of reset. After clocks_init.
    void usb_clock_init();
    // The console UART's pins and 8N1 frame. After clocks_init and the UART's reset release.
    void console_uart_init();
    // IO_BANK0 funcsel and the pad side of `func`, packed as bringup_values.h states.
    void pin_function(uint32_t pin, uint32_t func);

#if KICKOS_AMP_OWN_IMAGE
    // Defined by the chip (chip_rp2350.cc): serialises the polled console writer against the
    // peer kernel sharing the UART.
    bool console_claim(void);
    bool console_claim_open(bool spin);
    void console_drop(bool ended_line);
#else
    // The console UART's TX edge, behind the chip's own interrupt line.
    extern console_tx_backend const console_backend;
#endif
}

#endif
