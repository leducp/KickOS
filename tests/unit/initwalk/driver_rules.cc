// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// Packaged drivers under the init (docs/design-m10-target.md, section 2), each created from its
// declaration over the scripted kernel, which models what the create applies: `golden_xmc`
// (console, the xmcuartirq shape, and spi0, the xmcssc shape, then sensor using spi0, app using
// sensor, and health watching spi0 and sensor), and `driver_restart` (drv, an entry thread and a
// receiving worker with one restart, a client of it and a watcher of it).

#include "drivers.h"
#include "harness.h"

#include <kickos/sys/driver_service.h>
#include <kickos/sys/errno.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

namespace
{
    using harness::creates_of;
    using harness::Outcome;

    constexpr uint32_t KEPT = KOS_CAP_SIGNAL | KOS_CAP_TRANSFER | KOS_CAP_HANDOUT;

    using Driver = harness::Fixture;

    // The zero-length sends on stdout, a probe of the console.
    std::vector<fake::Call> probes()
    {
        std::vector<fake::Call> out;
        for (fake::Call const& c : fake::calls_of("kos_send"))
        {
            if (c.args[1] == 0u)
            {
                out.push_back(c);
            }
        }
        return out;
    }

    // How far past the clock at call `c` its argument `i`, a deadline, lies.
    uint64_t span_of(fake::Call const& c, size_t i)
    {
        return c.args.at(i) - c.at;
    }

    // The calls of the `n`-th start of `name`, from the two before its create, its notification's
    // create and its doorbell's mint, up to the handover's first read of the task's state.
    std::vector<std::string> start_of(char const* name, size_t n)
    {
        std::vector<fake::Call> const& all = fake::calls();
        size_t creates = 0;
        size_t at = all.size();
        for (size_t i = 0; i < all.size() and at == all.size(); i++)
        {
            if (all[i].fn == "kos_task_create" and all[i].text == name)
            {
                if (creates == n)
                {
                    at = i;
                }
                creates++;
            }
        }
        std::vector<std::string> out;
        if (at < 2u or at == all.size())
        {
            return out;
        }
        for (size_t i = at - 2u; i < all.size(); i++)
        {
            fake::Call const& c = all[i];
            if (c.fn == "kos_thread_self")
            {
                continue;
            }
            out.push_back(c.fn);
            if (c.fn == "kos_task_state")
            {
                break;
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
        fake::script({step::ready_all(), step::ready_all(), step::ready_all()});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;

        std::vector<fake::Block> const& r = fake::reservations();
        ASSERT_EQ(r.size(), 4u);
        std::vector<fake::Call> const grants = fake::calls_of("kos_mem_self_grant");
        ASSERT_EQ(grants.size(), 3u);
        EXPECT_EQ(grants[2].args[0], reinterpret_cast<uintptr_t>(r[3].base));
        EXPECT_EQ(grants[2].args[2], static_cast<uint64_t>(KOS_MEM_NOCACHE));
        EXPECT_EQ(creates_of("console").at(0).args[5], static_cast<uint64_t>(KOS_MEM_NOCACHE));
    }

    TEST_F(Driver, a_console_driver_is_created_on_the_init_s_block_and_endpoint_then_handed_over)
    {
        use("golden_xmc");
        fake::script({step::ready_all(), step::ready_all(), step::ready_all()});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;

        std::vector<std::string> const expected = {
            "kos_notify_create", "kos_notify_badge", "kos_task_create", "kos_handle_close", "kos_handle_close",
            "kos_watch", "kos_cap_narrow", "kos_task_state"};
        EXPECT_EQ(start_of("console", 0), expected);
        EXPECT_EQ(fake::calls_of("kos_endpoint_create").size(), 3u) << "every endpoint the init's, made at boot";
        EXPECT_TRUE(fake::calls_of("kos_irq_claim").empty()) << "the create claims the declared lines";

        // The ring block the init reserved and self-granted at boot, after the shared region.
        std::vector<fake::Block> const& r = fake::reservations();
        ASSERT_EQ(r.size(), 4u);
        EXPECT_EQ(r[3].size, 1024u);
        fake::Call const create = creates_of("console").at(0);
        EXPECT_EQ(create.args[0], reinterpret_cast<uintptr_t>(r[3].base));
        EXPECT_EQ(create.args[1], 1024u);
        EXPECT_EQ(create.args[3], 3u) << "its endpoint, notification and doorbell";
        EXPECT_EQ(create.args[4], 0u) << "no window passed: the declaration's is the kernel's to seat";

        // The entry thread holds the endpoint, the notification and the doorbell at its layout's
        // places, and the line the create claimed.
        fake::Thread const* const entry = fake::spawned("console").at(0);
        ASSERT_EQ(entry->caps.size(), 4u);
        ASSERT_EQ(entry->dest.size(), 3u);
        EXPECT_EQ(entry->dest[0], KOS_SPAWN_DELEGATED_CAP0 + 2u) << "the endpoint after notify and line";
        EXPECT_EQ(entry->dest[1], KOS_SPAWN_DELEGATED_CAP0 + 0u);
        EXPECT_EQ(entry->dest[2], KOS_SPAWN_DELEGATED_CAP0 + 3u);
        EXPECT_EQ(entry->caps[0].rights, KOS_CAP_WAIT | KOS_CAP_TRANSFER);
        EXPECT_EQ(entry->caps[2].badge, 1) << "the doorbell on the first bit above the line";
        ASSERT_EQ(entry->windows.size(), 1u);
        EXPECT_EQ(entry->windows[0].base, 0x40030000u);
        EXPECT_EQ(fake::objects()[entry->caps[3].object].kind, fake::Kind::LINE);
        ASSERT_FALSE(fake::tasks().empty());
        EXPECT_EQ(fake::tasks()[0].ceiling, 13u) << "the console's priority plus its IRQ thread's offset";

        // Narrowed at the end of its handover, never closed: HANDOUT stays with the init.
        fake::Cap const* const ep = console_endpoint();
        ASSERT_NE(ep, nullptr);
        EXPECT_EQ(ep->rights, KEPT);
        EXPECT_TRUE(probes().empty()) << "the handover reads the task's readiness";
    }

    TEST_F(Driver, a_driver_runs_on_its_declared_core)
    {
        harness::Patched patched{systems::find("golden_xmc")};
        patched.task("spi0").core_mask = 1u << 0;
        use(patched);
        fake::script({step::ready_all()});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;
        EXPECT_EQ(fake::instance("spi0")->core_mask, 1u << 0) << "the task's grant narrowed to it";
        EXPECT_EQ(fake::instance("console")->core_mask, 0xFFFFFFFFu) << "the console declares none";
    }

    TEST_F(Driver, a_create_the_kernel_refuses_is_restarted_then_dead_for_good)
    {
        use("golden_xmc");
        fake::refuse_if("kos_task_create", -KOS_EBUSY, 4,
                        [](fake::Call const& c)
                        {
                            return c.text == "spi0";
                        });
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;
        kickos::init::StatusFields const spi = status("spi0");
        EXPECT_EQ(spi.deaths, 4u);
        EXPECT_EQ(spi.restarts_left, 0u);
        EXPECT_EQ(spi.state, 0u);
        EXPECT_TRUE(fake::spawned("spi0").empty());
        EXPECT_TRUE(fake::slays().empty()) << "a refused create leaves no task to slay";
        EXPECT_EQ(status("sensor").state, kickos::init::STATUS_DEPENDENCY_DOWN);
        // health watches spi0 at bit 0 and sensor at bit 1, told of each.
        EXPECT_EQ(fake::calls_of("kos_notify").size(), 5u);
        EXPECT_NE(fake::console().find("init: `spi0` failed to start: the driver's start answered -16\n"),
                  std::string::npos)
            << fake::console();
        EXPECT_TRUE(fake::calls_of("kos_notify_create").size() >= 4u);
        // The init's own program handle stays open, and each other program's goes back after its create.
        size_t const program_closes = fake::calls_of("kos_program_find").size() - 1u;
        EXPECT_EQ(fake::calls_of("kos_notify_create").size(), fake::calls_of("kos_handle_close").size()
                                                                   - fake::calls_of("kos_notify_badge").size()
                                                                   - program_closes)
            << "every notification a refused start made goes back";
    }

    TEST_F(Driver, a_create_answered_eagain_waits_on_the_instance_watch_then_starts_at_its_bit)
    {
        use("golden_xmc");
        uint16_t const spi = harness::index_of(*system_, "spi0");
        fake::retire("spi0");
        fake::script({[this, spi]
                      {
                          EXPECT_EQ(harness::record(spi).phase, kickos::init::Phase::STARTWAIT);
                          EXPECT_EQ(harness::watches_held(), 1u);
                          EXPECT_TRUE(fake::spawned("spi0").empty());
                          EXPECT_EQ(status("spi0").state, kickos::init::STATUS_ALIVE);
                          EXPECT_EQ(status("sensor").state, kickos::init::STATUS_ALIVE);
                          fake::retired("spi0");
                      },
                      step::ready_all(), step::ready_all()});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;
        std::vector<fake::Call> const creates = creates_of("spi0");
        ASSERT_EQ(creates.size(), 2u) << "refused, then started at the bit";
        std::vector<fake::Call> const watches = harness::instance_watches();
        ASSERT_EQ(watches.size(), 1u);
        EXPECT_EQ(watches[0].args[2], creates[0].args[2]) << "the program the create used";
        EXPECT_EQ(fake::spawned("spi0").size(), 1u);
        EXPECT_EQ(status("spi0").deaths, 0u);
        EXPECT_EQ(harness::watches_held(), 0u);
        EXPECT_TRUE(fake::calls_of("kos_sleep_for").empty());
        EXPECT_TRUE(fake::slays().empty());
    }

    TEST_F(Driver, a_second_eagain_after_nothing_in_the_way_is_a_failed_start)
    {
        use("golden_xmc");
        fake::refuse_if("kos_task_create", -KOS_EAGAIN, 2,
                        [](fake::Call const& c)
                        {
                            return c.text == "spi0";
                        });
        fake::script({step::ready_all(), step::ready_all()});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;
        std::vector<fake::Call> const watches = harness::instance_watches();
        ASSERT_EQ(watches.size(), 1u);
        EXPECT_EQ(watches[0].result, -KOS_EALREADY);
        EXPECT_EQ(creates_of("spi0").size(), 3u) << "the create, its one retry, then the restart's";
        EXPECT_EQ(status("spi0").deaths, 1u);
        EXPECT_EQ(fake::spawned("spi0").size(), 1u);
        EXPECT_NE(fake::console().find("init: `spi0` failed to start: the driver's start answered -11\n"),
                  std::string::npos)
            << fake::console();
        EXPECT_TRUE(fake::calls_of("kos_sleep_for").empty());
    }

    TEST_F(Driver, a_driver_task_that_ends_is_restarted_through_its_declaration)
    {
        use("driver_restart");
        fake::script({step::ready_all(), step::ready_all(),
                      // A member traps: the kernel ends the task with a fault, then reports it dead.
                      step::end("drv", KOS_EXIT_FAULT), step::die("drv"), step::ready_all(),
                      step::end("drv", KOS_EXIT_FAULT), step::die("drv"), step::nothing()});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;
        EXPECT_EQ(fake::spawned("drv").size(), 2u) << "created again from its declaration";
        EXPECT_EQ(fake::releases().size(), 2u);
        kickos::init::StatusFields const drv = status("drv");
        EXPECT_EQ(drv.deaths, 2u);
        EXPECT_EQ(drv.state, 0u);
        EXPECT_EQ(fake::calls_of("kos_notify").size(), 2u) << "the watcher told of each death";
        // The count spent, the init's HANDOUT goes, so the client's calls are refused.
        int64_t const ep = static_cast<int64_t>(fake::spawned("drv").at(0)->caps.at(0).object);
        EXPECT_FALSE(fake::handout(static_cast<size_t>(ep)));
    }

    TEST_F(Driver, a_restart_waits_for_a_member_still_sweeping)
    {
        use("driver_restart");
        fake::script({step::ready_all(), step::ready_all(),
                      []
                      {
                          // A member traps: the group is cancelled and the entry's sweep is slow.
                          fake::stall("drv");
                          fake::cancel("drv", KOS_EXIT_FAULT);
                      },
                      []
                      {
                          EXPECT_EQ(fake::spawned("drv").size(), 1u) << "no restart while a member sweeps";
                          EXPECT_TRUE(fake::releases().empty());
                          fake::sweep("drv");
                      },
                      step::ready_all(), step::nothing()});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;
        EXPECT_EQ(fake::spawned("drv").size(), 2u);
        EXPECT_EQ(fake::releases().size(), 1u);
        EXPECT_TRUE(fake::slays().empty()) << "an ended task is never slain";
        EXPECT_EQ(status("drv").deaths, 1u);
    }

    TEST_F(Driver, a_console_driver_s_restart_repeats_the_handover)
    {
        harness::Patched patched{systems::find("golden_xmc")};
        patched.task("console").restart_max = 1u;
        use(patched);
        fake::script({step::ready_all(), step::ready_all(), step::die("console"), step::ready_all(),
                      step::nothing()});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;

        std::vector<std::string> const expected = {
            "kos_notify_create", "kos_notify_badge", "kos_task_create", "kos_handle_close", "kos_handle_close",
            "kos_watch", "kos_cap_narrow", "kos_task_state"};
        ASSERT_EQ(creates_of("console").size(), 2u);
        EXPECT_EQ(start_of("console", 0), expected);
        EXPECT_EQ(start_of("console", 1), expected) << "created, watched, narrowed, its readiness read";
        EXPECT_EQ(fake::reclaims(), 1u) << "the kernel took the console back at the death";
        fake::Cap const* const ep = console_endpoint();
        ASSERT_NE(ep, nullptr);
        EXPECT_EQ(ep->rights, KEPT) << "the WAIT the create's publish seated dropped at the end of the handover";
        EXPECT_EQ(fake::spawned("console").size(), 2u);
        EXPECT_EQ(status("console").deaths, 1u);
        EXPECT_EQ(status("console").state, kickos::init::STATUS_ALIVE);
        EXPECT_NE(fake::console().find("init: `console` is dead and released\n"), std::string::npos)
            << fake::console();
    }

    TEST_F(Driver, a_refused_console_create_narrows_before_it_prints)
    {
        harness::Patched patched{systems::find("golden_xmc")};
        patched.task("console").restart_max = 1u;
        use(patched);
        // The second instance is refused, so the WAIT the first create's publish seated must go
        // before the init prints the failure.
        fake::refuse_if("kos_task_create", -KOS_ENOMEM, 1,
                        [](fake::Call const& c)
                        {
                            return c.text == "console" and fake::spawned("console").size() == 1u;
                        });
        fake::script({step::ready_all(), step::ready_all(), step::die("console"), step::nothing()});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;
        std::vector<std::string> const second = start_of("console", 1);
        auto const narrow = std::find(second.begin(), second.end(), "kos_cap_narrow");
        ASSERT_NE(narrow, second.end()) << "the failure narrows the init's capability";
        std::vector<std::string> const seq = fake::sequence({"kos_cap_narrow", "kos_send"});
        ASSERT_FALSE(seq.empty());
        EXPECT_EQ(seq.back(), "kos_send") << "the failure was reported before the narrow";
        EXPECT_EQ(fake::console().find("failed to start"), std::string::npos)
            << "a user line reached a console whose driver is gone: " << fake::console();
        EXPECT_EQ(status("console").deaths, 2u);
        EXPECT_EQ(status("console").state, 0u);
    }

    // The narrows of the init's capability on task `task`'s endpoint.
    size_t endpoint_narrows(uint16_t task)
    {
        size_t n = 0;
        for (fake::Call const& c : fake::calls_of("kos_cap_narrow"))
        {
            if (c.args[0] == harness::record(task).endpoint)
            {
                n++;
            }
        }
        return n;
    }

    // A start that only waits on its instance's watch is no failed start: the init's endpoint
    // keeps WAIT, and its one receiver holder with it, until the start that runs narrows it.
    TEST_F(Driver, a_console_start_waiting_on_its_instance_watch_leaves_the_endpoint_whole)
    {
        use("golden_xmc");
        uint16_t const console = harness::index_of(*system_, "console");
        fake::retire("console");
        fake::script({[console]
                      {
                          EXPECT_EQ(harness::record(console).phase, kickos::init::Phase::STARTWAIT);
                          EXPECT_EQ(endpoint_narrows(console), 0u)
                              << "a start that only waits narrowed the init's endpoint";
                          fake::retired("console");
                      },
                      step::ready_all(), step::ready_all()});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;
        ASSERT_EQ(creates_of("console").size(), 2u) << "refused, then started at the bit";
        EXPECT_EQ(endpoint_narrows(console), 1u) << "the start that ran narrows once";
        fake::Cap const* const ep = console_endpoint();
        ASSERT_NE(ep, nullptr);
        EXPECT_EQ(ep->rights, KEPT);
    }

    // The handover's waits on the console's watch bit.
    std::vector<fake::Call> handover_waits(uint16_t console)
    {
        std::vector<fake::Call> out;
        for (fake::Call const& w : fake::calls_of("kos_notify_wait"))
        {
            if (w.args[0] == (1u << (console % 32u)))
            {
                out.push_back(w);
            }
        }
        return out;
    }

    // No later start runs before the console is ready, however long its bring-up takes.
    TEST_F(Driver, a_console_that_is_not_ready_holds_every_later_start_with_no_deadline)
    {
        fake::Config c = harness::config(systems::find("golden_xmc"));
        c.console_receives = false;
        use("golden_xmc", c);
        uint16_t const console = harness::index_of(*system_, "console");
        fake::script({[] { fake::advance(std::chrono::nanoseconds(std::chrono::seconds(10)).count()); }});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;
        std::vector<fake::Call> const waits = handover_waits(console);
        ASSERT_FALSE(waits.empty());
        EXPECT_EQ(static_cast<int64_t>(waits.back().args[1]), KOS_NEVER.ns);
        EXPECT_TRUE(fake::slays().empty()) << "a slow bring-up was slain";
        EXPECT_TRUE(creates_of("spi0").empty()) << "a later task started before the console was ready";
        EXPECT_EQ(status("console").deaths, 0u);
        EXPECT_EQ(status("console").state, kickos::init::STATUS_ALIVE);
    }

    TEST_F(Driver, a_console_ready_late_is_handed_over)
    {
        fake::Config c = harness::config(systems::find("golden_xmc"));
        c.console_receives = false;
        use("golden_xmc", c);
        fake::script({[]
                      {
                          fake::advance(std::chrono::nanoseconds(std::chrono::seconds(10)).count());
                          fake::set_console_receives(true);
                      },
                      step::ready_all(), step::ready_all()});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;
        EXPECT_TRUE(fake::slays().empty());
        EXPECT_EQ(creates_of("spi0").size(), 1u) << "the walk went on past the handover";
        EXPECT_EQ(status("console").deaths, 0u);
    }

    TEST_F(Driver, a_console_that_ends_before_it_is_ready_is_a_failed_start_reported_after_its_death)
    {
        fake::Config c = harness::config(systems::find("golden_xmc"));
        c.console_receives = false;
        use("golden_xmc", c);
        uint16_t const console = harness::index_of(*system_, "console");
        fake::on_spawn = [](fake::Thread const&)
        {
            if (fake::instance("console") != nullptr)
            {
                fake::stall("console");
            }
        };
        size_t died = 0;
        fake::script({
            []
            {
                fake::on_spawn = nullptr;
                fake::cancel("console", KOS_EXIT_FAULT);
            },
            [&died]
            {
                died = fake::calls().size();
                fake::sweep("console");
            },
        });
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;
        ASSERT_NE(died, 0u) << "the handover did not wait for the death";
        std::vector<fake::Call> const& calls = fake::calls();
        for (size_t k = 0; k < died and k < calls.size(); k++)
        {
            EXPECT_NE(calls[k].fn, "kos_task_slay") << "slain before its death";
            EXPECT_FALSE(calls[k].fn == "kos_send" and calls[k].args[1] != 0u) << "reported before its death";
        }
        std::vector<fake::Call> const waits = handover_waits(console);
        ASSERT_EQ(waits.size(), 2u) << "to the end, then to the death";
        EXPECT_EQ(static_cast<int64_t>(waits[1].args[1]), KOS_NEVER.ns);
        EXPECT_TRUE(probes().empty());
        EXPECT_EQ(status("console").deaths, 1u);
        EXPECT_EQ(status("console").state, 0u);
    }

    struct Ending
    {
        bool receives;
    };

    class Drain : public Driver, public ::testing::WithParamInterface<Ending>
    {
    };

    TEST_P(Drain, the_ending_drains_the_console_with_one_bounded_call)
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
        EXPECT_TRUE(probes().empty());
        std::vector<fake::Call> const drains = fake::calls_of("kos_call");
        ASSERT_EQ(drains.size(), 1u);
        EXPECT_EQ(drains[0].args[0], KOS_CAP_STDOUT);
        uint64_t const probe_ns = static_cast<uint64_t>(
            std::chrono::nanoseconds(kickos::driver::KOS_DRV_SEND_WAIT).count());
        EXPECT_EQ(span_of(drains[0], 2), probe_ns);
        EXPECT_LE(fake::now() - ended, probe_ns);
    }

    INSTANTIATE_TEST_SUITE_P(Console, Drain, ::testing::Values(Ending{true}, Ending{false}),
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
        EXPECT_TRUE(fake::calls_of("kos_call").empty());
    }
}
