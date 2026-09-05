// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The pieces of the LX6 cross-core doorbell a CHIP owns: which register raises it on a peer,
// which one this core clears, where the matrix points them, and which CPU interrupt input they
// arrive on. The request and answer cells, the service body and the kernel lock live in
// klock_lx6.cc.

#ifndef KICKOS_ARCH_LX6_DOORBELL_H
#define KICKOS_ARCH_LX6_DOORBELL_H

#include <stdint.h>

#if KICKOS_NUM_CORES > 1

extern "C"
{

// Raise the doorbell on every core named in `cores`, a bitmask of core indices. The CALLING
// core's bit is serviced by the caller and must not be raised here.
//
// THE BODY OWES THE ORDERING: it makes the caller's prior stores visible BEFORE the trigger
// write, or a receiver woken by the trigger reads a cell the sender has not yet published.
void kickos_lx6_doorbell_send(uint32_t cores);

// Drop the doorbell's pending state on the CALLING core, and make the drop visible before the
// caller's next load. CALLED BEFORE THE SERVICE READS THE CELLS, never after: that order is
// what turns the set-versus-clear race into a spurious entry instead of a lost request.
void kickos_lx6_doorbell_clear(void);

// Point this core's matrix bank at the doorbell: its own inbound source to the doorbell input,
// every other inter-core source into a sink. Called once per core at bring-up, and the only
// place this backend touches a matrix register for the doorbell.
void kickos_lx6_doorbell_route(void);

// The CPU interrupt input the route above lands on. Must be LEVEL type: the pending state then
// follows the trigger register, so clearing the trigger is the whole acknowledgement and no
// INTCLEAR is owed.
uint32_t kickos_lx6_doorbell_cpu_int(void);

// The far side, on the calling core: answers every peer that has asked. Takes NO kernel lock.
void kickos_lx6_doorbell_service(void);

// Whether any peer has asked this core for something it has not answered. THE CELL IS THE
// AUTHORITY: a spurious raise finds nothing owed and costs one return.
int kickos_lx6_doorbell_pending(void);

// Where a released secondary lands, from the chip's reset entry. Seats everything about that
// core, publishes its arrival, and parks it on the doorbell. Never returns to the caller.
void kickos_lx6_secondary_entry(void);

// The park the line above ends in, and the primary's check of the mechanism over it, both in
// klock_lx6.cc. The CHIP calls the check once every secondary has published arrival: it needs
// every peer's route live before the first raise.
//
// The park OPENS THIS CORE'S INTERRUPTS ITSELF, so the caller must have seated the route first.
void kickos_lx6_doorbell_park(void);
void kickos_lx6_doorbell_selfcheck(void);

// The arrival byte. The arriving core sets its own once it is ready to TAKE a doorbell, and the
// primary's release reads it: released is not arrived.
void kickos_lx6_core_arrived_set(void);
uint32_t kickos_lx6_core_arrived(uint32_t core);

}

#endif

#endif // KICKOS_ARCH_LX6_DOORBELL_H
