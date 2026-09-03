// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What a part declares to select the GICv3 backend (arch_arm64_gicv3.cc). The interface that
// backend implements is gic.h, and everything the backend holds beyond this struct is
// architected by ARM.

#ifndef KICKOS_ARCH_ARM64_COMMON_GICV3_H
#define KICKOS_ARCH_ARM64_COMMON_GICV3_H

#include <stdint.h>

extern "C"
{
    // Where a part puts its GICv3, and the routing that is the part's. PHYSICAL addresses:
    // every device register is reached through the kernel's own half.
    //
    // There is no CPU-interface window: a GICv3 CPU interface is the ICC_* system registers.
    // What replaces it is one REDISTRIBUTOR per core, and the stride between them is the
    // part's because it counts the frames the implementation ships: two 64 KB frames under
    // GICv3, four where GICv4 adds the virtual LPI pair (IHI 0069H.b section 12.10).
    struct kickos_gicv3_map
    {
        uintptr_t dist_pa;      // GICD, one distributor for the machine
        uintptr_t rdist_pa;     // GICR, the first core's RD_base of a contiguous series
        uintptr_t rdist_stride; // bytes from one core's RD_base to the next
        // FRAMES THE DIE CARRIES, WHICH IS NOT KICKOS_NUM_CORES. A redistributor exists per
        // core the part implements, decoded and reachable whether or not this image released a
        // core into it, so an image driving one core of four still owes the other three a
        // reservation and still has to search them on an EL1 handover.
        int rdist_count;
        int intid_count; // INTIDs the distributor implements, the banked IDs included
        int timer_intid; // the PPI the EL1 physical timer asserts
    };

    // The chip's controller, read from bring-up onward and never written.
    extern struct kickos_gicv3_map const kickos_gicv3;
}

#endif
