// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What a part declares to select the GICv2 backend (arch_arm64_gicv2.cc). The interface that
// backend implements is gic.h, and everything the backend holds beyond this struct is
// architected by ARM.

#ifndef KICKOS_ARCH_ARM64_COMMON_GICV2_H
#define KICKOS_ARCH_ARM64_COMMON_GICV2_H

#include <stdint.h>

extern "C"
{
    // Where a part puts its GICv2, and the routing that is the part's. PHYSICAL addresses:
    // every device register is reached through the kernel's own half.
    struct kickos_gicv2_map
    {
        uintptr_t dist_pa;  // GICD, one distributor for the machine
        uintptr_t cpu_pa;   // GICC, the calling core's CPU interface
        int intid_count;    // INTIDs the distributor implements, the banked IDs included
        int timer_intid;    // the PPI the EL1 physical timer asserts
    };

    // The chip's controller, read from bring-up onward and never written.
    extern struct kickos_gicv2_map const kickos_gicv2;
}

#endif
