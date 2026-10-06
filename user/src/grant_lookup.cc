// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The lookups of <kickos/sys.h> over the emitted table.

#include <kickos/sys.h>
#include <kickos/sys/errno.h>
#include <kickos/sys/init_status.h>
#include <kickos/sys/table.h>

#include <stddef.h>
#include <stdint.h>

namespace
{
    constexpr uint32_t STATUS_RETRIES = 100u;
    constexpr uint64_t STATUS_RETRY_NS = 1000000ull;

    kos_window_t const NO_WINDOW = {nullptr, 0u};

    struct Table
    {
        kos_table_task const* task;
        kos_table_grant const* grant;
        kos_table_ref const* ref;
        kos_table_region const* region;
        char const* strings;
    };

    // The arrays follow the header in this order with no gap between them.
    Table table()
    {
        kos_table_header const* const header = kickos_table;
        Table t;
        t.task = reinterpret_cast<kos_table_task const*>(header + 1);
        t.grant = reinterpret_cast<kos_table_grant const*>(t.task + header->task_count);
        t.ref = reinterpret_cast<kos_table_ref const*>(t.grant + header->grant_count);
        auto const priv = reinterpret_cast<kos_table_priv const*>(t.ref + header->ref_count);
        t.region = reinterpret_cast<kos_table_region const*>(priv + header->priv_count);
        t.strings = reinterpret_cast<char const*>(t.region + header->region_count);
        return t;
    }

    bool same_name(char const* a, char const* b)
    {
        size_t i = 0;
        while (a[i] == b[i])
        {
            if (a[i] == '\0')
            {
                return true;
            }
            i++;
        }
        return false;
    }

    // The first grant of `self` named `name` whose kind is `kind` or `also`.
    kos_table_grant const* find(Table const& t, kos_self_t const* self, char const* name,
                                uint8_t kind, uint8_t also)
    {
        if (self == nullptr or name == nullptr)
        {
            return nullptr;
        }
        for (uint16_t i = 0; i < self->grant_count; i++)
        {
            kos_table_grant const* const grant = &t.grant[self->first_grant + i];
            if ((grant->kind == kind or grant->kind == also) and same_name(&t.strings[grant->name], name))
            {
                return grant;
            }
        }
        return nullptr;
    }

    kos_cap_t cap_of(kos_self_t const* self, char const* name, uint8_t kind, uint8_t also)
    {
        Table const t = table();
        kos_table_grant const* const grant = find(t, self, name, kind, also);
        if (grant == nullptr or grant->cap_slot == KOS_TABLE_NONE)
        {
            return KOS_CAP_NONE;
        }
        return grant->cap_slot;
    }

    // The spawn window at the grant's place, refused unless it has `kind` and the grant's size
    // and flags.
    kos_window_t window_of(Table const& t, kos_table_grant const* grant, uint8_t kind)
    {
        if (grant == nullptr or grant->window == KOS_TABLE_NONE)
        {
            return NO_WINDOW;
        }
        uint32_t size = grant->size;
        if (grant->kind == KOS_GRANT_REGION)
        {
            size = t.region[grant->target].size;
        }
        struct kos_window window;
        if (kos_window_get(grant->window, &window) != 0 or window.kind != kind or window.size != size
            or window.flags != grant->flags)
        {
            return NO_WINDOW;
        }
        return kos_window_t{reinterpret_cast<void*>(window.base), window.size};
    }

    kos_window_t window_named(kos_self_t const* self, char const* name, uint8_t grant_kind,
                              uint8_t window_kind)
    {
        Table const t = table();
        return window_of(t, find(t, self, name, grant_kind, grant_kind), window_kind);
    }
}

extern "C"
{

kos_cap_t kos_grant_endpoint(kos_self_t const* self, char const* name)
{
    kos_cap_t const cap = cap_of(self, name, KOS_GRANT_ENDPOINT_SERVE, KOS_GRANT_ENDPOINT_USE);
    if (cap != KOS_CAP_NONE)
    {
        return cap;
    }
    return cap_of(self, name, KOS_GRANT_PORT, KOS_GRANT_PORT);
}

kos_cap_t kos_grant_notify(kos_self_t const* self, char const* name)
{
    return cap_of(self, name, KOS_GRANT_NOTIFICATION, KOS_GRANT_NOTIFICATION);
}

kos_window_t kos_grant_mmio(kos_self_t const* self, char const* name)
{
    return window_named(self, name, KOS_GRANT_WINDOW, KOS_WINDOW_DEVICE);
}

kos_window_t kos_grant_mem(kos_self_t const* self, char const* name)
{
    return window_named(self, name, KOS_GRANT_REGION, KOS_WINDOW_MEMORY);
}

kos_window_t kos_grant_ports(kos_self_t const* self, char const* name)
{
    return window_named(self, name, KOS_GRANT_PORTS, KOS_WINDOW_PORTS);
}

kos_line_t kos_grant_irq(kos_self_t const* self, char const* name)
{
    Table const t = table();
    kos_table_grant const* const grant = find(t, self, name, KOS_GRANT_LINE, KOS_GRANT_LINE);
    if (grant == nullptr or grant->cap_slot == KOS_TABLE_NONE)
    {
        return kos_line_t{KOS_CAP_NONE, KOS_TABLE_NONE};
    }
    return kos_line_t{grant->cap_slot, grant->line_index};
}

int kos_task_status(kos_self_t const* self, uint32_t i, struct kos_task_status* out)
{
    if (self == nullptr or out == nullptr or i >= self->watch_count)
    {
        return -KOS_EINVAL;
    }
    Table const t = table();
    kos_table_grant const* grant = nullptr;
    for (uint16_t k = 0; k < self->grant_count and grant == nullptr; k++)
    {
        if (t.grant[self->first_grant + k].kind == KOS_GRANT_STATUS)
        {
            grant = &t.grant[self->first_grant + k];
        }
    }
    kos_window_t const block = window_of(t, grant, KOS_WINDOW_MEMORY);
    if (block.addr == nullptr)
    {
        return -KOS_EINVAL;
    }
    // The watcher's own block holds the tasks it watches alone, in `watches` order.
    uint16_t const task = t.ref[self->first_watch + i].task;
    auto const record = static_cast<kickos::init::StatusRecord const*>(block.addr) + i;

    kickos::init::StatusFields fields;
    uint32_t retries = 0;
    while (not kickos::init::status_read(record, &fields))
    {
        if (retries == STATUS_RETRIES)
        {
            return -KOS_EAGAIN;
        }
        kos_sleep_ns(STATUS_RETRY_NS);
        retries++;
    }
    out->name = &t.strings[t.task[task].name];
    out->alive = (fields.state & kickos::init::STATUS_ALIVE) != 0u;
    out->deaths = fields.deaths;
    out->restarts_left = fields.restarts_left;
    out->dependency_down = (fields.state & kickos::init::STATUS_DEPENDENCY_DOWN) != 0u;
    return 0;
}

}
