// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The init's walk against the scripted kernel, rule by rule (docs/design-m10-target.md, sections
// 1 and 8). The systems are emitted by the tool: the goldens, the board defaults, `alarm` (the
// arm64 golden whose sensor takes the RTC's alarm line on core 1), `init_priority` (the arm64
// golden stating `init: { priority: 7 }`), and `chain` and `chain_ends` (sensor and slow serve,
// mid uses sensor and serves, late uses both servers, top uses mid, and watcher watches sensor,
// mid, top and late, in that order).

#include "harness.h"

#include <kickos/sys/driver_service.h>
#include <kickos/sys/errno.h>
#include <kickos/sys/init_status.h>
#include <kickos/sys/task_trampoline.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace
{
    using harness::Outcome;
    using kickos::init::StatusFields;
    using Steps = std::vector<std::function<void()>>;

    constexpr kos_cap_t CAP0 = KOS_SPAWN_DELEGATED_CAP0;

    struct TableView
    {
        kos_table_header const* header;
        kos_table_task const* tasks;
        kos_table_grant const* grants;
        kos_table_region const* regions;
    };

    TableView view(systems::System const& system)
    {
        TableView v;
        v.header = *system.table;
        v.tasks = reinterpret_cast<kos_table_task const*>(v.header + 1);
        v.grants = reinterpret_cast<kos_table_grant const*>(v.tasks + v.header->task_count);
        auto const refs = reinterpret_cast<kos_table_ref const*>(v.grants + v.header->grant_count);
        auto const privs = reinterpret_cast<kos_table_priv const*>(refs + v.header->ref_count);
        v.regions = reinterpret_cast<kos_table_region const*>(privs + v.header->priv_count);
        return v;
    }

    // The names of the entry threads spawned so far, in order.
    std::vector<std::string> spawned()
    {
        std::vector<std::string> names;
        for (fake::Thread const& t : fake::threads())
        {
            names.push_back(t.name);
        }
        return names;
    }

    size_t starts(char const* name)
    {
        return fake::spawned(name).size();
    }

    // The init's served endpoint object for the task spawned as `name`, from its first spawn.
    size_t served_object(char const* name, uint16_t slot)
    {
        return fake::spawned(name).at(0)->caps.at(slot).object;
    }

    class Walk : public ::testing::Test
    {
    protected:
        void use(char const* name)
        {
            system_ = &systems::find(name);
            fake::reset(harness::config(*system_));
        }

        void use(harness::Patched const& patched)
        {
            system_ = &patched.system();
            fake::reset(harness::config(*system_));
        }

        Outcome run()
        {
            return harness::run(*system_);
        }

        uint16_t index(char const* task)
        {
            return harness::index_of(*system_, task);
        }

        StatusFields status(char const* task)
        {
            return harness::status(index(task));
        }

        systems::System const* system_ = nullptr;
    };

    void expect_idle(Outcome const& outcome)
    {
        EXPECT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;
    }

    // --- boot --------------------------------------------------------------------------------

    TEST_F(Walk, boot_holds_what_the_table_needs_in_order)
    {
        use("golden_arm64");
        expect_idle(run());
        std::vector<std::string> const boot = fake::sequence(
            {"kos_notify_create", "kos_notify_bind", "kos_ram_alloc", "kos_mem_self_grant", "kos_endpoint_create",
             "kos_cap_narrow", "kos_task_create"});
        std::vector<std::string> const expected = {
            "kos_notify_create", "kos_notify_bind", "kos_ram_alloc", "kos_mem_self_grant", "kos_ram_alloc",
            "kos_mem_self_grant", "kos_ram_alloc", "kos_ram_alloc", "kos_ram_alloc", "kos_ram_alloc",
            "kos_endpoint_create", "kos_cap_narrow", "kos_notify_create"};
        ASSERT_GE(boot.size(), expected.size());
        EXPECT_EQ(std::vector<std::string>(boot.begin(), boot.begin() + static_cast<ptrdiff_t>(expected.size())),
                  expected);

        // The private block with one record per task and per region, health's status block with
        // one record per task it watches, the region, then each task's stack.
        TableView const v = view(*system_);
        std::vector<fake::Block> const& r = fake::reservations();
        ASSERT_EQ(r.size(), 6u);
        EXPECT_EQ(r[0].size, 4u * KICKOS_INIT_PRIVATE_RECORD_SIZE);
        EXPECT_EQ(r[1].size, 1u * KICKOS_INIT_STATUS_RECORD_SIZE);
        EXPECT_EQ(r[2].size, v.regions[0].size);
        EXPECT_EQ((std::vector<size_t>{r[3].size, r[4].size, r[5].size}), (std::vector<size_t>{4096u, 8192u, 4096u}));
        ASSERT_EQ(fake::self_grants().size(), 2u);
        EXPECT_EQ(fake::self_grants()[0].base, r[0].base);
        EXPECT_EQ(fake::self_grants()[1].base, r[1].base);

        // The served endpoint kept with HANDOUT and without WAIT.
        std::vector<fake::Call> const narrow = fake::calls_of("kos_cap_narrow");
        ASSERT_EQ(narrow.size(), 1u);
        EXPECT_EQ(narrow[0].args[1], static_cast<uint64_t>(KOS_CAP_SIGNAL | KOS_CAP_TRANSFER | KOS_CAP_HANDOUT));
        EXPECT_TRUE(fake::handout(served_object("sensor", 0)));

        // Every record written, alive with its restarts.
        StatusFields const sensor = status("sensor");
        EXPECT_EQ(sensor.deaths, 0u);
        EXPECT_EQ(sensor.restarts_left, 3u);
        EXPECT_EQ(sensor.state, kickos::init::STATUS_ALIVE);
        EXPECT_EQ(status("app").state, kickos::init::STATUS_ALIVE);
    }

    TEST_F(Walk, a_spawn_past_what_the_kernel_build_takes_is_refused_at_boot)
    {
        use("wide");
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::PANIC);
        EXPECT_EQ(outcome.message, system_->refusal);
        EXPECT_TRUE(fake::calls_of("kos_task_create").empty());
    }

    TEST_F(Walk, the_init_lowers_itself_to_2_before_any_other_call_where_init_states_none)
    {
        use("golden_arm64");
        expect_idle(run());
        ASSERT_FALSE(fake::calls().empty());
        EXPECT_EQ(fake::calls()[0].fn, "kos_thread_set_priority");
        EXPECT_EQ(fake::calls()[0].args, (std::vector<uint64_t>{2u}));
        EXPECT_EQ(fake::calls_of("kos_thread_set_priority").size(), 1u);
    }

    TEST_F(Walk, the_init_lowers_itself_to_the_priority_its_table_states)
    {
        use("init_priority");
        expect_idle(run());
        ASSERT_FALSE(fake::calls().empty());
        EXPECT_EQ(fake::calls()[0].fn, "kos_thread_set_priority");
        EXPECT_EQ(fake::calls()[0].args, (std::vector<uint64_t>{7u}));

        harness::Patched patched(systems::find("golden_arm64"));
        patched.header().init_priority = 9u;
        use(patched);
        expect_idle(run());
        ASSERT_FALSE(fake::calls().empty());
        EXPECT_EQ(fake::calls()[0].args, (std::vector<uint64_t>{9u}));
    }

    TEST_F(Walk, a_refused_lowering_panics_holding_nothing)
    {
        use("golden_arm64");
        fake::refuse("kos_thread_set_priority", -KOS_EPERM);
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::PANIC);
        EXPECT_NE(outcome.message.find("kos_thread_set_priority"), std::string::npos) << outcome.message;
        ASSERT_EQ(fake::calls().size(), 2u);
        EXPECT_EQ(fake::calls()[1].fn, "kos_panic");
    }

    // --- the scan ----------------------------------------------------------------------------

    TEST_F(Walk, tasks_start_in_file_order)
    {
        use("chain");
        fake::script({step::ready_all(), step::ready_all(), step::ready_all()});
        expect_idle(run());
        EXPECT_EQ(spawned(), (std::vector<std::string>{"sensor", "slow", "watcher", "mid", "late", "top"}));
    }

    TEST_F(Walk, a_task_whose_server_is_not_ready_is_skipped_and_started_on_readiness)
    {
        use("chain");
        fake::script({
            []
            {
                EXPECT_EQ(spawned(), (std::vector<std::string>{"sensor", "slow", "watcher"}));
                fake::ready("sensor");
            },
            []
            {
                EXPECT_EQ(spawned(), (std::vector<std::string>{"sensor", "slow", "watcher", "mid"}));
                fake::ready("slow");
            },
            []
            {
                EXPECT_EQ(spawned(), (std::vector<std::string>{"sensor", "slow", "watcher", "mid", "late"}));
                fake::ready("mid");
            },
        });
        expect_idle(run());
        EXPECT_EQ(spawned(), (std::vector<std::string>{"sensor", "slow", "watcher", "mid", "late", "top"}));
    }

    // --- the wait ----------------------------------------------------------------------------

    TEST_F(Walk, a_death_and_a_readiness_in_one_wake_both_take_effect)
    {
        use("chain");
        fake::script({
            step::ready("sensor"),
            []
            {
                // The sensor's death and the slow server's readiness in one wake: late, using both,
                // starts once the sensor's next instance is ready.
                fake::die("sensor");
                fake::ready("slow");
            },
            []
            {
                EXPECT_EQ(starts("sensor"), 2u);
                EXPECT_EQ(starts("late"), 0u);
                fake::ready("sensor");
            },
        });
        expect_idle(run());
        EXPECT_EQ(starts("late"), 1u);
        EXPECT_EQ(starts("mid"), 1u);
    }

    TEST_F(Walk, a_ready_report_of_an_ended_instance_is_ignored)
    {
        use("chain");
        fake::script({
            []
            {
                fake::ready("sensor");
                fake::end("sensor", 0);
            },
            []
            {
                EXPECT_EQ(starts("mid"), 0u);
                fake::die("sensor");
            },
            []
            {
                // Its clients wait for the instance it runs now, the endpoint kept for them.
                EXPECT_EQ(starts("sensor"), 2u);
                EXPECT_EQ(starts("mid"), 0u);
                EXPECT_TRUE(fake::handout(served_object("sensor", 0)));
                fake::ready("sensor");
            },
        });
        expect_idle(run());
        EXPECT_EQ(starts("mid"), 1u);
    }

    TEST_F(Walk, a_restarted_server_is_ready_only_once_its_new_instance_is)
    {
        use("chain");
        fake::script({
            step::ready("sensor"),
            step::die("sensor"),
            step::ready("slow"),
            []
            {
                EXPECT_EQ(starts("late"), 0u) << "the readiness was the dead instance's";
                fake::ready("sensor");
            },
        });
        expect_idle(run());
        EXPECT_EQ(starts("late"), 1u);
    }

    TEST_F(Walk, a_server_that_has_ended_serves_no_new_client)
    {
        use("chain");
        fake::script({
            step::ready("sensor"),
            []
            {
                // Ready before, ended now and not yet dead: late, which also waited on slow, waits on.
                fake::end("sensor", 0);
                fake::ready("slow");
            },
            []
            {
                EXPECT_EQ(starts("late"), 0u);
                fake::die("sensor");
            },
            step::ready("sensor"),
        });
        expect_idle(run());
        EXPECT_EQ(starts("late"), 1u);
        EXPECT_EQ(fake::spawned("late").at(0)->params.task, fake::tasks().back().handle);
    }

    TEST_F(Walk, a_task_without_a_live_handle_is_skipped_by_the_wait)
    {
        use("chain");
        fake::script({
            step::die("sensor"),
            step::die("sensor"),
            step::die("sensor"),
            // Bit 0 again, with sensor dead for good and no handle held.
            step::raise(0),
        });
        expect_idle(run());
        std::vector<fake::Call> const states = fake::calls_of("kos_task_state");
        EXPECT_EQ(states.size(), 3u);
        for (fake::Call const& c : states)
        {
            EXPECT_NE(c.args[0], static_cast<uint64_t>(KOS_TASK_NONE));
        }
    }

    // --- a death -----------------------------------------------------------------------------

    TEST_F(Walk, a_dead_task_restarts_with_the_same_grants_until_its_count_is_spent)
    {
        use("golden_arm64");
        Steps steps = {step::ready("sensor")};
        for (int death = 0; death < 4; death++)
        {
            steps.push_back(step::die("sensor"));
            steps.push_back(step::ready_all());
        }
        fake::script(steps);
        expect_idle(run());

        std::vector<fake::Thread const*> const sensor = fake::spawned("sensor");
        ASSERT_EQ(sensor.size(), 4u) << "the first start and three restarts";
        for (fake::Thread const* t : sensor)
        {
            ASSERT_EQ(t->windows.size(), sensor[0]->windows.size());
            for (size_t w = 0; w < t->windows.size(); w++)
            {
                EXPECT_EQ(t->windows[w].base, sensor[0]->windows[w].base);
                EXPECT_EQ(t->windows[w].size, sensor[0]->windows[w].size);
            }
            ASSERT_EQ(t->caps.size(), sensor[0]->caps.size());
            for (size_t c = 0; c < t->caps.size(); c++)
            {
                EXPECT_EQ(t->caps[c].object, sensor[0]->caps[c].object);
                EXPECT_EQ(t->caps[c].rights, sensor[0]->caps[c].rights);
            }
            EXPECT_EQ(t->params.stack_base, sensor[0]->params.stack_base);
            EXPECT_EQ(t->params.stack_size, sensor[0]->params.stack_size);
        }
        // Each instance a new task, the dead one released first.
        EXPECT_EQ(fake::calls_of("kos_task_kill").size(), 4u);
        StatusFields const s = status("sensor");
        EXPECT_EQ(s.deaths, 4u);
        EXPECT_EQ(s.restarts_left, 0u);
        EXPECT_EQ(s.state, 0u);
    }

    TEST_F(Walk, watchers_are_told_after_the_counter_with_the_record_written)
    {
        use("golden_arm64");
        uint16_t const sensor = index("sensor");
        std::vector<StatusFields> told;
        fake::on_call = [&told, sensor](fake::Call const& c)
        {
            if (c.fn == "kos_notify")
            {
                told.push_back(harness::status(sensor));
            }
        };
        fake::script({step::ready_all(), step::die("sensor")});
        expect_idle(run());

        ASSERT_EQ(told.size(), 1u);
        EXPECT_EQ(told[0].deaths, 1u);
        EXPECT_EQ(told[0].restarts_left, 2u);
        EXPECT_EQ(told[0].state, kickos::init::STATUS_ALIVE);
        // The raise is bit 0 of health's notification, the one /init/events delegates it.
        size_t const events = fake::spawned("health").at(0)->caps.at(0).object;
        EXPECT_EQ(fake::objects()[events].pending, 1u << 0);
    }

    TEST_F(Walk, a_task_dead_for_good_drops_the_last_handout)
    {
        use("golden_arm64");
        Steps steps = {step::ready_all()};
        for (int death = 0; death < 4; death++)
        {
            steps.push_back(step::die("sensor"));
            steps.push_back(step::ready_all());
        }
        fake::script(steps);
        expect_idle(run());
        size_t const endpoint = served_object("sensor", 0);
        EXPECT_FALSE(fake::handout(endpoint));
        EXPECT_EQ(fake::objects()[endpoint].refs, 1u) << "app's SIGNAL copy alone";
    }

    TEST_F(Walk, dependency_down_reaches_every_pending_dependant_in_turn_and_their_watchers)
    {
        use("chain");
        uint32_t told = 0;
        fake::script({step::die("sensor"), step::die("sensor"), step::die("sensor"),
                      [&told]
                      {
                          size_t const events = fake::spawned("watcher").at(0)->caps.at(0).object;
                          told = fake::objects()[events].pending;
                      }});
        expect_idle(run());

        // watcher watches sensor, mid, top and late, bits 0 to 3.
        EXPECT_EQ(told, 0xFu);
        for (char const* name : {"mid", "top", "late"})
        {
            StatusFields const s = status(name);
            EXPECT_EQ(s.state, kickos::init::STATUS_DEPENDENCY_DOWN) << name;
            EXPECT_EQ(starts(name), 0u) << name;
        }
        EXPECT_EQ(status("sensor").state, 0u);
        EXPECT_EQ(status("slow").state, kickos::init::STATUS_ALIVE);
        // mid's endpoint goes with it, so top's callers would be refused.
        size_t mid_endpoints = 0;
        for (fake::Cap const& c : fake::table())
        {
            if (fake::objects()[c.object].kind == fake::Kind::ENDPOINT)
            {
                mid_endpoints++;
            }
        }
        EXPECT_EQ(mid_endpoints, 1u) << "slow's alone";
    }

    TEST_F(Walk, a_running_client_of_a_dead_server_goes_down_at_its_restart)
    {
        use("chain");
        fake::script({step::ready("sensor"),
                      step::die("sensor"),
                      step::die("sensor"),
                      step::die("sensor"),
                      [this]
                      {
                          // mid runs on, and top, waiting on mid rather than on the sensor, with it.
                          EXPECT_EQ(status("mid").state, kickos::init::STATUS_ALIVE);
                          EXPECT_EQ(status("top").state, kickos::init::STATUS_ALIVE);
                          EXPECT_EQ(status("late").state, kickos::init::STATUS_DEPENDENCY_DOWN);
                          fake::die("mid");
                      }});
        expect_idle(run());
        StatusFields const mid = status("mid");
        EXPECT_EQ(mid.state, kickos::init::STATUS_DEPENDENCY_DOWN);
        EXPECT_EQ(mid.deaths, 1u);
        EXPECT_EQ(mid.restarts_left, 0u);
        EXPECT_EQ(starts("mid"), 1u);
        EXPECT_EQ(status("top").state, kickos::init::STATUS_DEPENDENCY_DOWN);
    }

    // --- an end before its death -------------------------------------------------------------

    // The kernel has stopped every member at the end, so the init waits on the DEAD bit alone.
    TEST_F(Walk, an_ended_task_is_released_at_its_dead_bit_and_never_slain)
    {
        use("golden_arm64");
        fake::script({step::ready_all(),
                      step::end("sensor", 0),
                      []
                      {
                          EXPECT_EQ(starts("sensor"), 1u) << "held until its DEAD bit";
                          EXPECT_TRUE(fake::calls_of("kos_task_kill").empty());
                          EXPECT_EQ(fake::console(), "");
                          fake::advance(10000000000ull);
                          fake::die("sensor");
                      }});
        expect_idle(run());
        EXPECT_TRUE(fake::calls_of("kos_task_slay").empty());
        EXPECT_EQ(fake::calls_of("kos_task_kill").size(), 1u);
        EXPECT_EQ(starts("sensor"), 2u);
        EXPECT_EQ(status("sensor").deaths, 1u);
        for (fake::Call const& wait : fake::calls_of("kos_notify_wait"))
        {
            EXPECT_EQ(wait.args[2], KOS_TIMEOUT_NONE) << "the wait is never bounded";
        }
        EXPECT_EQ(fake::console(), "init: `sensor` is dead and released\n");
    }

    // --- the ending --------------------------------------------------------------------------

    TEST_F(Walk, the_ends_task_ends_the_system_at_its_ended_bit_with_its_status)
    {
        for (int code : {3, -3, 0})
        {
            use("default_qemu_arm64");
            fake::script({step::end("main", code)});
            Outcome const outcome = run();
            ASSERT_EQ(outcome.kind, Outcome::Kind::SHUTDOWN) << outcome.message;
            EXPECT_EQ(outcome.status, code);
            // The init waits for no DEAD bit and kills nothing.
            EXPECT_TRUE(fake::calls_of("kos_task_kill").empty());
            EXPECT_TRUE(fake::calls_of("kos_task_slay").empty());
            EXPECT_NE(fake::instance("main")->state & KOS_TASK_LIVE, 0u);
        }
    }

    TEST_F(Walk, an_ends_task_dependency_down_ends_the_system_cancelled)
    {
        use("chain_ends");
        fake::script({step::die("sensor"), step::die("sensor"), step::die("sensor")});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::SHUTDOWN) << outcome.message;
        EXPECT_EQ(outcome.status, KOS_EXIT_CANCELLED);
        EXPECT_EQ(starts("top"), 0u);
    }

    TEST_F(Walk, an_ends_task_whose_start_fails_ends_the_system_cancelled)
    {
        use("default_xmc4800_relax");
        fake::refuse("kos_thread_create", -KOS_ENOMEM);
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::SHUTDOWN) << outcome.message;
        EXPECT_EQ(outcome.status, KOS_EXIT_CANCELLED);
    }

    // --- failures ----------------------------------------------------------------------------

    struct FailedStep
    {
        char const* fn;
        int result;
        // The calls between kos_task_create and the slay, in order.
        std::vector<std::string> unwind;
    };

    class FailedStart : public Walk, public ::testing::WithParamInterface<FailedStep>
    {
    };

    TEST_P(FailedStart, unwinds_its_step_then_is_slain_and_counted_as_a_death)
    {
        FailedStep const& step = GetParam();
        use("alarm");
        fake::refuse(step.fn, step.result);
        expect_idle(run());

        std::vector<std::string> const relevant = {
            "kos_task_create", "kos_task_sched_grant", "kos_task_watch", "kos_thread_set_affinity", "kos_irq_claim",
            "kos_thread_create", "kos_handle_close", "kos_task_slay"};
        std::vector<std::string> const seen = fake::sequence(relevant);
        // The sensor is the first task the walk creates.
        std::vector<std::string> expected = {"kos_task_create"};
        expected.insert(expected.end(), step.unwind.begin(), step.unwind.end());
        expected.push_back("kos_task_slay");
        ASSERT_GE(seen.size(), expected.size());
        EXPECT_EQ(std::vector<std::string>(seen.begin(), seen.begin() + static_cast<ptrdiff_t>(expected.size())),
                  expected);
        std::vector<fake::Call> const slays = fake::calls_of("kos_task_slay");
        ASSERT_FALSE(slays.empty());
        EXPECT_EQ(slays[0].args[0], fake::tasks()[0].handle);
        EXPECT_EQ(slays[0].args[1], kickos::init::SLAY_TIMEOUT_US);
        EXPECT_TRUE(fake::tasks()[0].released);

        // The restart that follows starts it.
        StatusFields const s = status("sensor");
        EXPECT_EQ(s.deaths, 1u);
        EXPECT_EQ(s.restarts_left, 2u);
        EXPECT_EQ(starts("sensor"), 1u);
        EXPECT_NE(fake::instance("sensor")->handle, fake::tasks()[0].handle) << "a new task";

        // Reported once slain, naming its step and the answer.
        std::string const line = std::string{"init: `sensor` failed to start: "} + step.fn + " answered "
                                 + std::to_string(step.result) + "\n";
        EXPECT_NE(fake::console().find(line), std::string::npos) << fake::console();
    }

    INSTANTIATE_TEST_SUITE_P(
        Step, FailedStart,
        ::testing::Values(
            FailedStep{"kos_task_sched_grant", -KOS_EPERM, {"kos_task_sched_grant", "kos_handle_close"}},
            FailedStep{"kos_task_watch", -KOS_EACCES, {"kos_task_sched_grant", "kos_task_watch", "kos_handle_close"}},
            FailedStep{"kos_thread_set_affinity", -KOS_EPERM,
                       {"kos_task_sched_grant", "kos_task_watch", "kos_thread_set_affinity", "kos_handle_close"}},
            FailedStep{"kos_irq_claim", -KOS_EBUSY,
                       {"kos_task_sched_grant", "kos_task_watch", "kos_thread_set_affinity", "kos_irq_claim",
                        "kos_thread_set_affinity", "kos_handle_close"}},
            FailedStep{"kos_thread_create", -KOS_ENOMEM,
                       {"kos_task_sched_grant", "kos_task_watch", "kos_thread_set_affinity", "kos_irq_claim",
                        "kos_thread_create", "kos_handle_close", "kos_thread_set_affinity", "kos_handle_close"}}),
        [](::testing::TestParamInfo<FailedStep> const& p)
        {
            return std::string{p.param.fn};
        });

    TEST_F(Walk, a_task_failing_every_start_is_restarted_while_its_count_lasts_then_dead_for_good)
    {
        use("golden_arm64");
        // The sensor's watches, the only ones with a ready endpoint.
        fake::refuse_if("kos_task_watch", -KOS_EACCES, 4,
                        [](fake::Call const& c)
                        {
                            return c.args[2] != KOS_CAP_NONE;
                        });
        expect_idle(run());
        StatusFields const s = status("sensor");
        EXPECT_EQ(s.deaths, 4u);
        EXPECT_EQ(s.restarts_left, 0u);
        EXPECT_EQ(s.state, 0u);
        EXPECT_EQ(starts("sensor"), 0u);
        EXPECT_EQ(fake::calls_of("kos_notify").size(), 4u) << "health told each time";
        // app uses it, so it goes down; health runs.
        EXPECT_EQ(status("app").state, kickos::init::STATUS_DEPENDENCY_DOWN);
        EXPECT_EQ(starts("health"), 1u);
    }

    TEST_F(Walk, a_watcher_that_has_not_taken_its_bit_yet_is_no_refusal)
    {
        use("golden_arm64");
        fake::refuse("kos_notify", -KOS_EALREADY);
        fake::script({step::ready_all(), step::die("sensor")});
        expect_idle(run());
        EXPECT_EQ(starts("sensor"), 2u);
    }

    TEST_F(Walk, a_claim_retiring_from_its_last_holder_is_retried_within_the_bound)
    {
        use("alarm");
        fake::refuse("kos_irq_claim", -KOS_EAGAIN, kickos::init::CLAIM_RETRIES);
        expect_idle(run());
        EXPECT_EQ(fake::calls_of("kos_irq_claim").size(), kickos::init::CLAIM_RETRIES + 1u);
        std::vector<fake::Call> const sleeps = fake::calls_of("kos_sleep_ns");
        ASSERT_EQ(sleeps.size(), kickos::init::CLAIM_RETRIES);
        EXPECT_EQ(sleeps[0].args[0], kickos::init::CLAIM_RETRY_NS);
        EXPECT_TRUE(fake::calls_of("kos_task_slay").empty());
        EXPECT_EQ(starts("sensor"), 1u);
        EXPECT_EQ(status("sensor").deaths, 0u);
    }

    TEST_F(Walk, a_claim_still_retiring_past_the_bound_is_a_failed_start)
    {
        use("alarm");
        fake::refuse("kos_irq_claim", -KOS_EAGAIN, kickos::init::CLAIM_RETRIES + 1u);
        expect_idle(run());
        EXPECT_EQ(fake::calls_of("kos_sleep_ns").size(), kickos::init::CLAIM_RETRIES);
        EXPECT_EQ(fake::calls_of("kos_task_slay").size(), 1u);
        EXPECT_EQ(status("sensor").deaths, 1u);
        EXPECT_EQ(starts("sensor"), 1u) << "its restart claims at once";
    }

    struct Refusal
    {
        char const* fn;
        int result;
        uint32_t skip;
        char const* message;
    };

    class Refused : public Walk, public ::testing::WithParamInterface<Refusal>
    {
    };

    TEST_P(Refused, any_other_refusal_panics_naming_the_step_and_the_task)
    {
        Refusal const& r = GetParam();
        use("golden_arm64");
        fake::refuse(r.fn, r.result, 1, r.skip);
        fake::script({step::ready_all(), step::die("sensor")});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::PANIC);
        EXPECT_EQ(outcome.message.rfind(r.message, 0), 0u) << outcome.message;
    }

    INSTANTIATE_TEST_SUITE_P(
        Step, Refused,
        ::testing::Values(
            Refusal{"kos_notify_create", -KOS_ENOMEM, 0, "init: kos_notify_create answered -12"},
            Refusal{"kos_ram_alloc", 0, 4, "init: kos_ram_alloc of the stack for `app` answered null"},
            Refusal{"kos_endpoint_create", -KOS_ENOMEM, 0, "init: kos_endpoint_create for `sensor` answered -12"},
            Refusal{"kos_notify_create", -KOS_EAGAIN, 1, "init: kos_notify_create for `health` answered -11"},
            Refusal{"kos_notify_badge", -KOS_EMFILE, 0, "init: kos_notify_badge for `sensor` answered -24"},
            Refusal{"kos_task_create", -KOS_ENOMEM, 1, "init: kos_task_create for `health` answered -12"},
            Refusal{"kos_handle_close", -KOS_EBADF, 0, "init: kos_handle_close of the badged copy for `sensor`"},
            Refusal{"kos_notify_wait", -KOS_EINVAL, 0, "init: kos_notify_wait answered -22"},
            Refusal{"kos_task_state", -KOS_EBADF, 0, "init: kos_task_state for `sensor` answered -9"},
            Refusal{"kos_task_kill", -KOS_EPERM, 0, "init: kos_task_kill for `sensor` answered -1"},
            Refusal{"kos_notify", -KOS_EBADF, 0, "init: kos_notify for `health` answered -9"}),
        [](::testing::TestParamInfo<Refusal> const& p)
        {
            return std::string{p.param.fn} + "_" + std::to_string(p.index);
        });

    // --- the spawn ---------------------------------------------------------------------------

    // Each window-kind grant of `task` at its place, and each capability at its slot.
    void expect_spawned_as_the_table_says(systems::System const& system, char const* task)
    {
        TableView const v = view(system);
        uint16_t const i = harness::index_of(system, task);
        kos_table_task const& t = v.tasks[i];
        fake::Thread const* const thread = fake::spawned(task).at(0);
        EXPECT_EQ(thread->params.entry, &kickos::init::task_trampoline);
        EXPECT_EQ(thread->params.arg, &t);
        EXPECT_EQ(thread->params.prio, t.priority);
        EXPECT_EQ(thread->params.core_mask, t.core_mask);
        EXPECT_EQ(thread->params.authority, t.authority);
        EXPECT_EQ(thread->params.task, fake::instance(task)->handle);
        EXPECT_EQ(thread->caps.size(), t.cap_grant_count);
        uint16_t windows = 0;
        for (uint16_t k = 0; k < t.grant_count; k++)
        {
            kos_table_grant const& g = v.grants[t.first_grant + k];
            if (g.window != KOS_TABLE_NONE)
            {
                windows++;
                ASSERT_LT(g.window, thread->windows.size());
                kos_window const& w = thread->windows[g.window];
                EXPECT_EQ(w.flags, g.flags) << task << " window " << g.window;
                if (g.kind == KOS_GRANT_STATUS)
                {
                    EXPECT_EQ(w.kind, KOS_WINDOW_MEMORY);
                    EXPECT_EQ(w.base, reinterpret_cast<uintptr_t>(harness::record(harness::index_of(system, task)).status));
                    EXPECT_EQ(w.size, g.size);
                }
                else if (g.kind == KOS_GRANT_REGION)
                {
                    EXPECT_EQ(w.kind, KOS_WINDOW_MEMORY);
                    auto const kept = static_cast<kickos::init::PrivateRecord const*>(fake::reservations()[0].base);
                    EXPECT_EQ(w.base, reinterpret_cast<uintptr_t>(kept[v.header->task_count + g.target].region));
                    EXPECT_EQ(w.size, v.regions[g.target].size);
                }
                else if (g.kind == KOS_GRANT_PORTS)
                {
                    EXPECT_EQ(w.kind, KOS_WINDOW_PORTS);
                    EXPECT_EQ(w.base, g.base);
                    EXPECT_EQ(w.size, g.size);
                }
                else
                {
                    EXPECT_EQ(w.kind, KOS_WINDOW_DEVICE);
                    EXPECT_EQ(w.base, g.base);
                    EXPECT_EQ(w.size, g.size);
                }
            }
            if (g.cap_slot == KOS_TABLE_NONE)
            {
                continue;
            }
            fake::Cap const& c = thread->caps.at(g.cap_slot - CAP0);
            EXPECT_EQ(c.handle, g.cap_slot);
            fake::Kind const kind = fake::objects()[c.object].kind;
            if (g.kind == KOS_GRANT_ENDPOINT_SERVE)
            {
                EXPECT_EQ(kind, fake::Kind::ENDPOINT);
                EXPECT_EQ(c.rights, KOS_CAP_WAIT);
            }
            else if (g.kind == KOS_GRANT_ENDPOINT_USE)
            {
                EXPECT_EQ(kind, fake::Kind::ENDPOINT);
                EXPECT_EQ(c.rights, KOS_CAP_SIGNAL);
                // The server's own endpoint, the one its watch reports readiness on.
                uint16_t const server = g.target;
                char const* const server_name = reinterpret_cast<char const*>(
                    v.regions + v.header->region_count) + v.tasks[server].name;
                EXPECT_EQ(static_cast<int64_t>(c.object), fake::instance(server_name)->ready_object);
            }
            else if (g.kind == KOS_GRANT_NOTIFICATION)
            {
                EXPECT_EQ(kind, fake::Kind::NOTIFY);
                EXPECT_EQ(c.rights, KOS_CAP_WAIT);
                EXPECT_NE(c.object, fake::init_notification());
            }
            else
            {
                EXPECT_EQ(kind, fake::Kind::LINE);
                EXPECT_EQ(fake::objects()[c.object].line, static_cast<int>(g.line));
                EXPECT_EQ(c.rights, KOS_CAP_WAIT);
            }
        }
        EXPECT_EQ(thread->windows.size(), windows);
    }

    TEST_F(Walk, a_spawn_takes_its_windows_in_table_order_and_each_capability_at_its_slot)
    {
        for (char const* name : {"golden_arm64", "alarm", "golden_x86"})
        {
            SCOPED_TRACE(name);
            use(name);
            fake::script({step::ready_all()});
            expect_idle(run());
            for (char const* task : {"sensor", "app", "health"})
            {
                SCOPED_TRACE(task);
                expect_spawned_as_the_table_says(*system_, task);
            }
        }
        // health: the status block at its place, then the history read-only.
        use("golden_arm64");
        fake::script({step::ready_all()});
        expect_idle(run());
        fake::Thread const* const health = fake::spawned("health").at(0);
        ASSERT_EQ(health->windows.size(), 2u);
        EXPECT_EQ(health->windows[0].base, reinterpret_cast<uintptr_t>(fake::reservations()[1].base));
        EXPECT_EQ(health->windows[0].flags, KOS_WINDOW_RO);
        EXPECT_EQ(health->windows[1].base, reinterpret_cast<uintptr_t>(fake::reservations()[2].base));
    }

    TEST_F(Walk, where_sp_is_masked_no_stack_is_reserved_or_handed_over)
    {
        use("default_xmc4800_relax");
        expect_idle(run());
        EXPECT_EQ(fake::reservations().size(), 1u);
        std::vector<fake::Call> const create = fake::calls_of("kos_task_create");
        ASSERT_EQ(create.size(), 1u);
        EXPECT_EQ(create[0].args[0], 0u);
        EXPECT_EQ(create[0].args[1], 0u);
        fake::Thread const* const main = fake::spawned("main").at(0);
        EXPECT_EQ(main->params.stack_base, nullptr);
        EXPECT_EQ(main->params.stack_size, 0u);
    }

    TEST_F(Walk, a_region_board_hands_the_reserved_stack_over_at_its_declared_size)
    {
        use("default_esp32c6_wroom");
        expect_idle(run());
        ASSERT_EQ(fake::reservations().size(), 2u);
        fake::Block const& stack = fake::reservations()[1];
        EXPECT_EQ(stack.size, 8192u);
        std::vector<fake::Call> const create = fake::calls_of("kos_task_create");
        ASSERT_EQ(create.size(), 1u);
        EXPECT_EQ(create[0].args[0], 0u) << "no memory on a region board";
        fake::Thread const* const main = fake::spawned("main").at(0);
        EXPECT_EQ(main->params.stack_base, stack.base);
        EXPECT_EQ(main->params.stack_size, 8192u);
    }

    TEST_F(Walk, a_translating_board_runs_the_task_on_its_stack_block_as_its_data)
    {
        use("default_qemu_arm64");
        expect_idle(run());
        ASSERT_EQ(fake::reservations().size(), 2u);
        fake::Block const& stack = fake::reservations()[1];
        EXPECT_EQ(stack.size, 20480u);
        std::vector<fake::Call> const create = fake::calls_of("kos_task_create");
        ASSERT_EQ(create.size(), 1u);
        EXPECT_EQ(create[0].args[0], reinterpret_cast<uintptr_t>(stack.base));
        EXPECT_EQ(create[0].args[1], 20480u);
        fake::Thread const* const main = fake::spawned("main").at(0);
        EXPECT_EQ(main->params.stack_base, stack.base);
        EXPECT_EQ(main->params.stack_size, 20480u);
    }

    TEST_F(Walk, a_line_taker_is_claimed_pinned_to_its_core_and_unpinned_after_its_spawn)
    {
        use("alarm");
        expect_idle(run());
        std::vector<std::string> const seen = fake::sequence(
            {"kos_task_watch", "kos_thread_self", "kos_thread_set_affinity", "kos_irq_claim", "kos_thread_create",
             "kos_handle_close"});
        std::vector<std::string> const expected = {
            "kos_task_watch", "kos_thread_self", "kos_thread_set_affinity", "kos_irq_claim", "kos_thread_create",
            "kos_handle_close", "kos_thread_self", "kos_thread_set_affinity", "kos_handle_close"};
        ASSERT_GE(seen.size(), expected.size());
        EXPECT_EQ(std::vector<std::string>(seen.begin(), seen.begin() + static_cast<ptrdiff_t>(expected.size())),
                  expected);
        std::vector<fake::Call> const pins = fake::calls_of("kos_thread_set_affinity");
        ASSERT_EQ(pins.size(), 2u);
        EXPECT_EQ(pins[0].args[1], 1u << 1) << "the sensor's core";
        EXPECT_EQ(pins[1].args[1], 0u) << "the init's default set again";
        std::vector<fake::Call> const claims = fake::calls_of("kos_irq_claim");
        ASSERT_EQ(claims.size(), 1u);
        EXPECT_EQ(claims[0].args[1], static_cast<uint64_t>(KOS_IRQ_EDGE));
        // The init keeps no line: the thread holds the only capability.
        for (fake::Cap const& c : fake::table())
        {
            EXPECT_NE(fake::objects()[c.object].kind, fake::Kind::LINE);
        }
    }

    // --- diagnostics -------------------------------------------------------------------------

    TEST_F(Walk, a_death_is_reported_once_released)
    {
        use("golden_arm64");
        fake::script({step::ready_all(), step::die("sensor")});
        expect_idle(run());
        EXPECT_EQ(fake::console(), "init: `sensor` is dead and released\n");
    }

    TEST_F(Walk, a_failed_start_whose_slay_does_not_empty_waits_for_its_death)
    {
        use("alarm");
        fake::refuse("kos_task_slay", -KOS_ETIMEDOUT);
        fake::refuse("kos_thread_create", -KOS_ENOMEM);
        fake::script({
            [this]
            {
                // Held, not yet counted, and not started again.
                EXPECT_EQ(status("sensor").deaths, 0u);
                EXPECT_TRUE(fake::spawned("sensor").empty());
                EXPECT_EQ(fake::calls_of("kos_task_create").size(), 2u) << "the sensor once, and health";
                fake::die_task(fake::tasks()[0].handle);
            },
        });
        expect_idle(run());
        EXPECT_EQ(status("sensor").deaths, 1u);
        EXPECT_EQ(starts("sensor"), 1u);
        EXPECT_EQ(fake::calls_of("kos_task_kill").size(), 1u);
        EXPECT_NE(fake::console().find("init: `sensor` keeps members past its slay\n"), std::string::npos);
    }

    // --- the generation ----------------------------------------------------------------------

    TEST_F(Walk, a_restart_under_a_reused_handle_is_not_ready_until_its_own_report)
    {
        use("chain");
        fake::Config c = harness::config(*system_);
        c.reuse_handles = true;
        fake::reset(c);
        fake::script({
            step::ready("sensor"),
            step::die("sensor"),
            []
            {
                ASSERT_EQ(fake::instance("sensor")->handle, fake::tasks()[0].handle) << "the handle came back";
                fake::ready("slow");
            },
            []
            {
                EXPECT_EQ(starts("late"), 0u) << "the readiness was the dead instance's";
                fake::ready("sensor");
            },
        });
        expect_idle(run());
        EXPECT_EQ(starts("late"), 1u);
    }

    TEST_F(Walk, each_restart_releases_the_dead_instance_before_it_creates_the_next)
    {
        use("golden_arm64");
        Steps steps = {step::ready_all()};
        for (int death = 0; death < 3; death++)
        {
            steps.push_back(step::die("sensor"));
            steps.push_back(step::ready_all());
        }
        fake::script(steps);
        expect_idle(run());
        std::vector<std::string> const seen = fake::sequence({"kos_task_kill", "kos_task_create"});
        std::vector<std::string> const expected = {
            "kos_task_create", "kos_task_create", "kos_task_create", "kos_task_kill", "kos_task_create",
            "kos_task_kill", "kos_task_create", "kos_task_kill", "kos_task_create"};
        EXPECT_EQ(seen, expected);
        std::vector<fake::Call> const kills = fake::calls_of("kos_task_kill");
        ASSERT_EQ(kills.size(), 3u);
        EXPECT_EQ(kills[0].args[0], fake::tasks()[0].handle);
    }

    // --- a table the init refuses at boot ----------------------------------------------------

    TEST_F(Walk, a_window_placed_past_the_task_s_windows_is_refused_at_boot)
    {
        harness::Patched patched{systems::find("golden_arm64")};
        patched.grant("health", 1).window = 3;
        use(patched);
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::PANIC);
        EXPECT_EQ(outcome.message, "init: `health` places a window at 3, past its 2");
        EXPECT_TRUE(fake::calls_of("kos_notify_create").empty());
    }

    TEST_F(Walk, a_capability_slot_past_the_task_s_grants_is_refused_at_boot)
    {
        harness::Patched patched{systems::find("golden_arm64")};
        patched.grant("sensor", 0).cap_slot = CAP0 + 1;
        use(patched);
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::PANIC);
        EXPECT_EQ(outcome.message, "init: `sensor` delegates a capability at slot 2, past its 1");
        EXPECT_TRUE(fake::calls_of("kos_notify_create").empty());
    }

    TEST_F(Walk, a_line_taker_with_no_core_above_one_kernel_core_is_refused_at_boot)
    {
        harness::Patched patched{systems::find("alarm")};
        patched.task("sensor").core_mask = 0u;
        use(patched);
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::PANIC);
        EXPECT_EQ(outcome.message,
                  "init: `sensor` takes a line and declares no core, on a build of more than one kernel core");
        EXPECT_TRUE(fake::calls_of("kos_notify_create").empty());
    }

    TEST_F(Walk, a_port_range_bound_to_a_packaged_driver_s_window_role_is_refused_at_boot)
    {
        harness::Patched patched{systems::find("golden_xmc")};
        patched.grant("spi0", 0).kind = KOS_GRANT_PORTS;
        use(patched);
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::PANIC);
        EXPECT_EQ(outcome.message, "init: `spi0` binds a port range to a window role of a packaged driver, which "
                                   "takes it as a device window");
        EXPECT_TRUE(fake::calls_of("kos_notify_create").empty());
    }

    TEST_F(Walk, a_refusal_names_its_answer_as_a_number)
    {
        use("golden_arm64");
        fake::refuse("kos_endpoint_create", -KOS_ENOMEM);
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::PANIC);
        EXPECT_EQ(outcome.message, "init: kos_endpoint_create for `sensor` answered -12");
    }

    TEST_F(Walk, a_task_s_ceiling_is_its_grant_and_its_entry_runs_at_its_priority)
    {
        harness::Patched patched{systems::find("golden_arm64")};
        patched.task("app").ceiling = 20u;
        use(patched);
        fake::script({step::ready_all()});
        expect_idle(run());
        fake::Thread const* const app = fake::spawned("app").at(0);
        EXPECT_EQ(app->params.prio, 8u);
        bool granted = false;
        for (fake::Call const& c : fake::calls_of("kos_task_sched_grant"))
        {
            if (c.args[0] == app->task)
            {
                EXPECT_EQ(c.args[1], 20u);
                granted = true;
            }
        }
        EXPECT_TRUE(granted);

        // The board default: main at 2 under the build's top priority.
        use("default_qemu_arm64");
        expect_idle(run());
        fake::Thread const* const main = fake::spawned("main").at(0);
        EXPECT_EQ(main->params.prio, 2u);
        ASSERT_EQ(fake::calls_of("kos_task_sched_grant").size(), 1u);
        EXPECT_EQ(fake::calls_of("kos_task_sched_grant")[0].args[1], 31u);
    }

    TEST_F(Walk, a_ceiling_below_the_task_s_priority_is_refused_at_boot)
    {
        harness::Patched patched{systems::find("golden_arm64")};
        patched.task("app").ceiling = 7u;
        use(patched);
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::PANIC);
        EXPECT_EQ(outcome.message, "init: `app` has a ceiling 7 below its priority 8");
        EXPECT_TRUE(fake::calls_of("kos_notify_create").empty());
    }

    TEST_F(Walk, a_user_task_carrying_a_ring_block_is_refused_at_boot)
    {
        harness::Patched patched{systems::find("golden_arm64")};
        patched.task("sensor").block = 1024u;
        use(patched);
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::PANIC);
        EXPECT_EQ(outcome.message, "init: `sensor` carries a ring block and runs no packaged driver");
        EXPECT_TRUE(fake::calls_of("kos_notify_create").empty());
    }

    TEST_F(Walk, each_watcher_reads_its_watched_tasks_alone_in_watches_order)
    {
        use("chain");
        fake::script({step::ready_all(), step::ready_all(), step::die("mid"), step::ready_all()});
        expect_idle(run());
        kickos::init::TaskRecord const& watcher = harness::record(index("watcher"));
        ASSERT_NE(watcher.status, nullptr);
        fake::Block const* block = nullptr;
        for (fake::Block const& b : fake::reservations())
        {
            if (b.base == watcher.status)
            {
                block = &b;
            }
        }
        ASSERT_NE(block, nullptr);
        EXPECT_EQ(block->size, 4u * KICKOS_INIT_STATUS_RECORD_SIZE) << "a record per task it watches, no more";
        std::vector<std::string> const order = {"sensor", "mid", "top", "late"};
        for (uint16_t k = 0; k < order.size(); k++)
        {
            SCOPED_TRACE(order[k]);
            StatusFields read{};
            ASSERT_TRUE(kickos::init::status_read(&watcher.status[k], &read));
            StatusFields const kept = kickos::init::status_of(harness::record(index(order[k].c_str())));
            EXPECT_EQ(read.deaths, kept.deaths);
            EXPECT_EQ(read.restarts_left, kept.restarts_left);
            EXPECT_EQ(read.state, kept.state);
        }
        StatusFields mid{};
        ASSERT_TRUE(kickos::init::status_read(&watcher.status[1], &mid));
        EXPECT_EQ(mid.deaths, 1u);
        fake::Thread const* const spawned = fake::spawned("watcher").at(0);
        ASSERT_FALSE(spawned->windows.empty());
        EXPECT_EQ(spawned->windows[0].base, reinterpret_cast<uintptr_t>(watcher.status));
    }

    // The fake hands each block out dirty, every count odd, so a record boot did not clear
    // stays odd through every write and its watcher reads none.
    TEST_F(Walk, every_status_record_reads_after_boot_over_a_dirty_reservation)
    {
        use("chain");
        expect_idle(run());
        kickos::init::TaskRecord const& watcher = harness::record(index("watcher"));
        ASSERT_NE(watcher.status, nullptr);
        std::vector<std::string> const order = {"sensor", "mid", "top", "late"};
        for (uint16_t k = 0; k < order.size(); k++)
        {
            SCOPED_TRACE(order[k]);
            EXPECT_EQ(static_cast<uint32_t>(watcher.status[k].count), 2u) << "cleared, then written once";
            StatusFields read{};
            ASSERT_TRUE(kickos::init::status_read(&watcher.status[k], &read));
            EXPECT_EQ(read.deaths, 0u);
            EXPECT_EQ(read.state, kickos::init::STATUS_ALIVE);
        }
    }

    TEST_F(Walk, a_death_restarts_its_task_before_a_stalled_console_takes_the_diagnostic)
    {
        use("golden_xmc");
        fake::script({step::ready_all(), step::ready_all(),
                      []
                      {
                          // The console took the handover's probe and stops receiving, its receiver alive.
                          fake::set_console_receives(false);
                          fake::die("sensor");
                      },
                      step::nothing()});
        expect_idle(run());
        std::vector<fake::Call> const& calls = fake::calls();
        size_t restarted = calls.size();
        size_t reported = calls.size();
        size_t spawns = 0;
        for (size_t k = 0; k < calls.size(); k++)
        {
            if (calls[k].fn == "kos_thread_create" and calls[k].text == "sensor")
            {
                spawns++;
                if (spawns == 2u)
                {
                    restarted = k;
                }
            }
            if (calls[k].fn == "kos_send_timed" and calls[k].args[1] != 0u and reported == calls.size())
            {
                reported = k;
            }
        }
        ASSERT_LT(restarted, calls.size()) << "the sensor restarted";
        ASSERT_LT(reported, calls.size()) << "its death reported";
        EXPECT_EQ(calls[reported].result, -KOS_ETIMEDOUT);
        EXPECT_LT(restarted, reported) << "the restart waits on no diagnostic";
    }

    TEST_F(Walk, a_diagnostic_on_a_console_that_stopped_receiving_is_bounded)
    {
        // The console is up and its receiver alive until the sensor dies.
        use("golden_xmc");
        fake::script({step::ready_all(), step::ready_all(),
                      []
                      {
                          fake::set_console_receives(false);
                          fake::die("sensor");
                      },
                      step::nothing()});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;
        size_t lines = 0;
        for (fake::Call const& c : fake::calls_of("kos_send_timed"))
        {
            if (c.args[1] != 0u)
            {
                lines++;
                EXPECT_EQ(c.args[2], kickos::driver::KOS_DRV_HANDOVER_PROBE_US);
                EXPECT_EQ(c.result, -KOS_ETIMEDOUT);
            }
        }
        EXPECT_EQ(lines, 1u) << "the death";
    }
}
