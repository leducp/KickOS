// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The one piece of the rv64imac cross-hart doorbell a CHIP owns: which register raises a
// machine software interrupt on a peer, and where it is. Everything above it, the request and
// answer cells, the service body and the kernel lock, is architectural and lives in
// klock_rv64imac.cc.
//
// A raise arrives at the peer as a MACHINE software interrupt, cause 3, which mideleg holds
// read-only zero (measured on qemu-riscv64: writing all ones reads back 0x3666). The chip's
// machine-mode trampoline lowers it to mip.SSIP, where the supervisor dispatch demuxes it.

#ifndef KICKOS_ARCH_RV64_DOORBELL_H
#define KICKOS_ARCH_RV64_DOORBELL_H

#include <stdint.h>

#if KICKOS_NUM_CORES > 1

extern "C"
{

// Raise the doorbell on every core named in `cores`, a bitmask of core indices. The CALLING
// core's bit is serviced by the caller and must not be raised here.
//
// A publish the far side must observe is ordered by the caller ahead of this, so the body owes
// the fence that makes its own stores visible before the raise.
//
// SUPERVISOR MODE MAY DO THIS WRITE WITH NO MACHINE-MODE LEG: PMP entry 0 grants the whole
// space and QEMU's CLINT gates the msip write on no privilege, both measured on qemu-riscv64.
void kickos_rv64_doorbell_send(uint32_t cores);

// The far side, on the calling core: answers every peer that has asked. Takes NO kernel lock.
void kickos_rv64_doorbell_service(void);

// Whether any peer has asked this core for something it has not answered. THE CELL, NOT THE
// RAISE, is what says a service is owed: sip.SSIP carries the local device-line injection too.
int kickos_rv64_doorbell_pending(void);

// Where a released secondary lands: seats this hart's supervisor state, publishes its arrival
// and parks it on the doorbell. Never returns. Called from the chip's supervisor landing pad.
void kickos_rv64_secondary_entry(void) __attribute__((noreturn));

// One poke and one wait over `peers`: every serviced hart runs the SFENCE.VMA in the doorbell's
// service body, which is the only way one hart's translation is reached from another here.
void kickos_rv64_translation_rendezvous(uint32_t peers);

// Nonzero while the software controller is carrying a raise no dispatch has taken. The poll
// clears the one cause every raise arrives on, so it must know what it did not service.
int kickos_rv64_inject_owed(void);

// Nonzero once hart `id` has reached the park. Read by the chip's arrival wait.
uint32_t kickos_rv64_core_online_read(uint32_t id);

}

#endif

#endif // KICKOS_ARCH_RV64_DOORBELL_H
