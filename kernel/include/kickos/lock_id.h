// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A thread's lock id: (generation << LOCK_ID_SLOT_BITS) | (slot + 1), in 31 bits, 0 meaning
// none. The kernel seats each thread's own where that thread reads it with one load.

#ifndef KICKOS_LOCK_ID_H
#define KICKOS_LOCK_ID_H

#include <stdint.h>

#include <kickos/config/system.h> // KICKOS_THREAD_SLOTS
#include <kickos/slotpool.h>

#if KICKOS_ARCH_SIM
#include <kickos/instance_local.h>
#endif

namespace kickos
{
    constexpr int lock_id_width(uint32_t n)
    {
        int bits = 0;
        while (n != 0)
        {
            bits++;
            n = n >> 1;
        }
        return bits;
    }

    // Wide enough for slot + 1 == KICKOS_THREAD_SLOTS.
    constexpr int LOCK_ID_SLOT_BITS = lock_id_width(KICKOS_THREAD_SLOTS);
    constexpr uint32_t LOCK_ID_SLOT_MASK = (1u << LOCK_ID_SLOT_BITS) - 1u;
    constexpr int LOCK_ID_GEN_BITS = 31 - LOCK_ID_SLOT_BITS;
    constexpr uint32_t LOCK_ID_GEN_MASK = (1u << LOCK_ID_GEN_BITS) - 1u;
    static_assert(LOCK_ID_GEN_BITS >= 16,
                  "a lock id must carry the handle's whole 16-bit generation, or the handle "
                  "derived from it names a stale occupant");

    // `gen` is the slot's 32-bit generation, truncated here to the bits the id has room for.
    constexpr uint32_t lock_id_pack(uint32_t gen, uint32_t slot)
    {
        return ((gen & LOCK_ID_GEN_MASK) << LOCK_ID_SLOT_BITS) | (slot + 1u);
    }

    // The kos_thread_t of the thread whose id this is: the generation's low 16 bits over the
    // slot index.
    constexpr uint32_t lock_id_handle(uint32_t id)
    {
        return handle_pack(static_cast<uint16_t>(id >> LOCK_ID_SLOT_BITS),
                           (id & LOCK_ID_SLOT_MASK) - 1u);
    }

#if KICKOS_ARCH_SIM
    // The running thread's id, rewritten at every switch.
    extern InstanceLocal<uint32_t> sim_self_seat;
#endif
}

#endif
