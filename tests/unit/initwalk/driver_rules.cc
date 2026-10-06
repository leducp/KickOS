// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Packaged drivers through their descriptors under the init (docs/design-m10-target.md, section 2),
// the real bring-up over the scripted kernel: `golden_xmc` (console, the xmcuartirq shape, and spi0,
// the xmcssc shape, then sensor using spi0, app using sensor, and health watching spi0 and sensor),
// and `driver_restart` (drv, an entry thread and a receiving worker with one restart, a client of it
// and a watcher of it).

#include "drivers.h"
#include "harness.h"

#include <kickos/sys/driver_service.h>
#include <kickos/sys/errno.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

namespace
{
    using harness::Outcome;

    constexpr uint32_t KEPT = KOS_CAP_SIGNAL | KOS_CAP_TRANSFER | KOS_CAP_HANDOUT;

    class Driver : public ::testing::Test
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

        void use(char const* name, fake::Config const& config)
        {
            system_ = &systems::find(name);
            fake::reset(config);
        }

        Outcome run()
        {
            return harness::run(*system_);
        }

        kickos::init::StatusFields status(char const* task)
        {
            return harness::status(harness::index_of(*system_, task));
        }

        systems::System const* system_ = nullptr;
    };

    // The zero-length sends on stdout: the handover's probe and the ending's drain.
    std::vector<fake::Call> probes()
    {
        std::vector<fake::Call> out;
        for (fake::Call const& c : fake::calls_of("kos_send_timed"))
        {
            if (c.args[1] == 0u)
            {
                out.push_back(c);
            }
        }
        return out;
    }

    // The calls from the `n`-th kos_task_create on, up to the next.
    std::vector<std::string> start_of(size_t n)
    {
        std::vector<std::string> out;
        size_t creates = 0;
        for (fake::Call const& c : fake::calls())
        {
            if (c.fn == "kos_task_create")
            {
                creates++;
            }
            if (creates == n + 1u and c.fn != "kos_clock_now" and c.fn != "kos_thread_self")
            {
                out.push_back(c.fn);
            }
        }
        return out;
    }

    // The init's capability on the endpoint the console's receiver holds.
    fake::Cap const* console_endpoint()
    {
        int64_t const object = fake::stdout_object();
        for (fake::Cap const& c : fake::table())
        {
            if (static_cast<int64_t>(c.object) == object)
            {
                return &c;
            }
        }
        return nullptr;
    }

    TEST_F(Driver, an_uncached_ring_block_is_self_granted_and_given_to_the_task_uncached)
    {
        harness::Patched patched(systems::find("golden_xmc"));
        patched.task("console").flags |= KOS_TABLE_TASK_BLOCK_UNCACHED;
        use(patched);
        drivers::behaviour.uncached_console = true;
        fake::script({step::ready_all(), step::ready_all(), step::ready_all()});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;
        EXPECT_EQ(status("console").deaths, 0u) << "the descriptor's typed block admitted";

        std::vector<fake::Block> const& r = fake::reservations();
        ASSERT_EQ(r.size(), 4u);
        std::vector<fake::Call> const grants = fake::calls_of("kos_mem_self_grant");
        ASSERT_EQ(grants.size(), 3u);
        EXPECT_EQ(grants[2].args[0], reinterpret_cast<uintptr_t>(r[3].base));
        EXPECT_EQ(grants[2].args[2], static_cast<uint64_t>(KOS_MEM_NOCACHE));
        EXPECT_EQ(fake::calls_of("kos_task_create").at(0).args[2], static_cast<uint64_t>(KOS_MEM_NOCACHE));
    }

    TEST_F(Driver, a_ring_block_typed_apart_from_its_descriptor_is_a_failed_start)
    {
        use("golden_xmc");
        drivers::behaviour.uncached_console = true;
        fake::script({step::ready_all(), step::ready_all(), step::ready_all()});
        (void)run();
        EXPECT_GE(status("console").deaths, 1u) << "a cached reservation under an uncached descriptor";
        std::vector<fake::Block> const& r = fake::reservations();
        ASSERT_EQ(r.size(), 4u);
        for (fake::Call const& c : fake::calls_of("kos_task_create"))
        {
            EXPECT_NE(c.args[0], reinterpret_cast<uintptr_t>(r[3].base)) << "no task made over the console's block";
        }
    }

    TEST_F(Driver, a_console_driver_starts_on_the_init_s_block_and_endpoint_in_the_design_s_order)
    {
        use("golden_xmc");
        fake::script({step::ready_all(), step::ready_all(), step::ready_all()});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;

        std::vector<std::string> const expected = {
            "kos_task_create", "kos_task_sched_grant", "kos_task_watch", "kos_console_publish", "kos_irq_claim",
            "kos_notify_create", "kos_notify_badge", "kos_irq_bind_notify", "kos_handle_close", "kos_thread_create",
            "kos_notify_badge", "kos_thread_create", "kos_handle_close", "kos_handle_close", "kos_handle_close",
            "kos_cap_narrow", "kos_send_timed", "kos_handle_close", "kos_notify_badge"};
        EXPECT_EQ(start_of(0), expected);
        EXPECT_EQ(fake::calls_of("kos_endpoint_create").size(), 3u) << "every endpoint the init's, made at boot";

        // The ring block the init reserved and self-granted at boot, after the shared region.
        std::vector<fake::Block> const& r = fake::reservations();
        ASSERT_EQ(r.size(), 4u);
        EXPECT_EQ(r[3].size, 1024u);
        ASSERT_EQ(fake::self_grants().size(), 3u);
        EXPECT_EQ(fake::self_grants()[2].base, r[3].base);
        fake::Call const create = fake::calls_of("kos_task_create").at(0);
        EXPECT_EQ(create.args[0], reinterpret_cast<uintptr_t>(r[3].base));
        EXPECT_EQ(create.args[1], 1024u);
        fake::Call const grant = fake::calls_of("kos_task_sched_grant").at(0);
        EXPECT_EQ(grant.args[1], 13u) << "the console's priority plus its IRQ thread's offset";

        // Narrowed at the end of its handover, never closed: HANDOUT stays with the init.
        fake::Cap const* const ep = console_endpoint();
        ASSERT_NE(ep, nullptr);
        EXPECT_EQ(ep->rights, KEPT);
        std::vector<fake::Call> const narrow = fake::calls_of("kos_cap_narrow");
        ASSERT_EQ(narrow.size(), 3u) << "spi0's and sensor's at boot, the console's at its handover";
        EXPECT_EQ(narrow[2].args[0], ep->handle);
        fake::Call const probe = probes().at(0);
        EXPECT_EQ(probe.args[0], static_cast<uint64_t>(KOS_CAP_STDOUT));
        EXPECT_EQ(probe.args[2], kickos::driver::KOS_DRV_HANDOVER_PROBE_US);

        // The grant is the table's figure, not one the bring-up derives.
        harness::Patched patched{systems::find("golden_xmc")};
        patched.task("console").ceiling = 20u;
        use(patched);
        fake::script({step::ready_all(), step::ready_all(), step::ready_all()});
        ASSERT_EQ(run().kind, Outcome::Kind::IDLE);
        EXPECT_EQ(fake::calls_of("kos_task_sched_grant").at(0).args[1], 20u);
    }

    TEST_F(Driver, every_driver_thread_runs_on_the_declared_core)
    {
        harness::Patched patched{systems::find("golden_xmc")};
        patched.task("spi0").core_mask = 1u << 0;
        use(patched);
        fake::script({step::ready_all()});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;
        fake::Thread const* const bus = fake::spawned("bus").at(0);
        EXPECT_EQ(bus->params.core_mask, 1u << 0);
        EXPECT_EQ(fake::instance("bus")->core_mask, 1u << 0) << "the task's grant narrowed to it";
        EXPECT_EQ(fake::spawned("uartirq").at(0)->params.core_mask, 0u) << "the console declares none";
    }

    TEST_F(Driver, a_start_failing_before_its_first_spawn_is_restarted_then_dead_for_good)
    {
        use("golden_xmc");
        fake::refuse_if("kos_irq_claim", -KOS_EBUSY, 4,
                        [](fake::Call const& c)
                        {
                            return c.args[0] == 85u;
                        });
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;
        kickos::init::StatusFields const spi = status("spi0");
        EXPECT_EQ(spi.deaths, 4u);
        EXPECT_EQ(spi.restarts_left, 0u);
        EXPECT_EQ(spi.state, 0u);
        EXPECT_TRUE(fake::spawned("bus").empty());
        EXPECT_EQ(fake::calls_of("kos_task_slay").size(), 4u) << "each instance slain by the init";
        EXPECT_EQ(status("sensor").state, kickos::init::STATUS_DEPENDENCY_DOWN);
        // health watches spi0 at bit 0 and sensor at bit 1, told of each.
        EXPECT_EQ(fake::calls_of("kos_notify").size(), 5u);
        EXPECT_NE(fake::console().find("init: `spi0` failed to start: the driver's start answered -1\n"),
                  std::string::npos)
            << fake::console();
    }

    TEST_F(Driver, a_failing_receiver_that_is_not_the_entry_ends_the_task_and_it_restarts_through_its_descriptor)
    {
        use("driver_restart");
        fake::script({step::ready_all(), step::ready_all(),
                      // The worker traps: the kernel ends the task with a fault, then reports it dead.
                      step::end("worker", KOS_EXIT_FAULT), step::die("worker"), step::ready_all(),
                      step::end("worker", KOS_EXIT_FAULT), step::die("worker"), step::nothing()});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;
        EXPECT_EQ(fake::spawned("entry").size(), 2u) << "started again through its descriptor";
        EXPECT_EQ(fake::spawned("worker").size(), 2u);
        EXPECT_EQ(fake::calls_of("kos_task_kill").size(), 2u);
        kickos::init::StatusFields const drv = status("drv");
        EXPECT_EQ(drv.deaths, 2u);
        EXPECT_EQ(drv.state, 0u);
        EXPECT_EQ(fake::calls_of("kos_notify").size(), 2u) << "the watcher told of each death";
        // The count spent, the init's HANDOUT goes, so the client's calls are refused.
        int64_t const ep = static_cast<int64_t>(fake::spawned("worker").at(0)->caps.at(0).object);
        EXPECT_FALSE(fake::handout(static_cast<size_t>(ep)));
    }

    TEST_F(Driver, a_restart_waits_for_a_member_still_sweeping)
    {
        use("driver_restart");
        fake::script({step::ready_all(), step::ready_all(),
                      []
                      {
                          // The worker traps: the group is cancelled and the entry's sweep is slow.
                          fake::stall("worker");
                          fake::cancel("worker", KOS_EXIT_FAULT);
                      },
                      []
                      {
                          EXPECT_EQ(fake::spawned("worker").size(), 1u) << "no restart while a member sweeps";
                          EXPECT_TRUE(fake::calls_of("kos_task_kill").empty());
                          fake::sweep("worker");
                      },
                      step::ready_all(), step::nothing()});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;
        EXPECT_EQ(fake::spawned("worker").size(), 2u);
        EXPECT_EQ(fake::calls_of("kos_task_kill").size(), 1u);
        EXPECT_TRUE(fake::calls_of("kos_task_slay").empty()) << "an ended task is never slain";
        EXPECT_EQ(status("drv").deaths, 1u);
    }

    TEST_F(Driver, a_failed_console_start_narrows_the_endpoint_before_it_prints)
    {
        use("golden_xmc");
        fake::refuse_if("kos_thread_create", -KOS_ENOMEM, 1,
                        [](fake::Call const& c)
                        {
                            return c.text == "service";
                        });
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;
        EXPECT_NE(fake::console().find("[xmcuartirq] ERROR: driver thread spawn failed\n"), std::string::npos)
            << "printed on the kernel console the narrowing gave back: " << fake::console();
        EXPECT_EQ(status("console").deaths, 1u);
        EXPECT_EQ(status("console").state, 0u) << "the composition gives it no restart";
        EXPECT_FALSE(fake::handout(static_cast<size_t>(fake::stdout_object())));
    }

    // The console's starts, each from its kos_task_create up to the handover's probe.
    std::vector<std::vector<std::string>> console_starts()
    {
        std::vector<std::vector<std::string>> out;
        for (size_t n = 0; n < fake::calls_of("kos_task_create").size(); n++)
        {
            std::vector<std::string> start = start_of(n);
            auto const publish = std::find(start.begin(), start.end(), "kos_console_publish");
            if (publish == start.end())
            {
                continue;
            }
            auto const probe = std::find(publish, start.end(), "kos_send_timed");
            if (probe != start.end())
            {
                start.erase(probe + 1, start.end());
            }
            out.push_back(start);
        }
        return out;
    }

    TEST_F(Driver, a_console_driver_s_restart_repeats_the_handover)
    {
        harness::Patched patched{systems::find("golden_xmc")};
        patched.task("console").restart_max = 1u;
        use(patched);
        fake::script({step::ready_all(), step::ready_all(), step::die("service"), step::ready_all(),
                      step::nothing()});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;

        std::vector<std::vector<std::string>> const starts = console_starts();
        ASSERT_EQ(starts.size(), 2u);
        std::vector<std::string> const expected = {
            "kos_task_create", "kos_task_sched_grant", "kos_task_watch", "kos_console_publish", "kos_irq_claim",
            "kos_notify_create", "kos_notify_badge", "kos_irq_bind_notify", "kos_handle_close", "kos_thread_create",
            "kos_notify_badge", "kos_thread_create", "kos_handle_close", "kos_handle_close", "kos_handle_close",
            "kos_cap_narrow", "kos_send_timed"};
        EXPECT_EQ(starts[0], expected);
        EXPECT_EQ(starts[1], expected) << "published before the claim, the receiver last, narrowed, probed";

        std::vector<fake::Call> const publishes = fake::calls_of("kos_console_publish");
        ASSERT_EQ(publishes.size(), 2u);
        EXPECT_EQ(publishes[1].args[0], publishes[0].args[0]) << "the endpoint the init kept";
        EXPECT_EQ(publishes[1].result, 0) << "published through HANDOUT alone";
        EXPECT_EQ(fake::reclaims(), 1u) << "the kernel took the console back at the death";
        fake::Cap const* const ep = console_endpoint();
        ASSERT_NE(ep, nullptr);
        EXPECT_EQ(ep->rights, KEPT) << "the WAIT the publish seated dropped at the end of the handover";
        EXPECT_EQ(fake::spawned("service").size(), 2u);
        EXPECT_EQ(status("console").deaths, 1u);
        EXPECT_EQ(status("console").state, kickos::init::STATUS_ALIVE);
        EXPECT_NE(fake::console().find("init: `console` is dead and released\n"), std::string::npos)
            << fake::console();
    }

    TEST_F(Driver, a_failure_during_the_second_handover_narrows_before_it_prints)
    {
        harness::Patched patched{systems::find("golden_xmc")};
        patched.task("console").restart_max = 1u;
        use(patched);
        // The second instance's receiver is refused, so no thread of it ever holds WAIT.
        fake::refuse_if("kos_thread_create", -KOS_ENOMEM, 1,
                        [](fake::Call const& c)
                        {
                            return c.text == "service" and fake::spawned("service").size() == 1u;
                        });
        fake::script({step::ready_all(), step::ready_all(), step::die("service"), step::nothing()});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;

        std::vector<std::vector<std::string>> const starts = console_starts();
        ASSERT_EQ(starts.size(), 2u);
        std::vector<std::string> const& second = starts[1];
        auto const narrow = std::find(second.begin(), second.end(), "kos_cap_narrow");
        auto const print = std::find(second.begin(), second.end(), "kos_print");
        ASSERT_NE(narrow, second.end()) << "the failure narrows the init's capability";
        ASSERT_NE(print, second.end());
        EXPECT_LT(narrow - second.begin(), print - second.begin());
        EXPECT_NE(fake::console().find("[xmcuartirq] ERROR: driver thread spawn failed\n"), std::string::npos)
            << "printed on the kernel console the narrowing gave back: " << fake::console();
        EXPECT_EQ(fake::reclaims(), 2u) << "at the first instance's death, then at the narrow";
        EXPECT_EQ(status("console").deaths, 2u);
        EXPECT_EQ(status("console").state, 0u);
    }

    TEST_F(Driver, a_handover_probe_that_is_not_taken_is_a_failed_start)
    {
        fake::Config c = harness::config(systems::find("golden_xmc"));
        c.console_receives = false;
        use("golden_xmc", c);
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;
        std::vector<fake::Call> const sent = probes();
        ASSERT_EQ(sent.size(), 1u);
        EXPECT_EQ(sent[0].result, -KOS_ETIMEDOUT) << "a fresh endpoint parks the probe until its deadline";
        EXPECT_EQ(fake::calls_of("kos_task_slay").size(), 1u) << "the init slays the driver it handed over to";
        EXPECT_EQ(status("console").deaths, 1u);
        EXPECT_EQ(status("console").state, 0u);
    }

    struct Ending
    {
        bool receives;
        size_t sends;
    };

    class Drain : public Driver, public ::testing::WithParamInterface<Ending>
    {
    };

    TEST_P(Drain, the_ending_drains_the_console_within_two_probes)
    {
        harness::Patched patched{systems::find("golden_xmc")};
        patched.header().flags = KOS_TABLE_ENDS_TASK;
        patched.header().ends_task = harness::index_of(patched.system(), "app");
        fake::Config c = harness::config(patched.system());
        system_ = &patched.system();
        fake::reset(c);
        uint64_t ended = 0;
        bool const receives = GetParam().receives;
        fake::script({step::ready_all(), step::ready_all(),
                      [&ended, receives]
                      {
                          ended = fake::now();
                          // A console that stops taking what it is sent, its receiver alive.
                          fake::set_console_receives(receives);
                          fake::end("app", 3);
                      }});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::SHUTDOWN) << outcome.message;
        EXPECT_EQ(outcome.status, 3);
        std::vector<fake::Call> const sends = probes();
        ASSERT_EQ(sends.size(), 1u + GetParam().sends) << "the handover's probe, then the drain's";
        for (fake::Call const& s : sends)
        {
            EXPECT_EQ(s.args[1], 0u);
            EXPECT_EQ(s.args[2], kickos::driver::KOS_DRV_HANDOVER_PROBE_US);
        }
        EXPECT_LE(fake::now() - ended, 2u * kickos::driver::KOS_DRV_HANDOVER_PROBE_US * 1000u);
    }

    INSTANTIATE_TEST_SUITE_P(Console, Drain, ::testing::Values(Ending{true, 2u}, Ending{false, 1u}),
                             [](::testing::TestParamInfo<Ending> const& p)
                             {
                                 if (p.param.receives)
                                 {
                                     return std::string{"taken"};
                                 }
                                 return std::string{"stalled"};
                             });

    TEST_F(Driver, no_drain_without_a_console_driver)
    {
        use("default_qemu_arm64");
        fake::script({step::end("main", 0)});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::SHUTDOWN) << outcome.message;
        EXPECT_TRUE(probes().empty());
    }
}
