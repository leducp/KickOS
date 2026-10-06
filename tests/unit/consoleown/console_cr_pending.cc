// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A user writer's half-sent CR across the kernel console's two refusals, at an own-image AMP
// node's posture, where the writer carries it from one offer to the next.

#include <gtest/gtest.h>

#include <kickos/arch/arch.h>
#include <kickos/cap.h>
#include <kickos/console_tx.h>
#include <kickos/kernel.h>
#include <kickos/sched.h>
#include <kickos/sys/errno.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

namespace
{
    int g_pokes = 0;
    int g_reclaims = 0;
    bool g_window_free = true;
    bool g_serves = false;
    kickos::Thread g_writer;
}

extern "C"
{
    arch_irq_state_t arch_irq_save(void) { return 0; }
    void arch_irq_restore(arch_irq_state_t) {}
    int arch_in_isr(void) { return 0; }

    int arch_console_write(char const*, size_t n)
    {
        g_pokes = g_pokes + 1;
        return static_cast<int>(n);
    }
    int arch_console_write_retry(char const*, size_t n, bool* cr_pending)
    {
        g_pokes = g_pokes + 1;
        *cr_pending = false;
        return static_cast<int>(n);
    }
    void arch_console_write_sync(char const*, size_t) { g_pokes = g_pokes + 1; }
    void arch_console_flush_sync(void) {}
    void arch_console_reclaim(void) { g_reclaims = g_reclaims + 1; }
    void arch_console_reclaim_window(uintptr_t* base, size_t* size)
    {
        *base = 0x40000000u;
        *size = 0x100u;
    }

    int console_tx_armed(void) { return 0; }
    void console_tx_flush_sync(void) {}
    void console_tx_deinit(void) {}

    int kvsnprintf(char* buf, size_t size, char const* fmt, va_list ap)
    {
        return vsnprintf(buf, size, fmt, ap);
    }

    void arch_shutdown(int status)
    {
        printf("FIXTURE FAIL: arch_shutdown(%d) ended the arm\n", status);
        exit(1);
    }
    int arch_reboot(void) { return 0; }
    void kfault_terminate(void)
    {
        printf("FIXTURE FAIL: kfault_terminate ended the arm\n");
        exit(1);
    }
    void kickos_panic_stack_enter(char const* msg, char const* file, unsigned line,
                                  uintptr_t top)
    {
        (void)top;
        kickos_panic_report(msg, file, line);
    }
}

namespace kickos
{
    bool dev_window_free(uintptr_t, size_t) { return g_window_free; }

    int32_t cap_console_deliver(char const*, size_t) { return 0; }

    bool cap_console_serves(Thread const*) { return g_serves; }

    namespace sched
    {
        Thread* current() { return &g_writer; }
    }
}

namespace
{
    // The ownership state never returns to KERNEL_OWNED, so each arm gets its own process.
    void run_isolated(void (*body)())
    {
        fflush(nullptr);
        pid_t const pid = fork();
        ASSERT_NE(pid, -1) << "fork failed, the arm did not run";
        if (pid == 0)
        {
            body();
            fflush(nullptr);
            if (::testing::Test::HasFailure())
            {
                _exit(1);
            }
            _exit(0);
        }
        int status = 0;
        ASSERT_EQ(waitpid(pid, &status, 0), pid);
        ASSERT_TRUE(WIFEXITED(status)) << "the arm died on a signal";
        ASSERT_EQ(WEXITSTATUS(status), 0) << "see the arm's own failure text above";
    }

    // The retry offers the same newline to the kernel console again, so the CR already on the
    // wire is still owed and the newline goes out alone.
    TEST(ConsoleCrPending, ARetryKeepsAHalfSentCr)
    {
        run_isolated([]() {
            console_owner_set_user();
            console_note_driver_death();
            g_window_free = false;
            console_on_driver_death();
            ASSERT_EQ(g_reclaims, 0) << "the reclaim did not defer, so no retry is asked";
            g_writer.console_cr_pending = 1;
            EXPECT_EQ(kickos::kconsole_write_user("\n", 1), -KOS_EAGAIN);
            EXPECT_EQ(g_writer.console_cr_pending, 1u) << "the retry would send a second CR";
            EXPECT_EQ(g_pokes, 0);
        });
    }

    // The rest of the line leaves for the endpoint, whose driver ends it itself: an owed CR
    // kept here would put a stray newline ahead of this thread's next kernel console line.
    TEST(ConsoleCrPending, AHandBackDropsAHalfSentCr)
    {
        run_isolated([]() {
            console_owner_set_user();
            g_serves = true;
            g_writer.console_cr_pending = 1;
            EXPECT_EQ(kickos::kconsole_write_user("\n", 1), -KOS_EBUSY);
            EXPECT_EQ(g_writer.console_cr_pending, 0u);
            EXPECT_EQ(g_pokes, 0);
        });
    }
}
