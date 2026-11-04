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

    // The zero-length sends on stdout: the handover's probe.
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

    // The handover's probe bound: the readiness wait its receiver may sit in, then the probe's own.
    constexpr uint64_t HANDOVER_PROBE_US = kickos::driver::KOS_DRV_HANDOVER_PROBE_US
                                           + uint64_t{kickos::driver::KOS_DRV_READY_WAIT_MAX}
                                                 * (kickos::driver::KOS_DRV_READY_WAIT_NS / 1000u);

    // The creates of task `name`, in order.
    std::vector<fake::Call> creates_of(char const* name)
    {
        std::vector<fake::Call> out;
        for (fake::Call const& c : fake::calls_of("kos_task_create"))
        {
            if (c.text == name)
            {
                out.push_back(c);
            }
        }
        return out;
    }

    // The calls of the `n`-th start of `name`, from the two before its create, its notification's
    // create and its doorbell's mint, up to the handover's probe.
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
            if (c.fn == "kos_clock_now" or c.fn == "kos_thread_self")
            {
                continue;
            }
            out.push_back(c.fn);
            if (c.fn == "kos_send" and c.args[1] == 0u)
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
            "kos_task_watch", "kos_cap_narrow", "kos_send"};
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
        fake::Call const probe = probes().at(0);
        EXPECT_EQ(probe.args[0], static_cast<uint64_t>(KOS_CAP_STDOUT));
        EXPECT_EQ(probe.args[2], HANDOVER_PROBE_US);
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

    TEST_F(Driver, a_line_retiring_from_the_instance_before_is_waited_for)
    {
        use("golden_xmc");
        fake::refuse_if("kos_task_create", -KOS_EAGAIN, 3,
                        [](fake::Call const& c)
                        {
                            return c.text == "spi0";
                        });
        fake::script({step::ready_all()});
        Outcome const outcome = run();
        ASSERT_EQ(outcome.kind, Outcome::Kind::IDLE) << outcome.message;
        EXPECT_EQ(creates_of("spi0").size(), 4u) << "retried while its line retires";
        EXPECT_EQ(status("spi0").deaths, 0u);
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
            "kos_task_watch", "kos_cap_narrow", "kos_send"};
        ASSERT_EQ(creates_of("console").size(), 2u);
        EXPECT_EQ(start_of("console", 0), expected);
        EXPECT_EQ(start_of("console", 1), expected) << "created, watched, narrowed, probed";
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
        EXPECT_NE(fake::console().find("init: `console` failed to start: the driver's start answered -12\n"),
                  std::string::npos)
            << fake::console();
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
        EXPECT_EQ(fake::slays().size(), 1u) << "the init slays the driver it handed over to";
        EXPECT_NE(fake::console().find("[console] ERROR: the console handover probe was not taken\n"),
                  std::string::npos)
            << "printed on the kernel console the task's end gave back: " << fake::console();
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
        std::vector<fake::Call> const sends = probes();
        ASSERT_EQ(sends.size(), 1u) << "the handover's probe";
        EXPECT_EQ(sends[0].args[2], HANDOVER_PROBE_US);
        std::vector<fake::Call> const drains = fake::calls_of("kos_call");
        ASSERT_EQ(drains.size(), 1u);
        EXPECT_EQ(drains[0].args[0], KOS_CAP_STDOUT);
        EXPECT_EQ(drains[0].args[2], kickos::driver::KOS_DRV_HANDOVER_PROBE_US);
        EXPECT_LE(fake::now() - ended, kickos::driver::KOS_DRV_HANDOVER_PROBE_US * 1000u);
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
