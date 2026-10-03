// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A watcher's status block: one record per task it watches, in `watches` order, written by the
// init alone and mapped read-only into that watcher as /init/status.

#ifndef KICKOS_SYS_INIT_STATUS_H
#define KICKOS_SYS_INIT_STATUS_H

#include <kickos/sys/atomic.h>
#include <kickos/sys/init_geometry.h>

#include <stdint.h>

namespace kickos::init
{
    // StatusFields::state
    enum : uint8_t
    {
        STATUS_ALIVE = 1u << 0,          // running, or a restart of it is coming
        STATUS_DEPENDENCY_DOWN = 1u << 1 // never started: a server it uses is dead for good
    };

    struct StatusFields
    {
        uint16_t deaths;
        uint8_t restarts_left;
        uint8_t state;
    };

    // `count` is odd while the init writes the fields. Its loads are relaxed: the reader orders
    // them with fence_acquire.
    struct StatusRecord
    {
        Atomic<uint32_t, Order::RELEASE> count;
        Atomic<uint16_t, Order::RELAXED> deaths;
        Atomic<uint8_t, Order::RELAXED> restarts_left;
        Atomic<uint8_t, Order::RELAXED> state;
    };
    static_assert(sizeof(StatusRecord) == KICKOS_INIT_STATUS_RECORD_SIZE,
                  "a status record is the size cmake/init_geometry.cmake declares");

    // The record's state before its first write: the count even at 0 and every field 0. The init
    // clears each record of a block it reserves before it writes one.
    void status_clear(StatusRecord* record);

    // The init's write: the count odd, a release fence, the fields, the count even. The init is
    // the record's one writer.
    void status_write(StatusRecord* record, StatusFields const& fields);

    // One copy of the fields, false where the count was odd or changed across the copy.
    bool status_read(StatusRecord const* record, StatusFields* out);
}

#endif
