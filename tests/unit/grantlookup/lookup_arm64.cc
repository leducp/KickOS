// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The lookups against qemu-arm64.yaml with the sensor taking the RTC's alarm line and health
// watching app then sensor, and a two-page /shm/log declared ahead of /shm/history: sensor holds
// the RTC window at place 0, the alarm at slot 0, its served endpoint at slot 1, the history at
// place 1 and the log at place 2; health holds /init/events at slot 0, the status block at place
// 0 and the history at place 1.

#include "lookup_seam.h"

#include <kickos/sys.h>
#include <kickos/sys/errno.h>
#include <kickos/sys/init_status.h>
#include <kickos/sys/task_trampoline.h>

#include <gtest/gtest.h>

#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

namespace
{
    constexpr kos_cap_t CAP0 = KOS_SPAWN_DELEGATED_CAP0;
    constexpr uint32_t PAGE = 0x1000u;
    // Where the sensor's space maps the RTC, which is not its physical base 0x9010000.
    constexpr uintptr_t RTC_MAPPED = 0x7000000u;

    alignas(4096) unsigned char g_history[PAGE];
    alignas(4096) unsigned char g_log[2u * PAGE];

    uintptr_t history()
    {
        return reinterpret_cast<uintptr_t>(g_history);
    }

    void expect_no_window(kos_window_t w)
    {
        EXPECT_EQ(kos_window_addr(w), nullptr);
        EXPECT_EQ(kos_window_size(w), 0u);
    }

    class Lookup : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            seam::reset();
        }

        void as_sensor()
        {
            seam::windows = {seam::window(RTC_MAPPED, PAGE, KOS_WINDOW_DEVICE, 0),
                             seam::window(history(), PAGE, KOS_WINDOW_MEMORY, 0),
                             seam::window(reinterpret_cast<uintptr_t>(g_log), 2u * PAGE, KOS_WINDOW_MEMORY, 0)};
        }

        void as_health(seam::StatusBlock const& block)
        {
            seam::windows = {seam::window(block.base(), PAGE, KOS_WINDOW_MEMORY, KOS_WINDOW_RO),
                             seam::window(history(), PAGE, KOS_WINDOW_MEMORY, KOS_WINDOW_RO)};
        }
    };

    TEST_F(Lookup, an_endpoint_is_found_served_and_used_at_its_slot)
    {
        EXPECT_EQ(kos_grant_endpoint(seam::task("sensor"), "/svc/sensor"), CAP0 + 1u);
        EXPECT_EQ(kos_grant_endpoint(seam::task("app"), "/svc/sensor"), CAP0 + 0u);
        EXPECT_TRUE(seam::asked.empty());
    }

    TEST_F(Lookup, a_notification_is_found_at_its_slot)
    {
        EXPECT_EQ(kos_grant_notify(seam::task("health"), "/init/events"), CAP0 + 0u);
    }

    TEST_F(Lookup, a_device_window_is_where_the_kernel_answers_it)
    {
        as_sensor();
        kos_window_t const rtc = kos_grant_mmio(seam::task("sensor"), "/dev/rtc");
        EXPECT_EQ(kos_window_addr(rtc), reinterpret_cast<void*>(RTC_MAPPED));
        EXPECT_EQ(kos_window_size(rtc), PAGE);
        EXPECT_EQ(seam::asked, std::vector<uint32_t>{0u});
    }

    TEST_F(Lookup, a_region_is_asked_at_its_window_place)
    {
        as_sensor();
        kos_window_t const mine = kos_grant_mem(seam::task("sensor"), "/shm/history");
        EXPECT_EQ(kos_window_addr(mine), g_history);
        EXPECT_EQ(kos_window_size(mine), PAGE);
        EXPECT_EQ(seam::asked, std::vector<uint32_t>{1u});
    }

    TEST_F(Lookup, each_region_is_its_own_size)
    {
        as_sensor();
        kos_window_t const log = kos_grant_mem(seam::task("sensor"), "/shm/log");
        EXPECT_EQ(kos_window_addr(log), g_log);
        EXPECT_EQ(kos_window_size(log), 2u * PAGE);
        EXPECT_EQ(kos_window_size(kos_grant_mem(seam::task("sensor"), "/shm/history")), PAGE);
        EXPECT_EQ(seam::asked, (std::vector<uint32_t>{2u, 1u}));
    }

    TEST_F(Lookup, a_watchers_region_sits_after_its_status_block)
    {
        seam::StatusBlock block(PAGE);
        as_health(block);
        kos_window_t const theirs = kos_grant_mem(seam::task("health"), "/shm/history");
        EXPECT_EQ(kos_window_addr(theirs), g_history);
        EXPECT_EQ(seam::asked, std::vector<uint32_t>{1u});
    }

    TEST_F(Lookup, a_line_is_found_by_the_role_its_entry_renamed_it_to)
    {
        kos_line_t const alarm = kos_grant_irq(seam::task("sensor"), "alarm");
        EXPECT_EQ(alarm.cap, CAP0 + 0u);
        EXPECT_EQ(alarm.index, 0u);
        kos_line_t const by_path = kos_grant_irq(seam::task("sensor"), "/dev/rtc/alarm");
        EXPECT_EQ(by_path.cap, KOS_CAP_NONE);
        EXPECT_EQ(by_path.index, KOS_TABLE_NONE);
    }

    TEST_F(Lookup, a_name_of_another_kind_answers_invalid)
    {
        as_sensor();
        kos_self_t const* const sensor = seam::task("sensor");
        EXPECT_EQ(kos_grant_endpoint(sensor, "/shm/history"), KOS_CAP_NONE);
        EXPECT_EQ(kos_grant_endpoint(sensor, "alarm"), KOS_CAP_NONE);
        EXPECT_EQ(kos_grant_notify(sensor, "/svc/sensor"), KOS_CAP_NONE);
        expect_no_window(kos_grant_mmio(sensor, "/shm/history"));
        expect_no_window(kos_grant_mem(sensor, "/dev/rtc"));
        expect_no_window(kos_grant_ports(sensor, "/dev/rtc"));
        EXPECT_EQ(kos_grant_irq(sensor, "/svc/sensor").cap, KOS_CAP_NONE);
        EXPECT_EQ(kos_grant_irq(sensor, "/svc/sensor").index, KOS_TABLE_NONE);

        seam::StatusBlock block(PAGE);
        as_health(block);
        expect_no_window(kos_grant_mem(seam::task("health"), "/init/status"));
        EXPECT_EQ(kos_grant_endpoint(seam::task("health"), "/init/events"), KOS_CAP_NONE);
    }

    TEST_F(Lookup, an_absent_name_answers_invalid)
    {
        as_sensor();
        kos_self_t const* const app = seam::task("app");
        EXPECT_EQ(kos_grant_endpoint(app, "/svc/sensors"), KOS_CAP_NONE);
        EXPECT_EQ(kos_grant_endpoint(app, "/svc/sens"), KOS_CAP_NONE);
        EXPECT_EQ(kos_grant_endpoint(app, ""), KOS_CAP_NONE);
        EXPECT_EQ(kos_grant_endpoint(app, nullptr), KOS_CAP_NONE);
        EXPECT_EQ(kos_grant_notify(app, "/init/events"), KOS_CAP_NONE);
        expect_no_window(kos_grant_mmio(app, "/dev/rtc"));
        expect_no_window(kos_grant_mem(app, "/shm/history"));
        EXPECT_EQ(kos_grant_irq(app, "alarm").cap, KOS_CAP_NONE);
        EXPECT_TRUE(seam::asked.empty());
    }

    TEST_F(Lookup, a_null_self_answers_invalid)
    {
        as_sensor();
        EXPECT_EQ(kos_grant_endpoint(nullptr, "/svc/sensor"), KOS_CAP_NONE);
        EXPECT_EQ(kos_grant_notify(nullptr, "/init/events"), KOS_CAP_NONE);
        expect_no_window(kos_grant_mmio(nullptr, "/dev/rtc"));
        expect_no_window(kos_grant_mem(nullptr, "/shm/history"));
        expect_no_window(kos_grant_ports(nullptr, "/dev/rtc"));
        kos_line_t const line = kos_grant_irq(nullptr, "alarm");
        EXPECT_EQ(line.cap, KOS_CAP_NONE);
        EXPECT_EQ(line.index, KOS_TABLE_NONE);
        struct kos_task_status st;
        EXPECT_EQ(kos_task_status(nullptr, 0, &st), -KOS_EINVAL);
        EXPECT_TRUE(seam::asked.empty());
    }

    TEST_F(Lookup, a_window_the_thread_does_not_hold_answers_invalid)
    {
        seam::windows = {seam::window(RTC_MAPPED, PAGE, KOS_WINDOW_DEVICE, 0)};
        seam::stale = seam::window(history(), PAGE, KOS_WINDOW_MEMORY, 0);
        expect_no_window(kos_grant_mem(seam::task("sensor"), "/shm/history"));
        seam::windows.clear();
        seam::stale = seam::window(RTC_MAPPED, PAGE, KOS_WINDOW_DEVICE, 0);
        expect_no_window(kos_grant_mmio(seam::task("sensor"), "/dev/rtc"));
    }

    TEST_F(Lookup, a_window_of_another_kind_answers_invalid)
    {
        as_sensor();
        seam::windows[0].kind = KOS_WINDOW_MEMORY;
        seam::windows[1].kind = KOS_WINDOW_DEVICE;
        expect_no_window(kos_grant_mmio(seam::task("sensor"), "/dev/rtc"));
        expect_no_window(kos_grant_mem(seam::task("sensor"), "/shm/history"));
    }

    TEST_F(Lookup, a_window_with_other_flags_answers_invalid)
    {
        as_sensor();
        seam::windows[1].flags = KOS_WINDOW_RO;
        expect_no_window(kos_grant_mem(seam::task("sensor"), "/shm/history"));
        seam::windows[0].flags = KOS_WINDOW_UNCACHED;
        expect_no_window(kos_grant_mmio(seam::task("sensor"), "/dev/rtc"));

        seam::StatusBlock block(PAGE);
        as_health(block);
        seam::windows[1].flags = 0;
        expect_no_window(kos_grant_mem(seam::task("health"), "/shm/history"));
        as_health(block);
        seam::windows[0].flags = 0;
        struct kos_task_status st;
        EXPECT_EQ(kos_task_status(seam::task("health"), 0, &st), -KOS_EINVAL);
    }

    TEST_F(Lookup, a_window_of_another_size_answers_invalid)
    {
        as_sensor();
        seam::windows[0].size = 2u * PAGE;
        seam::windows[1].size = PAGE / 2u;
        expect_no_window(kos_grant_mmio(seam::task("sensor"), "/dev/rtc"));
        expect_no_window(kos_grant_mem(seam::task("sensor"), "/shm/history"));
    }

    // ---------------------------------------------------------------------------------------
    // The status read

    void write(seam::StatusBlock& block, char const* task, uint16_t deaths, uint8_t restarts_left,
               uint8_t state)
    {
        kickos::init::status_write(block.record(seam::watched_at("health", task)),
                                   kickos::init::StatusFields{deaths, restarts_left, state});
    }

    TEST_F(Lookup, a_stable_record_is_the_watched_tasks_own)
    {
        seam::StatusBlock block(PAGE);
        as_health(block);
        write(block, "sensor", 1, 2, kickos::init::STATUS_ALIVE);
        write(block, "app", 4, 0, kickos::init::STATUS_DEPENDENCY_DOWN);

        struct kos_task_status st;
        ASSERT_EQ(kos_task_status(seam::task("health"), 0, &st), 0);
        EXPECT_STREQ(st.name, "app");
        EXPECT_FALSE(st.alive);
        EXPECT_EQ(st.deaths, 4u);
        EXPECT_EQ(st.restarts_left, 0u);
        EXPECT_TRUE(st.dependency_down);

        ASSERT_EQ(kos_task_status(seam::task("health"), 1, &st), 0);
        EXPECT_STREQ(st.name, "sensor");
        EXPECT_TRUE(st.alive);
        EXPECT_EQ(st.deaths, 1u);
        EXPECT_EQ(st.restarts_left, 2u);
        EXPECT_FALSE(st.dependency_down);

        EXPECT_TRUE(seam::sleeps.empty());
        EXPECT_EQ(seam::asked, (std::vector<uint32_t>{0u, 0u}));
    }

    TEST_F(Lookup, past_the_last_watched_task_answers_einval)
    {
        seam::StatusBlock block(PAGE);
        as_health(block);
        struct kos_task_status st;
        EXPECT_EQ(kos_task_status(seam::task("health"), 2, &st), -KOS_EINVAL);
        EXPECT_EQ(kos_task_status(seam::task("sensor"), 0, &st), -KOS_EINVAL);
        EXPECT_EQ(kos_task_status(seam::task("health"), 0, nullptr), -KOS_EINVAL);
    }

    TEST_F(Lookup, a_status_block_not_held_as_granted_answers_einval)
    {
        seam::StatusBlock block(PAGE);
        struct kos_task_status st;
        EXPECT_EQ(kos_task_status(seam::task("health"), 0, &st), -KOS_EINVAL);
        as_health(block);
        seam::windows[0].size = 2u * PAGE;
        EXPECT_EQ(kos_task_status(seam::task("health"), 0, &st), -KOS_EINVAL);
        as_health(block);
        seam::windows[0].kind = KOS_WINDOW_DEVICE;
        EXPECT_EQ(kos_task_status(seam::task("health"), 0, &st), -KOS_EINVAL);
    }

    TEST_F(Lookup, a_record_held_odd_answers_eagain_after_a_hundred_sleeps)
    {
        seam::StatusBlock block(PAGE);
        as_health(block);
        block.record(seam::watched_at("health", "sensor"))->count = 1u;
        block.record(seam::watched_at("health", "sensor"))->deaths = 5u;
        struct kos_task_status st;
        memset(&st, 0xA5, sizeof(st));
        struct kos_task_status untouched;
        memcpy(&untouched, &st, sizeof(st));
        EXPECT_EQ(kos_task_status(seam::task("health"), 1, &st), -KOS_EAGAIN);
        EXPECT_EQ(memcmp(&st, &untouched, sizeof(st)), 0);
        EXPECT_EQ(seam::sleeps, std::vector<uint64_t>(100u, 1000000u));
    }

    TEST_F(Lookup, a_record_going_even_within_the_bound_is_answered)
    {
        seam::StatusBlock block(PAGE);
        as_health(block);
        kickos::init::StatusRecord* const sensor = block.record(seam::watched_at("health", "sensor"));
        sensor->count = 1u;
        sensor->deaths = 3u;
        seam::on_sleep = [&]()
        {
            if (seam::sleeps.size() == 100u)
            {
                sensor->restarts_left = 7u;
                sensor->state = kickos::init::STATUS_ALIVE;
                sensor->count = 2u;
            }
        };
        struct kos_task_status st;
        ASSERT_EQ(kos_task_status(seam::task("health"), 1, &st), 0);
        EXPECT_EQ(st.deaths, 3u);
        EXPECT_EQ(st.restarts_left, 7u);
        EXPECT_TRUE(st.alive);
        EXPECT_EQ(seam::sleeps.size(), 100u);
    }

    // A write landing between the reader's two loads of the count: the block is unreadable until
    // the first load faults, which opens it and single-steps that one load; the trap after it
    // starts a write and stores the deaths alone, and the next sleep finishes the write. It holds
    // only while that load is the read's first access to the block.
#if defined(__x86_64__) && defined(__linux__)
    constexpr long long TRAP_FLAG = 0x100;
    seam::StatusBlock* g_torn_block;
    kickos::init::StatusRecord* g_torn;

    void on_fault(int, siginfo_t*, void* context)
    {
        mprotect(reinterpret_cast<void*>(g_torn_block->base()), g_torn_block->size(), PROT_READ | PROT_WRITE);
        static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_EFL] |= TRAP_FLAG;
    }

    void on_step(int, siginfo_t*, void* context)
    {
        g_torn->count = 3u;
        g_torn->deaths = 9u;
        static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_EFL] &= ~TRAP_FLAG;
    }
#endif

    TEST_F(Lookup, a_write_between_the_two_loads_is_read_again)
    {
#if defined(__x86_64__) && defined(__linux__)
        seam::StatusBlock block(PAGE);
        as_health(block);
        g_torn_block = &block;
        g_torn = block.record(seam::watched_at("health", "sensor"));
        kickos::init::status_write(g_torn, kickos::init::StatusFields{1, 2, kickos::init::STATUS_ALIVE});
        seam::on_sleep = [&]()
        {
            g_torn->restarts_left = 1u;
            g_torn->count = 4u;
        };

        struct sigaction fault = {};
        fault.sa_sigaction = on_fault;
        fault.sa_flags = SA_SIGINFO;
        struct sigaction step = fault;
        step.sa_sigaction = on_step;
        struct sigaction old_fault;
        struct sigaction old_step;
        ASSERT_EQ(sigaction(SIGSEGV, &fault, &old_fault), 0);
        ASSERT_EQ(sigaction(SIGTRAP, &step, &old_step), 0);
        ASSERT_EQ(mprotect(reinterpret_cast<void*>(block.base()), block.size(), PROT_NONE), 0);

        struct kos_task_status st;
        int const rc = kos_task_status(seam::task("health"), 1, &st);

        sigaction(SIGSEGV, &old_fault, nullptr);
        sigaction(SIGTRAP, &old_step, nullptr);
        ASSERT_EQ(rc, 0);
        EXPECT_EQ(st.deaths, 9u);
        EXPECT_EQ(st.restarts_left, 1u);
        EXPECT_EQ(seam::sleeps.size(), 1u);
#else
        GTEST_SKIP() << "single-steps one load, which this host's arm does on x86_64 Linux alone";
#endif
    }

    // ---------------------------------------------------------------------------------------
    // The trampoline

    TEST_F(Lookup, the_trampoline_exits_zero_when_the_entry_returns)
    {
        kos_self_t const* const app = seam::task("app");
        int code = -1;
        try
        {
            kickos::init::task_trampoline(const_cast<kos_self_t*>(app));
        }
        catch (seam::Exit const& exit)
        {
            code = exit.code;
        }
        EXPECT_EQ(seam::entered, app);
        EXPECT_EQ(code, 0);
    }

    TEST_F(Lookup, the_trampoline_flushes_what_a_returning_entry_printed)
    {
        fflush(stdout);
        int const console = dup(STDOUT_FILENO);
        FILE* const sink = tmpfile();
        ASSERT_NE(sink, nullptr);
        ASSERT_GE(dup2(fileno(sink), STDOUT_FILENO), 0);
        seam::entry_print = "unterminated";
        try
        {
            kickos::init::task_trampoline(const_cast<kos_self_t*>(seam::task("app")));
        }
        catch (seam::Exit const&)
        {
        }
        char got[32] = {};
        ssize_t const n = pread(fileno(sink), got, sizeof(got) - 1u, 0);
        dup2(console, STDOUT_FILENO);
        close(console);
        fclose(sink);
        EXPECT_EQ(n, 12);
        EXPECT_STREQ(got, "unterminated");
    }

    TEST_F(Lookup, the_trampoline_keeps_the_code_the_entry_exits_with)
    {
        seam::entry_exit = 5;
        int code = -1;
        try
        {
            kickos::init::task_trampoline(const_cast<kos_self_t*>(seam::task("sensor")));
        }
        catch (seam::Exit const& exit)
        {
            code = exit.code;
        }
        EXPECT_EQ(seam::entered, seam::task("sensor"));
        EXPECT_EQ(code, 5);
    }
}
