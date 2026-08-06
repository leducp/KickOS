// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// ESP32-C6 USB Serial/JTAG registers (TRM v1.2 ch.32). The native USB console; the
// ROM leaves it enumerated. NOT used as the console: it is gated on the host draining
// CDC and it re-enumerates on reset. UART0 is the console.

#ifndef KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_USB_SERIAL_JTAG_H
#define KICKOS_ARCH_RISCV_CHIP_ESP32C6_REGS_USB_SERIAL_JTAG_H

#include <kickos/chip_mmap.h>

#include <stdint.h>

namespace kickos::esp32c6::reg::usb_serial_jtag
{
    constexpr uintptr_t EP1 = mmap::USB_SERIAL_JTAG_BASE + 0x00u;      // RDWR_BYTE [7:0]
    constexpr uintptr_t EP1_CONF = mmap::USB_SERIAL_JTAG_BASE + 0x04u;

    constexpr uint32_t WR_DONE = 1u << 0;          // EP1_CONF WT bit0: flush to host
    constexpr uint32_t IN_EP_DATA_FREE = 1u << 1;  // EP1_CONF RO bit1: TX FIFO has room
}

#endif
