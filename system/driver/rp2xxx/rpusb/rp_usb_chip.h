// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The per-chip delta list for the shared RP USB backend. Every other RP2040 / RP2350
// difference the driver can see is absorbed by writing absolute values (rp_usb_regs.h).
// KICKOS_RPUSB_CHIP_* is set by the driver's CMakeLists from KICKOS_CHIP; the include path
// is the chip's own arch directory, chosen by REGDIR.

#ifndef KICKOS_DRIVER_RP2XXX_RP_USB_CHIP_H
#define KICKOS_DRIVER_RP2XXX_RP_USB_CHIP_H

#if defined(KICKOS_RPUSB_CHIP_RP2350)

#include "irq.h"
#include <kickos/chip_mmap.h>
namespace rpchip = kickos::rp2350;

#elif defined(KICKOS_RPUSB_CHIP_RP2040)

#include "irq.h"
#include <kickos/chip_mmap.h>
namespace rpchip = kickos::rp2040;

#else
#error "rpusb: no KICKOS_RPUSB_CHIP_* selected; the driver CMakeLists sets it from KICKOS_CHIP"
#endif

#endif
