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

#include <type_traits>

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

    // `count` is odd while the init writes the fields. Every field, not only the count, is
    // acquire and release: one relaxed field lets a copy torn by a write in progress read as
    // whole, with the count even and unchanged on both sides. Only a StatusWriter stores to it.
    class StatusRecord
    {
    public:
        uint32_t count() const { return count_; }

    private:
        Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE> count_;
        Atomic<uint16_t, Order::ACQUIRE | Order::RELEASE> deaths_;
        Atomic<uint8_t, Order::ACQUIRE | Order::RELEASE> restarts_left_;
        Atomic<uint8_t, Order::ACQUIRE | Order::RELEASE> state_;

        static_assert(std::is_same_v<decltype(count_),
                                     Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE>>
                          and std::is_same_v<decltype(deaths_),
                                             Atomic<uint16_t, Order::ACQUIRE | Order::RELEASE>>
                          and std::is_same_v<decltype(restarts_left_),
                                             Atomic<uint8_t, Order::ACQUIRE | Order::RELEASE>>
                          and std::is_same_v<decltype(state_),
                                             Atomic<uint8_t, Order::ACQUIRE | Order::RELEASE>>,
                      "the seqlock's order is its fields' own: no fence holds it");

        friend class StatusWriter;
        friend bool status_read(StatusRecord const* record, StatusFields* out);
    };
    static_assert(sizeof(StatusRecord) == KICKOS_INIT_STATUS_RECORD_SIZE,
                  "a status record is the size cmake/init_geometry.cmake declares");

    // The one write of a record: the count odd from construction to destruction, the fields
    // stored in between. The init is the record's one writer.
    class StatusWriter
    {
    public:
        explicit StatusWriter(StatusRecord* record);
        ~StatusWriter();
        StatusWriter(StatusWriter const&) = delete;
        StatusWriter& operator=(StatusWriter const&) = delete;

        void deaths(uint16_t value);
        void restarts_left(uint8_t value);
        void state(uint8_t value);

        // The record's state before its first write: the count even at 0 and every field 0.
        // The init clears each record of a block it reserves before it writes one.
        static void clear(StatusRecord* record);

    private:
        StatusRecord* record_;
    };

    void status_write(StatusRecord* record, StatusFields const& fields);

    // One copy of the fields, false where the count was odd or changed across the copy.
    bool status_read(StatusRecord const* record, StatusFields* out);
}

#endif
