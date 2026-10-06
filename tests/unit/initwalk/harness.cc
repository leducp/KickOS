// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#include "harness.h"

#include "drivers.h"

#include "walk.h"

#include <gtest/gtest.h>

#include <string.h>

namespace systems
{
    System const& find(char const* name)
    {
        for (size_t i = 0; i < COUNT; i++)
        {
            if (strcmp(ALL[i].name, name) == 0)
            {
                return ALL[i];
            }
        }
        ADD_FAILURE() << "no system named " << name;
        return ALL[0];
    }
}

namespace harness
{
    fake::Config config(systems::System const& system)
    {
        fake::Config c;
        c.cap_reserved = system.cap_reserved;
        c.amp_ports = system.amp_ports;
        c.multicore = system.build.multicore;
        c.translating = system.build.translating;
        c.sp_masked = system.build.sp_masked;
        c.stride = system.limits.stride;
        c.min_stack = system.limits.min_stack;
        c.limits.cap_table = system.limits.cap_table;
        c.limits.tasks = system.limits.tasks;
        c.limits.threads = system.limits.threads;
        c.limits.endpoints = system.limits.endpoints;
        c.limits.notifications = system.limits.notifications;
        c.limits.irq_handles = system.limits.irq_handles;
        c.limits.domains = system.limits.domains;
        c.limits.free_regions = system.limits.free_regions;
        c.limits.ram_owners = system.limits.ram_owners;
        c.limits.task_endpoints = system.limits.task_endpoints;
        c.limits.task_notifications = system.limits.task_notifications;
        c.limits.task_lines = system.limits.task_lines;
        c.kernel_cores = system.limits.kernel_cores;
        drivers::behaviour = drivers::Behaviour{};
        return c;
    }

    namespace
    {
        systems::System const* g_ran = nullptr;
    }

    Outcome run(systems::System const& system)
    {
        g_ran = &system;
        fake::on_spawn = drivers::spawned;
        kickos::init::Walk walk{*system.table, system.build};
        try
        {
            walk.run();
        }
        catch (fake::Panic const& panic)
        {
            return Outcome{Outcome::Kind::PANIC, panic.message, 0};
        }
        catch (fake::Shutdown const& shutdown)
        {
            return Outcome{Outcome::Kind::SHUTDOWN, "", shutdown.status};
        }
        catch (fake::Idle const&)
        {
            return Outcome{Outcome::Kind::IDLE, "", 0};
        }
    }

    uint16_t index_of(systems::System const& system, char const* name)
    {
        kos_table_header const* const header = *system.table;
        auto const tasks = reinterpret_cast<kos_table_task const*>(header + 1);
        auto const grants = reinterpret_cast<kos_table_grant const*>(tasks + header->task_count);
        auto const refs = reinterpret_cast<kos_table_ref const*>(grants + header->grant_count);
        auto const privs = reinterpret_cast<kos_table_priv const*>(refs + header->ref_count);
        auto const regions = reinterpret_cast<kos_table_region const*>(privs + header->priv_count);
        auto const strings = reinterpret_cast<char const*>(regions + header->region_count);
        for (uint16_t i = 0; i < header->task_count; i++)
        {
            if (strcmp(&strings[tasks[i].name], name) == 0)
            {
                return i;
            }
        }
        ADD_FAILURE() << "no task named " << name << " in " << system.name;
        return 0;
    }

    Patched::Patched(systems::System const& system)
        : system_{system}
    {
        kos_table_header const* const header = *system.table;
        size_t const size = sizeof(kos_table_header) + header->task_count * sizeof(kos_table_task)
                            + header->grant_count * sizeof(kos_table_grant) + header->ref_count * sizeof(kos_table_ref)
                            + header->priv_count * sizeof(kos_table_priv)
                            + header->region_count * sizeof(kos_table_region) + header->strings_size;
        bytes_.resize((size + sizeof(uint64_t) - 1u) / sizeof(uint64_t));
        memcpy(bytes_.data(), header, size);
        header_ = reinterpret_cast<kos_table_header const*>(bytes_.data());
        system_.table = &header_;
    }

    systems::System const& Patched::system() const
    {
        return system_;
    }

    kos_table_header& Patched::header()
    {
        return *const_cast<kos_table_header*>(header_);
    }

    kos_table_task& Patched::task(char const* name)
    {
        auto const tasks = reinterpret_cast<kos_table_task*>(const_cast<kos_table_header*>(header_) + 1);
        return tasks[index_of(system_, name)];
    }

    kos_table_grant& Patched::grant(char const* name, uint16_t k)
    {
        auto const tasks = reinterpret_cast<kos_table_task*>(const_cast<kos_table_header*>(header_) + 1);
        auto const grants = reinterpret_cast<kos_table_grant*>(tasks + header_->task_count);
        return grants[task(name).first_grant + k];
    }

    kickos::init::TaskRecord const& record(uint16_t task)
    {
        auto const block = static_cast<kickos::init::PrivateRecord const*>(fake::reservations().at(0).base);
        return block[task].task;
    }

    kickos::init::StatusFields status(uint16_t task)
    {
        kickos::init::StatusFields const kept = kickos::init::status_of(record(task));
        kos_table_header const* const header = *g_ran->table;
        auto const tasks = reinterpret_cast<kos_table_task const*>(header + 1);
        auto const grants = reinterpret_cast<kos_table_grant const*>(tasks + header->task_count);
        auto const refs = reinterpret_cast<kos_table_ref const*>(grants + header->grant_count);
        for (uint16_t w = 0; w < header->task_count; w++)
        {
            for (uint16_t k = 0; k < tasks[w].watch_count; k++)
            {
                if (refs[tasks[w].first_watch + k].task != task)
                {
                    continue;
                }
                kickos::init::StatusFields read{};
                EXPECT_TRUE(kickos::init::status_read(&record(w).status[k], &read)) << "a status record held odd";
                EXPECT_EQ(read.deaths, kept.deaths) << "watcher " << w << "'s record " << k;
                EXPECT_EQ(read.restarts_left, kept.restarts_left) << "watcher " << w << "'s record " << k;
                EXPECT_EQ(read.state, kept.state) << "watcher " << w << "'s record " << k;
            }
        }
        return kept;
    }
}

namespace step
{
    Step ready_all()
    {
        return []
        {
            fake::ready_all();
        };
    }

    Step ready(char const* name)
    {
        return [name]
        {
            fake::ready(name);
        };
    }

    Step die(char const* name)
    {
        return [name]
        {
            fake::die(name);
        };
    }

    Step end(char const* name, int status)
    {
        return [name, status]
        {
            fake::end(name, status);
        };
    }

    Step raise(uint32_t bit)
    {
        return [bit]
        {
            fake::raise_bit(bit);
        };
    }

    Step nothing()
    {
        return []
        {
        };
    }
}

// Every entry the systems' tables name, which the walk never calls; drivers.cc defines the starts.
extern "C"
{
    struct kos_service_cfg;

    void sensor_main(kos_self_t const*)
    {
    }

    void app_main(kos_self_t const*)
    {
    }

    void health_main(kos_self_t const*)
    {
    }

    void kickos_main(kos_self_t const*)
    {
    }

    void slow_main(kos_self_t const*)
    {
    }

    void mid_main(kos_self_t const*)
    {
    }

    void late_main(kos_self_t const*)
    {
    }

    void top_main(kos_self_t const*)
    {
    }

    void watcher_main(kos_self_t const*)
    {
    }

}
