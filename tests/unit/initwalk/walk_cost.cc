// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The cost model is binding (docs/design-m10-target.md, section 3): for each golden composition,
// each board's default composition and each of the walk's own fixtures, the walk started with
// every server becoming ready spends at its peak exactly what admission counts for the same table
// and manifest, and spends no more through a restart and a failed start.

#include "harness.h"

#include <kickos/sys/errno.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{
    enum class Scenario
    {
        FIRST,   // every task's first start
        RESTART, // then the first restartable user task dies and is started again
        FAILED,  // that task's first start fails, and it is started again
        DRIVER_RESTART, // then the first restartable packaged driver dies and is started again
        DRIVER_FAILED   // that driver's first start fails, and it is started again
    };

    struct Case
    {
        std::string system;
        Scenario scenario;
    };

    // The first user task, or packaged driver, with a restart, or null.
    char const* restartable(systems::System const& system, bool driver = false)
    {
        kos_table_header const* const header = *system.table;
        auto const tasks = reinterpret_cast<kos_table_task const*>(header + 1);
        for (uint16_t i = 0; i < header->task_count; i++)
        {
            if ((tasks[i].driver != KOS_TABLE_NONE) == driver and tasks[i].restart_max != 0u)
            {
                auto const grants = reinterpret_cast<kos_table_grant const*>(tasks + header->task_count);
                auto const refs = reinterpret_cast<kos_table_ref const*>(grants + header->grant_count);
                auto const privs = reinterpret_cast<kos_table_priv const*>(refs + header->ref_count);
                auto const regions = reinterpret_cast<kos_table_region const*>(privs + header->priv_count);
                return reinterpret_cast<char const*>(regions + header->region_count) + tasks[i].name;
            }
        }
        return nullptr;
    }

    std::vector<Case> cases()
    {
        std::vector<Case> out;
        for (size_t i = 0; i < systems::COUNT; i++)
        {
            out.push_back(Case{systems::ALL[i].name, Scenario::FIRST});
            if (systems::ALL[i].refusal == nullptr and restartable(systems::ALL[i]) != nullptr)
            {
                out.push_back(Case{systems::ALL[i].name, Scenario::RESTART});
                out.push_back(Case{systems::ALL[i].name, Scenario::FAILED});
            }
            if (systems::ALL[i].refusal == nullptr and restartable(systems::ALL[i], true) != nullptr)
            {
                out.push_back(Case{systems::ALL[i].name, Scenario::DRIVER_RESTART});
                out.push_back(Case{systems::ALL[i].name, Scenario::DRIVER_FAILED});
            }
        }
        return out;
    }

    std::string label(Case const& c)
    {
        if (c.scenario == Scenario::RESTART)
        {
            return c.system + "_restart";
        }
        if (c.scenario == Scenario::FAILED)
        {
            return c.system + "_failed";
        }
        if (c.scenario == Scenario::DRIVER_RESTART)
        {
            return c.system + "_driver_restart";
        }
        if (c.scenario == Scenario::DRIVER_FAILED)
        {
            return c.system + "_driver_failed";
        }
        return c.system + "_first";
    }

    class Cost : public ::testing::TestWithParam<Case>
    {
    };

    TEST_P(Cost, the_init_spends_what_admission_counts)
    {
        Case const& c = GetParam();
        systems::System const& system = systems::find(c.system.c_str());
        kos_table_header const* const header = *system.table;
        fake::reset(harness::config(system));
        std::vector<step::Step> steps;
        for (uint16_t i = 0; i < header->task_count; i++)
        {
            steps.push_back(step::ready_all());
        }
        bool const driver = c.scenario == Scenario::DRIVER_RESTART or c.scenario == Scenario::DRIVER_FAILED;
        char const* const task = restartable(system, driver);
        uint16_t index = 0;
        if (task != nullptr)
        {
            index = harness::index_of(system, task);
        }
        if (c.scenario == Scenario::DRIVER_RESTART)
        {
            steps.push_back([index]
                            {
                                fake::die_task(harness::record(index).handle);
                            });
        }
        if (c.scenario == Scenario::DRIVER_FAILED)
        {
            // Its watch, the one naming the endpoint the init created for it.
            fake::refuse_if("kos_task_watch", -KOS_EACCES, 1,
                            [index](fake::Call const& call)
                            {
                                return call.args[2] == harness::record(index).endpoint;
                            });
        }
        if (c.scenario == Scenario::RESTART)
        {
            steps.push_back(step::die(task));
        }
        if (c.scenario == Scenario::FAILED)
        {
            std::string const name = task;
            fake::refuse_if("kos_thread_create", -KOS_ENOMEM, 1,
                            [name](fake::Call const& call)
                            {
                                return call.text == name;
                            });
        }
        for (uint16_t i = 0; i < header->task_count; i++)
        {
            steps.push_back(step::ready_all());
        }
        fake::script(steps);
        harness::Outcome const outcome = harness::run(system);

        if (system.refusal != nullptr)
        {
            ASSERT_EQ(outcome.kind, harness::Outcome::Kind::PANIC);
            EXPECT_EQ(outcome.message, system.refusal);
            EXPECT_TRUE(fake::calls_of("kos_task_create").empty());
            return;
        }
        ASSERT_EQ(outcome.kind, harness::Outcome::Kind::IDLE) << outcome.message;
        uint32_t live = 0;
        for (fake::Thread const& t : fake::threads())
        {
            if (t.live)
            {
                live++;
            }
        }
        EXPECT_EQ(live, system.figures.threads) << "every task running";
        if (c.scenario == Scenario::RESTART)
        {
            EXPECT_EQ(fake::spawned(task).size(), 2u) << "started again";
        }
        if (c.scenario == Scenario::FAILED)
        {
            EXPECT_EQ(harness::status(index).deaths, 1u) << "a failed start";
            EXPECT_EQ(fake::spawned(task).size(), 1u) << "then started";
        }
        if (driver)
        {
            EXPECT_EQ(harness::status(index).deaths, 1u);
            EXPECT_EQ(fake::calls_of("kos_task_watch").size(), header->task_count + 1u) << "started again";
        }

        fake::Peaks const& peak = fake::peaks();
        systems::Figures const& counted = system.figures;
        EXPECT_EQ(peak.cap_slots, counted.cap_slots);
        EXPECT_EQ(peak.endpoints, counted.endpoints);
        EXPECT_EQ(peak.notifications, counted.notifications);
        EXPECT_EQ(peak.threads, counted.threads);
        EXPECT_EQ(peak.tasks, counted.tasks);
        EXPECT_EQ(peak.irq_handles, counted.irq_handles);
        EXPECT_EQ(peak.domains, counted.domains);
        EXPECT_EQ(peak.reservations, counted.reservations);
        EXPECT_EQ(peak.self_grants, counted.self_grants);
    }

    INSTANTIATE_TEST_SUITE_P(System, Cost, ::testing::ValuesIn(cases()),
                             [](::testing::TestParamInfo<Case> const& p)
                             {
                                 return label(p.param);
                             });
}
