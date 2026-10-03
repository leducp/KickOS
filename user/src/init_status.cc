// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include <kickos/sys/init_status.h>

namespace kickos::init
{
    void status_clear(StatusRecord* record)
    {
        record->count = 0u;
        record->deaths = 0u;
        record->restarts_left = 0u;
        record->state = 0u;
    }

    void status_write(StatusRecord* record, StatusFields const& fields)
    {
        // The caller is the one writer, so this reads back its own last store.
        uint32_t const count = record->count;
        record->count = count + 1u;
        fence_release();
        record->deaths = fields.deaths;
        record->restarts_left = fields.restarts_left;
        record->state = fields.state;
        record->count = count + 2u;
    }

    bool status_read(StatusRecord const* record, StatusFields* out)
    {
        uint32_t const before = record->count;
        if ((before & 1u) != 0u)
        {
            return false;
        }
        fence_acquire();
        out->deaths = record->deaths;
        out->restarts_left = record->restarts_left;
        out->state = record->state;
        fence_acquire();
        return record->count == before;
    }
}
