// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The scripted kernel refuses what the kernel refuses, each rule against a call that breaks it, so
// a walk that breaks one fails here as it would on a board.

#include "fake_kernel.h"

#include <kickos/sys/errno.h>

#include <gtest/gtest.h>

#include <string>

namespace
{
    void entry(void*)
    {
    }

    class Fake : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            fake::reset(config_);
        }

        void with(fake::Config const& config)
        {
            config_ = config;
            fake::reset(config_);
        }

        kos_task_t task(void* data = nullptr, uint32_t size = 0u)
        {
            kos_task_t t = KOS_TASK_NONE;
            EXPECT_EQ(kos_task_create(data, size, 0u, &t), 0);
            return t;
        }

        kos_thread_params params(kos_task_t t)
        {
            kos_thread_params p = {};
            p.entry = entry;
            p.name = "t";
            p.prio = 10;
            p.task = t;
            return p;
        }

        int spawn(kos_thread_params const& p)
        {
            kos_thread_t thread = KOS_THREAD_NONE;
            return kos_thread_create(&p, &thread);
        }

        fake::Config config_;
    };

    TEST_F(Fake, a_stack_off_its_alignment_or_below_the_floor_is_refused)
    {
        fake::Config c;
        c.min_stack = 1024u;
        with(c);
        auto const block = static_cast<unsigned char*>(kos_ram_alloc(4096u));
        kos_thread_params p = params(task());
        p.stack_base = block;
        p.stack_size = 4096u;
        EXPECT_EQ(spawn(p), 0);
        p.stack_base = block + 8;
        p.stack_size = 2048u;
        EXPECT_EQ(spawn(p), -KOS_EINVAL);
        p.stack_base = block;
        p.stack_size = 2040u;
        EXPECT_EQ(spawn(p), -KOS_EINVAL);
        p.stack_size = 512u;
        EXPECT_EQ(spawn(p), -KOS_EINVAL);
    }

    TEST_F(Fake, a_masked_stack_is_one_whole_stride)
    {
        fake::Config c;
        c.sp_masked = true;
        c.stride = 4096u;
        with(c);
        kos_thread_params p = params(task());
        p.stack_base = kos_ram_alloc(4096u);
        p.stack_size = 2048u;
        EXPECT_EQ(spawn(p), -KOS_EINVAL);
        p.stack_size = 4096u;
        EXPECT_EQ(spawn(p), 0);
    }

    TEST_F(Fake, a_translating_stack_lies_in_its_task_s_data_region)
    {
        fake::Config c;
        c.translating = true;
        with(c);
        void* const data = kos_ram_alloc(8192u);
        void* const other = kos_ram_alloc(8192u);
        kos_thread_params p = params(task(data, 8192u));
        p.stack_base = other;
        p.stack_size = 8192u;
        EXPECT_EQ(spawn(p), -KOS_EPERM);
        p.stack_base = data;
        EXPECT_EQ(spawn(p), 0);
    }

    TEST_F(Fake, a_translating_board_hands_over_and_maps_whole_reservations)
    {
        fake::Config c;
        c.translating = true;
        with(c);
        void* const data = kos_ram_alloc(8192u);
        kos_task_t t = KOS_TASK_NONE;
        EXPECT_EQ(kos_task_create(data, 4096u, 0u, &t), -KOS_EPERM);
        kos_thread_params p = params(task());
        kos_window const half = {reinterpret_cast<uintptr_t>(kos_ram_alloc(8192u)), 4096u, KOS_WINDOW_MEMORY, 0};
        p.windows = &half;
        p.window_count = 1;
        EXPECT_EQ(spawn(p), -KOS_EPERM);
    }

    TEST_F(Fake, a_window_list_naming_one_memory_base_twice_is_refused)
    {
        uintptr_t const base = reinterpret_cast<uintptr_t>(kos_ram_alloc(256u));
        kos_window const twice[] = {{base, 256u, KOS_WINDOW_MEMORY, 0}, {base, 128u, KOS_WINDOW_MEMORY, KOS_WINDOW_RO}};
        kos_thread_params p = params(task());
        p.windows = twice;
        p.window_count = 2;
        EXPECT_EQ(spawn(p), -KOS_EINVAL);
    }

    TEST_F(Fake, a_device_or_port_window_carries_no_flags)
    {
        kos_window const device = {0x40000000u, 0x100u, KOS_WINDOW_DEVICE, KOS_WINDOW_RO};
        kos_window const ports = {0x60u, 4u, KOS_WINDOW_PORTS, KOS_WINDOW_UNCACHED};
        kos_thread_params p = params(task());
        p.window_count = 1;
        p.windows = &device;
        EXPECT_EQ(spawn(p), -KOS_EINVAL);
        p.windows = &ports;
        EXPECT_EQ(spawn(p), -KOS_EINVAL);
    }

    TEST_F(Fake, a_device_window_a_live_thread_holds_is_busy)
    {
        kos_window const device = {0x40000000u, 0x100u, KOS_WINDOW_DEVICE, 0};
        kos_thread_params p = params(task());
        p.windows = &device;
        p.window_count = 1;
        EXPECT_EQ(spawn(p), 0);
        kos_thread_params q = params(task());
        q.windows = &device;
        q.window_count = 1;
        EXPECT_EQ(spawn(q), -KOS_EBUSY);
    }

    TEST_F(Fake, a_claimed_line_is_busy)
    {
        kos_cap_t line = KOS_CAP_NONE;
        EXPECT_EQ(kos_irq_claim(7, 0u, &line), 0);
        EXPECT_EQ(kos_irq_claim(7, 0u, &line), -KOS_EBUSY);
    }

    TEST_F(Fake, the_init_s_table_holds_what_the_supply_backs)
    {
        fake::Config c;
        c.limits.cap_table = c.cap_reserved + 1u;
        with(c);
        kos_cap_t cap = KOS_CAP_NONE;
        EXPECT_EQ(kos_notify_create(&cap), 0);
        EXPECT_EQ(kos_endpoint_create(&cap), -KOS_EMFILE);
    }

    TEST_F(Fake, each_pool_holds_what_the_build_declares)
    {
        fake::Config c;
        c.limits.tasks = 1u;
        c.limits.threads = 1u;
        c.limits.endpoints = 1u;
        c.limits.notifications = 1u;
        c.limits.irq_handles = 1u;
        with(c);
        kos_task_t t = task();
        kos_task_t second = KOS_TASK_NONE;
        EXPECT_EQ(kos_task_create(nullptr, 0u, 0u, &second), -KOS_ENOMEM);
        EXPECT_EQ(spawn(params(t)), 0);
        EXPECT_EQ(spawn(params(t)), -KOS_ENOMEM);
        kos_cap_t cap = KOS_CAP_NONE;
        EXPECT_EQ(kos_endpoint_create(&cap), 0);
        EXPECT_EQ(kos_endpoint_create(&cap), -KOS_ENOMEM);
        EXPECT_EQ(kos_notify_create(&cap), 0);
        EXPECT_EQ(kos_notify_create(&cap), -KOS_ENOMEM);
        EXPECT_EQ(kos_irq_claim(3, 0u, &cap), 0);
        EXPECT_EQ(kos_irq_claim(4, 0u, &cap), -KOS_ENOMEM);
    }

    TEST_F(Fake, the_domain_pool_counts_every_space_on_a_translating_board)
    {
        fake::Config c;
        c.translating = true;
        c.limits.domains = 4u;
        with(c);
        task();
        kos_task_t second = KOS_TASK_NONE;
        EXPECT_EQ(kos_task_create(nullptr, 0u, 0u, &second), -KOS_ENOMEM) << "the kernel's two, the init's, one task";
    }

    TEST_F(Fake, root_s_free_regions_and_the_ram_owner_slots_run_out)
    {
        fake::Config c;
        c.limits.free_regions = 1u;
        c.limits.ram_owners = 2u;
        with(c);
        void* const a = kos_ram_alloc(256u);
        void* const b = kos_ram_alloc(256u);
        EXPECT_EQ(kos_ram_alloc(256u), nullptr);
        EXPECT_EQ(kos_mem_self_grant(a, 256u, 0u), 0);
        EXPECT_EQ(kos_mem_self_grant(b, 256u, 0u), -KOS_ENOMEM);
    }

    TEST_F(Fake, a_grant_narrows_a_task_s_priority_and_a_spawn_stays_within_it)
    {
        kos_task_t const t = task();
        EXPECT_EQ(kos_task_sched_grant(t, 9u, 0u), 0);
        EXPECT_EQ(kos_task_sched_grant(t, 12u, 0u), -KOS_EPERM) << "a grant never widens";
        kos_thread_params p = params(t);
        EXPECT_EQ(spawn(p), -KOS_EPERM) << "priority 10 over the ceiling 9";
        p.prio = 9;
        EXPECT_EQ(spawn(p), 0);
    }

    TEST_F(Fake, a_spawn_passes_no_authority_the_init_lacks)
    {
        fake::Config c;
        c.authority = KOS_AUTH_MEMORY;
        with(c);
        kos_thread_params p = params(task());
        p.authority = KOS_AUTH_IRQ;
        EXPECT_EQ(spawn(p), -KOS_EPERM);
    }

    TEST_F(Fake, another_creator_s_task_is_refused_to_the_init)
    {
        kos_task_t const foreign = fake::foreign_task();
        int status = 0;
        EXPECT_EQ(kos_task_state(foreign), -KOS_EPERM);
        EXPECT_EQ(kos_task_kill(foreign), -KOS_EPERM);
        EXPECT_EQ(kos_task_slay(foreign, 0u), -KOS_EPERM);
        EXPECT_EQ(kos_task_exit_status(foreign, &status), -KOS_EPERM);
        EXPECT_EQ(kos_task_sched_grant(foreign, 9u, 0u), -KOS_EPERM);
        EXPECT_EQ(spawn(params(foreign)), -KOS_EPERM);
    }

    TEST_F(Fake, a_notification_raised_needs_signal_and_waited_on_needs_wait_and_its_binding)
    {
        kos_cap_t note = KOS_CAP_NONE;
        ASSERT_EQ(kos_notify_create(&note), 0);
        uint32_t bits = 0;
        EXPECT_EQ(kos_notify_wait(note, 1u, 0u, &bits), -KOS_EPERM) << "not bound";
        kos_cap_t raiser = KOS_CAP_NONE;
        ASSERT_EQ(kos_notify_badge(note, 3u, &raiser), 0);
        ASSERT_EQ(kos_cap_narrow(raiser, KOS_CAP_WAIT), 0);
        EXPECT_EQ(kos_notify(raiser), -KOS_EACCES);
        ASSERT_EQ(kos_notify_bind(note), 0);
        ASSERT_EQ(kos_cap_narrow(note, KOS_CAP_SIGNAL), 0);
        EXPECT_EQ(kos_notify_wait(note, 1u, 0u, &bits), -KOS_EACCES);
    }

    TEST_F(Fake, a_priority_below_the_range_and_an_authority_bit_past_the_defined_ones_are_invalid)
    {
        kos_thread_params p = params(task());
        p.prio = 0;
        EXPECT_EQ(spawn(p), -KOS_EINVAL);
        p.prio = 10;
        p.authority = 1u << 8;
        EXPECT_EQ(spawn(p), -KOS_EINVAL);
    }

    TEST_F(Fake, a_spawn_past_its_task_s_ceiling_of_a_pool_is_refused_eagain)
    {
        fake::Config c;
        c.limits.task_endpoints = 1u;
        with(c);
        kos_cap_t a = KOS_CAP_NONE;
        kos_cap_t b = KOS_CAP_NONE;
        ASSERT_EQ(kos_endpoint_create(&a), 0);
        ASSERT_EQ(kos_endpoint_create(&b), 0);
        kos_cap_grant const two[] = {{a, KOS_CAP_SIGNAL}, {b, KOS_CAP_SIGNAL}};
        kos_thread_params p = params(task());
        p.caps = two;
        p.cap_count = 1;
        EXPECT_EQ(spawn(p), 0);
        p.cap_count = 2;
        EXPECT_EQ(spawn(p), -KOS_EAGAIN);
    }

    TEST_F(Fake, a_spawn_whose_cores_the_task_s_grant_does_not_meet_is_refused)
    {
        fake::Config c;
        c.multicore = true;
        c.kernel_cores = 4u;
        with(c);
        kos_task_t const t = task();
        ASSERT_EQ(kos_task_sched_grant(t, 0u, 1u << 1), 0);
        kos_thread_params p = params(t);
        p.core_mask = 1u << 2;
        EXPECT_EQ(spawn(p), -KOS_EPERM);
        p.core_mask = 1u << 7;
        EXPECT_EQ(spawn(p), -KOS_EINVAL);
        p.core_mask = (1u << 1) | (1u << 2);
        EXPECT_EQ(spawn(p), 0) << "a partial overlap is intersected";
    }

    TEST_F(Fake, a_claim_past_the_lines_a_kernel_owned_line_and_the_init_s_ceiling_are_refused)
    {
        fake::Config c;
        c.max_irq = 32u;
        c.kernel_lines = {5};
        c.limits.task_lines = 1u;
        with(c);
        kos_cap_t line = KOS_CAP_NONE;
        EXPECT_EQ(kos_irq_claim(32, 0u, &line), -KOS_EINVAL);
        EXPECT_EQ(kos_irq_claim(5, 0u, &line), -KOS_EPERM);
        EXPECT_EQ(kos_irq_claim(6, 0u, &line), 0);
        EXPECT_EQ(kos_irq_claim(7, 0u, &line), -KOS_EAGAIN);
    }

    TEST_F(Fake, above_one_core_a_claim_needs_the_claimer_on_exactly_one_core_of_the_build)
    {
        fake::Config c;
        c.multicore = true;
        c.kernel_cores = 2u;
        with(c);
        kos_cap_t line = KOS_CAP_NONE;
        kos_thread_t const self = kos_thread_self();
        ASSERT_EQ(kos_thread_set_affinity(self, 0x3u), 0);
        EXPECT_EQ(kos_irq_claim(6, 0u, &line), -KOS_EPERM);
        ASSERT_EQ(kos_thread_set_affinity(self, 1u << 4), 0);
        EXPECT_EQ(kos_irq_claim(6, 0u, &line), -KOS_EPERM);
        ASSERT_EQ(kos_thread_set_affinity(self, 1u << 1), 0);
        EXPECT_EQ(kos_irq_claim(6, 0u, &line), 0);
    }

    TEST_F(Fake, a_reservation_comes_back_dirty_with_every_word_odd)
    {
        void* const block = kos_ram_alloc(64u);
        ASSERT_NE(block, nullptr);
        uint32_t const* const words = static_cast<uint32_t const*>(block);
        for (size_t i = 0; i < 64u / sizeof(uint32_t); i++)
        {
            EXPECT_NE(words[i] & 1u, 0u) << "word " << i;
        }
    }

    TEST_F(Fake, a_slay_returns_once_every_member_is_swept)
    {
        kos_task_t const t = task();
        ASSERT_EQ(spawn(params(t)), 0);
        fake::stall("t");
        uint64_t const before = fake::now();
        EXPECT_EQ(kos_task_slay(t, 1000u), -KOS_ETIMEDOUT);
        EXPECT_EQ(fake::now() - before, 1000000u) << "parked to its timeout";
        EXPECT_FALSE(fake::tasks().at(0).released);
        EXPECT_EQ(kos_task_state(t) & KOS_TASK_DEAD, 0);
        fake::sweep("t");
        EXPECT_NE(kos_task_state(t) & KOS_TASK_DEAD, 0);
        EXPECT_EQ(kos_task_slay(t, 1000u), 0);
        EXPECT_TRUE(fake::tasks().at(0).released);
    }

    TEST_F(Fake, a_kill_drops_the_hold_and_the_slot_goes_once_every_member_is_swept)
    {
        fake::Config c;
        c.reuse_handles = true;
        with(c);
        kos_task_t const t = task();
        ASSERT_EQ(spawn(params(t)), 0);
        fake::stall("t");
        EXPECT_EQ(kos_task_kill(t), 0);
        EXPECT_FALSE(fake::tasks().at(0).released) << "a member still sweeping";
        EXPECT_GE(kos_task_state(t), 0) << "resolvable until its slot is freed";
        EXPECT_EQ(kos_task_kill(t), -KOS_EBADF) << "the creator's hold is gone";
        EXPECT_NE(task(), t) << "its handle is not handed out again yet";
        fake::sweep("t");
        EXPECT_TRUE(fake::tasks().at(0).released);
        EXPECT_EQ(kos_task_state(t), -KOS_EBADF);
        EXPECT_EQ(task(), t);
    }

    TEST_F(Fake, a_fresh_endpoint_with_its_creator_seated_parks_a_send_until_its_receiver_waits)
    {
        kos_cap_t ep = KOS_CAP_NONE;
        ASSERT_EQ(kos_endpoint_create(&ep), 0);
        ASSERT_EQ(kos_console_publish(ep), 0);
        kos_cap_grant const wait[] = {{ep, KOS_CAP_WAIT}};
        kos_thread_params p = params(task());
        p.caps = wait;
        p.cap_count = 1;
        ASSERT_EQ(spawn(p), 0);
        ASSERT_EQ(kos_cap_narrow(ep, KOS_CAP_SIGNAL | KOS_CAP_TRANSFER | KOS_CAP_HANDOUT), 0);
        fake::set_console_receives(false);
        uint64_t const before = fake::now();
        EXPECT_EQ(kos_send_timed(KOS_CAP_STDOUT, "x", 1u, 1000u), -KOS_ETIMEDOUT);
        EXPECT_EQ(fake::now() - before, 1000000u);
        EXPECT_THROW(kos_send(KOS_CAP_STDOUT, "x", 1u), fake::Panic);
        fake::set_console_receives(true);
        EXPECT_EQ(kos_send_timed(KOS_CAP_STDOUT, "x", 1u, 1000u), 1);
    }

    TEST_F(Fake, a_console_flush_is_two_zero_length_sends_and_stops_at_a_failed_one)
    {
        kos_cap_t ep = KOS_CAP_NONE;
        ASSERT_EQ(kos_endpoint_create(&ep), 0);
        ASSERT_EQ(kos_console_publish(ep), 0);
        kos_cap_grant const wait[] = {{ep, KOS_CAP_WAIT}};
        kos_thread_params p = params(task());
        p.caps = wait;
        p.cap_count = 1;
        ASSERT_EQ(spawn(p), 0);
        ASSERT_EQ(kos_cap_narrow(ep, KOS_CAP_SIGNAL | KOS_CAP_TRANSFER | KOS_CAP_HANDOUT), 0);
        size_t const before = fake::calls_of("kos_send_timed").size();
        EXPECT_EQ(kos_console_flush(1000u), 0);
        std::vector<fake::Call> sends = fake::calls_of("kos_send_timed");
        ASSERT_EQ(sends.size(), before + 2u);
        for (size_t k = before; k < sends.size(); k++)
        {
            EXPECT_EQ(sends[k].args, (std::vector<uint64_t>{KOS_CAP_STDOUT, 0u, 1000u}));
        }
        fake::set_console_receives(false);
        EXPECT_EQ(kos_console_flush(1000u), -KOS_ETIMEDOUT);
        EXPECT_EQ(fake::calls_of("kos_send_timed").size(), before + 3u);
    }

    TEST_F(Fake, a_vacated_endpoint_answers_at_once_until_its_next_receiver_waits)
    {
        kos_cap_t ep = KOS_CAP_NONE;
        ASSERT_EQ(kos_endpoint_create(&ep), 0);
        ASSERT_EQ(kos_console_publish(ep), 0);
        // The creator's WAIT goes before any receiver is seated.
        ASSERT_EQ(kos_cap_narrow(ep, KOS_CAP_SIGNAL | KOS_CAP_TRANSFER | KOS_CAP_HANDOUT), 0);
        kos_cap_grant const wait[] = {{ep, KOS_CAP_WAIT}};
        kos_thread_params p = params(task());
        p.caps = wait;
        p.cap_count = 1;
        ASSERT_EQ(spawn(p), 0);
        fake::set_console_receives(false);
        uint64_t const before = fake::now();
        EXPECT_EQ(kos_send_timed(KOS_CAP_STDOUT, "x", 1u, 1000u), -KOS_EAGAIN);
        EXPECT_EQ(kos_send(KOS_CAP_STDOUT, "x", 1u), -KOS_EAGAIN);
        EXPECT_EQ(fake::now(), before);
        fake::set_console_receives(true);
        EXPECT_EQ(kos_send_timed(KOS_CAP_STDOUT, "x", 1u, 1000u), 1);

        // The receiver dies, and the next one seated has not waited yet.
        fake::die("t");
        EXPECT_EQ(kos_send_timed(KOS_CAP_STDOUT, "x", 1u, 1000u), -KOS_EAGAIN) << "a HANDOUT holder remains";
        kos_thread_params q = params(task());
        q.caps = wait;
        q.cap_count = 1;
        ASSERT_EQ(spawn(q), 0);
        fake::set_console_receives(false);
        EXPECT_EQ(kos_send_timed(KOS_CAP_STDOUT, "x", 1u, 1000u), -KOS_EAGAIN);
        EXPECT_EQ(fake::now(), before);
        fake::set_console_receives(true);
        EXPECT_EQ(kos_send_timed(KOS_CAP_STDOUT, "x", 1u, 1000u), 1);
        fake::set_console_receives(false);
        EXPECT_EQ(kos_send_timed(KOS_CAP_STDOUT, "x", 1u, 1000u), -KOS_ETIMEDOUT);
        EXPECT_EQ(fake::now() - before, 1000000u);
        EXPECT_THROW(kos_send(KOS_CAP_STDOUT, "x", 1u), fake::Panic);
    }

    TEST_F(Fake, a_walk_that_never_stops_calling_fails_rather_than_hangs)
    {
        kos_cap_t note = KOS_CAP_NONE;
        ASSERT_EQ(kos_notify_create(&note), 0);
        ASSERT_EQ(kos_notify_bind(note), 0);
        std::string message;
        try
        {
            while (true)
            {
                uint32_t bits = 0;
                (void)kos_notify_wait(note, 1u, 1000u, &bits);
            }
        }
        catch (fake::Panic const& panic)
        {
            message = panic.message;
        }
        EXPECT_EQ(message, "fake: runaway");
    }

    TEST_F(Fake, a_grant_past_the_init_s_own_ceiling_or_naming_no_core_of_the_build_is_refused)
    {
        fake::Config c;
        c.init_ceiling = 20u;
        with(c);
        kos_task_t const t = task();
        EXPECT_EQ(kos_task_sched_grant(t, 21u, 0u), -KOS_EPERM);
        EXPECT_EQ(kos_task_sched_grant(t, 0u, 1u << 3), -KOS_EINVAL) << "one kernel core";
        EXPECT_EQ(kos_task_sched_grant(t, 20u, 1u), 0);
    }
}
