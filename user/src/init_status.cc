// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sys/init_status.h>

namespace kickos::init
{
    // The destructor's even store is the constructor's odd one plus one: a writer that did not
    // store odd leaves the count odd, and every reader refused.
    StatusWriter::StatusWriter(StatusRecord* record) : record_{record}
    {
        record_->count_ = record_->count_.load() + 1u;
    }

    StatusWriter::~StatusWriter() { record_->count_ = record_->count_.load() + 1u; }

    void StatusWriter::deaths(uint16_t value) { record_->deaths_ = value; }

    void StatusWriter::restarts_left(uint8_t value) { record_->restarts_left_ = value; }

    void StatusWriter::state(uint8_t value) { record_->state_ = value; }

    void StatusWriter::clear(StatusRecord* record)
    {
        record->count_ = 0u;
        record->deaths_ = 0u;
        record->restarts_left_ = 0u;
        record->state_ = 0u;
    }

    void status_write(StatusRecord* record, StatusFields const& fields)
    {
        StatusWriter w(record);
        w.deaths(fields.deaths);
        w.restarts_left(fields.restarts_left);
        w.state(fields.state);
    }

    bool status_read(StatusRecord const* record, StatusFields* out)
    {
        uint32_t const before = record->count_;
        if ((before & 1u) != 0u)
        {
            return false;
        }
        out->deaths = record->deaths_;
        out->restarts_left = record->restarts_left_;
        out->state = record->state_;
        return record->count_ == before;
    }
}
