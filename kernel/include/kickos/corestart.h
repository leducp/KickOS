// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A peer core's bring-up handshake: one cell per direction per core. The kernel publishes
// that a core's control block is complete, and that core publishes that it has committed to
// its own scheduler.

#ifndef KICKOS_CORESTART_H
#define KICKOS_CORESTART_H

#include <stdint.h>

#include <kickos/arch/arch.h>

namespace kickos
{
#if KICKOS_KERNEL_CORES > 1
    // Publish every core named in `cores` as seated. CALL ONCE ITS CONTROL BLOCK IS COMPLETE:
    // the store releases those writes to the acquire in kickos_kernel_core_seated.
    void corestart_seat(uint32_t cores);

    // Whether `core` has committed to its own scheduler. A core outside the built range
    // answers false.
    bool corestart_arrived(uint32_t core);
#endif
}

#endif
