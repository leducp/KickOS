// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Proof that the caller was handed the kernel's exclusion. An entry that requires the exclusion
// takes one, and only an IrqLock (kickos/irqlock.h) or the IPC fastpath, entered under the
// trap's own mask, can make one. The token is copyable and storable: it shows the caller was
// given a lock, not that the lock is held now.

#ifndef KICKOS_HELD_H
#define KICKOS_HELD_H

#include <stdint.h>

struct arch_context;

namespace kickos
{
    extern "C" struct arch_context* kickos_ipc_fastpath(uint32_t* args);

    // Empty and passed by value. alignas(4): the RX ABI passes an
    // aggregate whose size is not a whole number of words on the stack, every frame of a chain.
    class alignas(4) Held final
    {
        Held() {}
        friend class IrqLock;
        friend ::arch_context* kickos_ipc_fastpath(uint32_t* args);
    };
}

#endif
