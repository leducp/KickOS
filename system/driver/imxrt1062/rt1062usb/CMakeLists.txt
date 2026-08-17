# SPDX-License-Identifier: CECILL-C
# Copyright (c) 2026 Philippe Leduc

# The i.MX RT1062 USB CDC-ACM console driver. REGDIR is the chip directory, which is what
# makes the body resolve irq.h.
kickos_add_driver(rt1062usb SOURCES rt1062usb.cc REGDIR arch/arm/chip/imxrt1062)

# Off unless a bench run asks for it: makes the IRQ thread read a kernel-only address, so a
# capture says whether an enforcing MPU refuses this thread at all.
if(KICKOS_RTUSB_UNGRANTED_PROBE)
    target_compile_definitions(kickos_rt1062usb PRIVATE KICKOS_RTUSB_UNGRANTED_PROBE=1)
endif()
