// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A packaged driver's bring-up in its own task, from the task's entry: the driver a declaration
// names, and the drivers a task hosts. Linked into every task's image, never the init's.

#include "driver_path.h"

#include <kickos/sys/driver_service.h>
#include <kickos/sys/emit.h> // kconsole_write_all
#include <kickos/sys/image.h>
#include <kickos/sys/table.h>
#include <kickos/sys/task_enter.h>

#include <string.h>

// Every task image takes it, a program's included.
KOS_IMAGE_SYSTEM_OBJECT()

namespace
{
    struct Table
    {
        kos_table_task const* tasks;
        kos_table_grant const* grants;
        char const* strings;
    };

    Table table()
    {
        auto const header = kickos_table;
        auto const tasks = reinterpret_cast<kos_table_task const*>(header + 1);
        auto const grants = reinterpret_cast<kos_table_grant const*>(tasks + header->task_count);
        auto const refs = reinterpret_cast<kos_table_ref const*>(grants + header->grant_count);
        auto const privs = reinterpret_cast<kos_table_priv const*>(refs + header->ref_count);
        auto const regions = reinterpret_cast<kos_table_region const*>(privs + header->priv_count);
        return Table{tasks, grants, reinterpret_cast<char const*>(regions + header->region_count)};
    }

    kos_driver_instance instance_of(Table const& tb, kos_table_task const* t, uintptr_t block)
    {
        kos_driver_instance in = {};
        in.name = &tb.strings[t->name];
        in.priority = t->priority;
        in.ceiling = t->ceiling;
        in.core_mask = t->core_mask;
        in.authority = t->authority;
        in.block = reinterpret_cast<void*>(block);
        in.block_size = t->block;
        in.block_flags = kickos::init::driver_path::block_flags(*t);
        in.console = (t->flags & KOS_TABLE_TASK_CONSOLE) != 0u;
        bool window = false;
        uint16_t lines = 0;
        for (uint16_t k = 0; k < t->grant_count; k++)
        {
            kos_table_grant const& g = tb.grants[t->first_grant + k];
            if (g.kind == KOS_GRANT_WINDOW and not window)
            {
                in.mmio_base = static_cast<uintptr_t>(g.base);
                in.mmio_window = g.size;
                window = true;
            }
            if (g.kind == KOS_GRANT_LINE)
            {
                // In the order of the driver's line roles; a count past the descriptor's is
                // refused by the bring-up.
                if (lines < kickos::driver::KOS_DRV_LINES_MAX)
                {
                    in.lines[lines].number = g.line;
                    in.lines[lines].index = g.line_index;
                }
                lines++;
            }
        }
        in.line_count = static_cast<uint8_t>(lines);
        if (lines > 0xFFu)
        {
            in.line_count = 0xFFu;
        }
        return in;
    }

    // What a hosted driver's staging made, closed once its threads hold their own copies.
    struct Staged
    {
        kos_cap_t at[KICKOS_MAX_SPAWN_GRANTS];
        kos_cap_t ep;
        kos_cap_t note;
        kos_window windows[KICKOS_MAX_THREAD_WINDOWS];
        uint8_t window_count;
    };

    void close_staged(Staged const& st)
    {
        for (kos_cap_t const cap : st.at)
        {
            if (cap != KOS_CAP_NONE)
            {
                (void)kos_handle_close(cap);
            }
        }
        bool placed = false;
        for (kos_cap_t const cap : st.at)
        {
            if (cap == st.note)
            {
                placed = true;
            }
        }
        if (st.note != KOS_CAP_NONE and not placed)
        {
            (void)kos_handle_close(st.note);
        }
    }

    // The kernel console, or the host's stdout where a console task already serving it holds the
    // console: never the endpoint of a driver this host is still bringing up.
    void hosted_say(char const* s)
    {
        kickos::kconsole_write_all(s, strlen(s));
    }

    int hosted_fail(char const* name, char const* msg)
    {
        hosted_say("[");
        hosted_say(name);
        hosted_say("] ERROR: ");
        hosted_say(msg);
        return -1;
    }

    // A zero-length send, which only a receive takes, bounded as a console handover's probe is.
    // The endpoint answers -KOS_EAGAIN until its server has first received, so that is retried.
    int32_t probe(kos_cap_t ep)
    {
        uint64_t const budget_ns = (uint64_t{kickos::driver::KOS_DRV_HANDOVER_PROBE_US} * 1000u)
                                   + (uint64_t{kickos::driver::KOS_DRV_READY_WAIT_MAX}
                                      * kickos::driver::KOS_DRV_READY_WAIT_NS);
        uint64_t const deadline = kos_clock_now() + budget_ns;
        while (true)
        {
            uint64_t const now = kos_clock_now();
            if (now >= deadline)
            {
                return -KOS_ETIMEDOUT;
            }
            uint32_t const left_us = static_cast<uint32_t>((deadline - now + 999u) / 1000u);
            int32_t const rc = kos_send(ep, "", 0, left_us);
            if (rc != -KOS_EAGAIN)
            {
                return rc;
            }
            kos_sleep_ns(kickos::driver::KOS_DRV_READY_WAIT_NS);
        }
    }

    int claim(uint16_t line, unsigned int flags, kos_cap_t* out)
    {
        uint32_t retries = 0;
        while (true)
        {
            int const rc = kos_irq_claim(line, flags, out);
            if (rc != -KOS_EAGAIN or retries == kickos::init::CLAIM_RETRIES)
            {
                return rc;
            }
            retries++;
            kos_sleep_ns(kickos::init::CLAIM_RETRY_NS);
        }
    }

    // Stages hosted record `h` of task `host` as its own task's create and init would: the
    // endpoint the init passed the host, the notification and its doorbell, each line claimed and
    // bound to its bit, and the windows thread 0 takes.
    int stage(Table const& tb, uint16_t host, kos_table_task const& h, uint16_t index, Staged* st)
    {
        char const* const name = &tb.strings[h.name];
        kos_table_task const& t = tb.tasks[host];
        for (uint16_t k = 0; k < h.grant_count; k++)
        {
            kos_table_grant const& g = tb.grants[h.first_grant + k];
            if (g.cap_slot == KOS_TABLE_NONE or g.kind == KOS_GRANT_LINE)
            {
                continue;
            }
            uint16_t const at = static_cast<uint16_t>(g.cap_slot - KOS_SPAWN_DELEGATED_CAP0);
            if (at >= KICKOS_MAX_SPAWN_GRANTS)
            {
                return hosted_fail(name, "a capability is placed past a spawn's grants\n");
            }
            if (g.kind == KOS_GRANT_ENDPOINT_SERVE)
            {
                for (uint16_t j = 0; j < t.grant_count; j++)
                {
                    kos_table_grant const& e = tb.grants[t.first_grant + j];
                    if (e.kind == KOS_GRANT_ENDPOINT_HOSTED and e.target == index)
                    {
                        st->at[at] = static_cast<kos_cap_t>(e.cap_slot);
                    }
                }
                if (st->at[at] == KOS_CAP_NONE)
                {
                    return hosted_fail(name, "the task it runs in holds no endpoint of it\n");
                }
                st->ep = st->at[at];
                continue;
            }
            if (g.kind != KOS_GRANT_NOTIFICATION)
            {
                continue;
            }
            if (st->note == KOS_CAP_NONE and kos_notify_create(&st->note) != 0)
            {
                return hosted_fail(name, "kos_notify_create failed\n");
            }
            if (g.badge == 0u)
            {
                st->at[at] = st->note;
            }
            else if (kos_notify_badge(st->note, g.badge - 1u, &st->at[at]) != 0)
            {
                return hosted_fail(name, "kos_notify_badge of the doorbell failed\n");
            }
        }
        uint16_t line = 0;
        for (uint16_t k = 0; k < h.grant_count; k++)
        {
            kos_table_grant const& g = tb.grants[h.first_grant + k];
            if (g.kind == KOS_GRANT_WINDOW and st->window_count < KICKOS_MAX_THREAD_WINDOWS)
            {
                kos_window& w = st->windows[st->window_count];
                w.base = static_cast<uintptr_t>(g.base);
                w.size = g.size;
                w.kind = KOS_WINDOW_DEVICE;
                w.flags = g.flags;
                st->window_count++;
                continue;
            }
            if (g.kind != KOS_GRANT_LINE or g.cap_slot == KOS_TABLE_NONE)
            {
                continue;
            }
            uint16_t const at = static_cast<uint16_t>(g.cap_slot - KOS_SPAWN_DELEGATED_CAP0);
            if (at >= KICKOS_MAX_SPAWN_GRANTS)
            {
                return hosted_fail(name, "a line is placed past a spawn's grants\n");
            }
            if (claim(g.line, g.flags & KOS_IRQ_LEVEL, &st->at[at]) != 0)
            {
                return hosted_fail(name, "kos_irq_claim failed\n");
            }
            if (st->note != KOS_CAP_NONE)
            {
                kos_cap_t bit = KOS_CAP_NONE;
                if (kos_notify_badge(st->note, line, &bit) != 0)
                {
                    return hosted_fail(name, "kos_notify_badge of a line failed\n");
                }
                int const rc = kos_irq_bind_notify(st->at[at], bit);
                (void)kos_handle_close(bit);
                if (rc != 0)
                {
                    return hosted_fail(name, "kos_irq_bind_notify failed\n");
                }
            }
            line++;
        }
        return 0;
    }
}

extern "C" void kickos_driver_task(kos_table_task const* t, uintptr_t block)
{
    kos_driver_instance in = instance_of(table(), t, block);
    (void)t->entry.driver(&in);
    kos_exit(1);
}

extern "C" void kickos_driver_hosted(kos_task_args const* args)
{
    uint16_t const host = static_cast<uint16_t>(args->decl);
    Table const tb = table();
    for (uint16_t i = 0; i < kickos_table->task_count; i++)
    {
        kos_table_task const& h = tb.tasks[i];
        if ((h.flags & KOS_TABLE_TASK_HOSTED) == 0u or h.host != host)
        {
            continue;
        }
        Staged st = {};
        for (kos_cap_t& cap : st.at)
        {
            cap = KOS_CAP_NONE;
        }
        st.ep = KOS_CAP_NONE;
        st.note = KOS_CAP_NONE;
        // Its lines are claimed on the host's core, which admission makes the driver's.
        int rc = stage(tb, host, h, i, &st);
        if (rc == 0)
        {
            // The host's shared block is the ring block of the one driver it runs that has one.
            uintptr_t ring = 0u;
            if (h.block != 0u)
            {
                ring = args->block;
            }
            kos_driver_instance in = instance_of(tb, &h, ring);
            in.hosted = st.at;
            in.windows = st.windows;
            in.window_count = st.window_count;
            rc = h.entry.driver(&in);
        }
        if (rc == 0 and probe(st.ep) < 0)
        {
            rc = hosted_fail(&tb.strings[h.name], "its endpoint never took the readiness probe\n");
        }
        close_staged(st);
        if (rc != 0)
        {
            kos_exit(1);
        }
    }
}
