// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// GICv2 for the arm64 family. The register displacements, the boundary below which a
// distributor register is the calling core's own bank, the acknowledge and end-of-interrupt
// protocol and the identifier meaning no interrupt is pending are architected by ARM and live
// in arch_arm64_gicv2.cc, which is where arch.h's mask/unmask/clear triad, arch_irq_inject and
// kickos_armv8a_gic_dispatch are defined. One interrupt-controller backend is linked per
// image.
//
// A chip declares the controller it has by defining kickos_gicv2.

#ifndef KICKOS_ARCH_ARM64_COMMON_GICV2_H
#define KICKOS_ARCH_ARM64_COMMON_GICV2_H

#include <stdint.h>

// KICKOS_NUM_CORES.
#include <kickos/arch/arch.h>

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

    // The distributor's shared half: one write set for the machine, ahead of any CPU
    // interface. The primary runs it.
    void kickos_gicv2_dist_init(void);

    // The calling core's CPU interface, its own bank of distributor registers and its timer
    // PPI. Every core runs it.
    void kickos_gicv2_percore_init(void);

    // Drops the pending state of one INTID. The INTID is the caller's to bound.
    void kickos_gicv2_clear_pending(int intid);

#if KICKOS_NUM_CORES > 1
    // Raises the doorbell on every core in `cores`, a bitmask of core INDICES. Orders the
    // caller's earlier writes ahead of the raise.
    void kickos_gicv2_doorbell_send(uint32_t cores);

    // Drops the doorbell's pending state on the CALLING core, for every source core. A core
    // servicing the doorbell outside its handler owes this call.
    void kickos_gicv2_doorbell_clear(void);

    // Masks every line on the calling core's own bank except the doorbell.
    void kickos_gicv2_doorbell_only(void);

    // What this backend calls on the calling core when the doorbell arrives. Must not take the
    // kernel lock: an initiator holding it waits on this.
    void kickos_arm64_doorbell_service(void);
#endif
}

#endif
