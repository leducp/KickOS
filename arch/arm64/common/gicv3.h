// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// What a part declares to select the GICv3 backend (arch_arm64_gicv3.cc); the interface it
// implements is gic.h.

#ifndef KICKOS_ARCH_ARM64_COMMON_GICV3_H
#define KICKOS_ARCH_ARM64_COMMON_GICV3_H

#include <stdint.h>

extern "C"
{
    // Where a part puts its GICv3, and the routing that is the part's. PHYSICAL addresses:
    // every device register is reached through the kernel's own half.
    //
    // The CPU interface is the ICC_* system registers; each core owns one redistributor, and
    // the stride counts the frames the part ships: two 64 KB frames under GICv3, four where
    // GICv4 adds the virtual LPI pair (IHI 0069H.b section 12.10).
    struct kickos_gicv3_map
    {
        uintptr_t dist_pa;      // GICD, one distributor for the machine
        uintptr_t rdist_pa;     // GICR, the first core's RD_base of a contiguous series
        uintptr_t rdist_stride; // bytes from one core's RD_base to the next
        // Redistributors the die carries, not KICKOS_NUM_CORES: each is decoded whether or not
        // this image released a core into it, so an image driving one core of four still
        // reserves the other three and searches them on an EL1 handover.
        int rdist_count;
        int intid_count; // INTIDs the distributor implements, the banked IDs included
        int timer_intid; // the PPI the EL1 physical timer asserts
    };

    // The chip's controller, read from bring-up onward and never written.
    extern struct kickos_gicv3_map const kickos_gicv3;
}

#endif
