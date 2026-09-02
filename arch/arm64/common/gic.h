// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The arm64 family's interrupt-controller interface, with one backend linked per image. The
// register displacements, the boundary below which interrupt state is the calling core's own
// bank, the acknowledge and end-of-interrupt protocol and the identifier meaning no interrupt
// is pending are architected by ARM and live in the backend, which is also where arch.h's
// mask/unmask/clear triad, arch_irq_inject and kickos_armv8a_gic_dispatch are defined.
//
// A chip declares WHICH controller it has, and where, by defining that backend's map: GICv2's
// is gicv2.h, GICv3's gicv3.h. Nothing here names a register, so a core-index mask is the only
// currency that crosses this boundary.

#ifndef KICKOS_ARCH_ARM64_COMMON_GIC_H
#define KICKOS_ARCH_ARM64_COMMON_GIC_H

#include <stdint.h>

// KICKOS_NUM_CORES.
#include <kickos/arch/arch.h>

extern "C"
{
    // The distributor's shared half: one write set for the machine, ahead of any core's own
    // interface. The primary runs it.
    void kickos_armv8a_gic_dist_init(void);

    // The calling core's own interface, its own bank of interrupt state and its timer PPI.
    // Every core runs it.
    void kickos_armv8a_gic_percore_init(void);

    // Drops the pending state of one INTID. The INTID is the caller's to bound.
    void kickos_armv8a_gic_clear_pending(int intid);

#if KICKOS_NUM_CORES > 1
    // Raises the doorbell on every core in `cores`, a bitmask of core INDICES. Orders the
    // caller's earlier writes ahead of the raise.
    void kickos_armv8a_gic_doorbell_send(uint32_t cores);

    // Drops the doorbell's pending state on the CALLING core, for every source core. A core
    // servicing the doorbell outside its handler owes this call.
    void kickos_armv8a_gic_doorbell_clear(void);

    // Masks every line on the calling core's own bank except the doorbell.
    void kickos_armv8a_gic_doorbell_only(void);

    // What a backend calls on the calling core when the doorbell arrives. Must not take the
    // kernel lock: an initiator holding it waits on this.
    void kickos_arm64_doorbell_service(void);
#endif
}

#endif
