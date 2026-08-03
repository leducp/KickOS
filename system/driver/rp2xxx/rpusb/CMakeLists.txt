# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# kickos_rpusb: the RP2040/RP2350 USB CDC-ACM console driver (a privileged one-shot
# bring-up plus two unprivileged threads on the <kickos/sys/usb_cdc_service.h>
# substrate). REGDIR is THIS build's chip directory, which is what makes the shared
# body resolve irq.h and mmap.h for the right part. No class leaf: the USB register
# map is the driver's alone -- the kernel side owns only RESETS, CLOCKS and PLL_USB.
kickos_add_driver(rpusb REGDIR arch/arm/chip/${KICKOS_CHIP})

# The per-chip delta selector rp_usb_chip.h reads. Uppercased chip id rather than a
# hand-maintained if/else, so a third RP part needs no edit here.
string(TOUPPER "${KICKOS_CHIP}" _rpusb_chip)
target_compile_definitions(kickos_rpusb PRIVATE KICKOS_RPUSB_CHIP_${_rpusb_chip}=1)
