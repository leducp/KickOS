// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The stdout policy against a scripted kernel. Each send and each kernel console write takes
// the next answer of its own script; an answer that accepts bytes records them on that route.
// The short-accept arms run the endpoint's rendezvous instead of a send script.

#include <gtest/gtest.h>

#include <csetjmp>
#include <deque>
#include <string>

#include <kickos/sys.h>
#include <kickos/sys/emit.h>

namespace
{
    // 0 or more: that many bytes are taken, the whole offer where it exceeds it. Negative: the
    // answer itself.
    constexpr int32_t TAKE_ALL = 1 << 30;

    // The endpoint as the kernel runs it, where a test turns it on: a send completes only
    // against a posted receive and consumes it, and with none posted the writer parks until the
    // driver runs and posts the next capacity of its script. A script run dry leaves the writer
    // parked for good.
    bool g_rendezvous = false;
    std::deque<size_t> g_receives;
    bool g_posted = false;
    size_t g_posted_cap = 0;
    int g_parks = 0;
    std::jmp_buf g_parked_for_good;

    std::deque<int32_t> g_send_script;
    std::deque<int32_t> g_kconsole_script;
    std::string g_endpoint;
    std::string g_kconsole;
    int g_sends = 0;
    int g_kconsole_writes = 0;
    int g_closes = 0;

    int32_t next(std::deque<int32_t>& script)
    {
        EXPECT_FALSE(script.empty()) << "the writer asked more than the script answers";
        if (script.empty())
        {
            return -KOS_EINVAL;
        }
        int32_t const a = script.front();
        script.pop_front();
        return a;
    }

    int32_t take(std::deque<int32_t>& script, std::string& route, void const* buf, size_t len)
    {
        int32_t const a = next(script);
        if (a < 0)
        {
            return a;
        }
        size_t n = len;
        if (static_cast<size_t>(a) < n)
        {
            n = static_cast<size_t>(a);
        }
        route.append(static_cast<char const*>(buf), n);
        return static_cast<int32_t>(n);
    }

    void reset(std::deque<int32_t> sends, std::deque<int32_t> kconsole)
    {
        g_send_script = std::move(sends);
        g_kconsole_script = std::move(kconsole);
        g_endpoint.clear();
        g_kconsole.clear();
        g_sends = 0;
        g_kconsole_writes = 0;
        g_closes = 0;
        kickos::emit_detail::g_dropped = 0;
        g_rendezvous = false;
    }

    // The driver has posted the first capacity of `receives` before the writer starts.
    void reset_rendezvous(std::deque<size_t> receives)
    {
        reset({}, {});
        g_rendezvous = true;
        g_receives = std::move(receives);
        g_posted = false;
        g_parks = 0;
        if (not g_receives.empty())
        {
            g_posted_cap = g_receives.front();
            g_receives.pop_front();
            g_posted = true;
        }
    }

    int32_t rendezvous(void const* buf, size_t len)
    {
        if (not g_posted)
        {
            g_parks = g_parks + 1;
            if (g_receives.empty())
            {
                std::longjmp(g_parked_for_good, 1);
            }
            g_posted_cap = g_receives.front();
            g_receives.pop_front();
        }
        g_posted = false;
        size_t n = len;
        if (g_posted_cap < n)
        {
            n = g_posted_cap;
        }
        g_endpoint.append(static_cast<char const*>(buf), n);
        return static_cast<int32_t>(n);
    }
}

extern "C"
{
    int32_t kos_send(kos_cap_t ep, void const* buf, size_t len)
    {
        EXPECT_EQ(ep, KOS_CAP_STDOUT);
        g_sends = g_sends + 1;
        if (g_rendezvous)
        {
            return rendezvous(buf, len);
        }
        return take(g_send_script, g_endpoint, buf, len);
    }

    int32_t kos_kconsole_write(void const* buf, size_t len)
    {
        g_kconsole_writes = g_kconsole_writes + 1;
        return take(g_kconsole_script, g_kconsole, buf, len);
    }

    int kos_handle_close(kos_cap_t)
    {
        g_closes = g_closes + 1;
        return 0;
    }

    uint64_t kos_clock_now(void)
    {
        return 1;
    }

    void kos_yield(void)
    {
    }
}

namespace
{
    // The send finds the endpoint vacated, and the restart's publish lands before the kernel
    // console is asked: the line goes back to the endpoint, once.
    TEST(StdoutWrite, ALineRefusedAcrossARestartIsSentAgain)
    {
        reset({-KOS_EAGAIN, TAKE_ALL}, {-KOS_EBUSY});
        kickos::emit("conwit: line 6 out\n");
        EXPECT_EQ(g_endpoint, "conwit: line 6 out\n");
        EXPECT_EQ(g_kconsole, "");
        EXPECT_EQ(g_sends, 2);
        EXPECT_EQ(g_kconsole_writes, 1);
        EXPECT_EQ(kickos::emit_detail::g_dropped, 0u);
        EXPECT_TRUE(g_send_script.empty());
    }

    // Anti-vacuity for the arm above: with no publish in between, the kernel console carries
    // the line and the endpoint is not asked again.
    TEST(StdoutWrite, ALineRefusedBeforeAnyRestartGoesToTheKernelConsole)
    {
        reset({-KOS_EAGAIN}, {TAKE_ALL});
        kickos::emit("conwit: line 6 out\n");
        EXPECT_EQ(g_kconsole, "conwit: line 6 out\n");
        EXPECT_EQ(g_endpoint, "");
        EXPECT_EQ(g_sends, 1);
    }

    // Only the remainder travels again, and a chunk the endpoint took is never repeated.
    TEST(StdoutWrite, OnlyTheRemainderIsSentAgain)
    {
        std::string line(KOS_EP_MSG_MAX + 10u, 'x');
        line.back() = '\n';
        reset({TAKE_ALL, -KOS_EAGAIN, TAKE_ALL}, {-KOS_EBUSY});
        kickos::stdout_write(line.data(), line.size());
        EXPECT_EQ(g_endpoint, line);
        EXPECT_EQ(g_kconsole, "");
        EXPECT_EQ(g_sends, 3);
    }

    // A receiver that accepts fewer bytes than offered, none included, leaves the rest to be
    // offered again on the endpoint, and the kernel console is never asked. Every offer after
    // the first parks until the driver receives again: one send per receive, never more.
    TEST(StdoutWrite, AShortAcceptIsOfferedAgainOnTheNextReceive)
    {
        constexpr size_t ZEROS = 1000;
        std::deque<size_t> receives(ZEROS, 0);
        receives.push_back(2);
        receives.push_back(64);
        reset_rendezvous(receives);
        if (setjmp(g_parked_for_good) == 0)
        {
            kickos::emit("line\n");
        }
        EXPECT_EQ(g_endpoint, "line\n");
        EXPECT_EQ(g_sends, static_cast<int>(ZEROS) + 2);
        EXPECT_EQ(g_parks, g_sends - 1);
        EXPECT_TRUE(g_receives.empty());
        EXPECT_EQ(g_kconsole_writes, 0);
        EXPECT_EQ(kickos::emit_detail::g_dropped, 0u);
    }

    // A driver that only ever accepts nothing keeps the writer parked on its endpoint, one send
    // per receive, with no fallback to the kernel console.
    TEST(StdoutWrite, AReceiverTakingNothingParksTheWriter)
    {
        constexpr size_t ZEROS = 1000;
        reset_rendezvous(std::deque<size_t>(ZEROS, 0));
        bool parked = false;
        if (setjmp(g_parked_for_good) == 0)
        {
            kickos::emit("line\n");
        }
        else
        {
            parked = true;
        }
        EXPECT_TRUE(parked) << "the writer gave the line up on a live endpoint";
        EXPECT_EQ(g_sends, static_cast<int>(ZEROS) + 1);
        EXPECT_EQ(g_parks, static_cast<int>(ZEROS));
        EXPECT_EQ(g_endpoint, "");
        EXPECT_EQ(g_kconsole_writes, 0);
    }

    // An empty line is no chunk at all, so nothing is sent: a zero-length send is the flush.
    TEST(StdoutWrite, AnEmptyLineSendsNothing)
    {
        reset({}, {});
        kickos::stdout_write("", 0);
        EXPECT_EQ(g_sends, 0);
        EXPECT_EQ(g_kconsole_writes, 0);
    }

    // A driver that cannot come back closes the stdout capability before the fallback.
    TEST(StdoutWrite, ARefusedConnectionClosesStdout)
    {
        reset({-KOS_ECONNREFUSED}, {TAKE_ALL});
        kickos::emit("line\n");
        EXPECT_EQ(g_closes, 1);
        EXPECT_EQ(g_kconsole, "line\n");
    }

    // A writer going straight to the kernel console is handed back to its endpoint too.
    TEST(StdoutWrite, KconsoleWriteAllSendsAServedRefusalToStdout)
    {
        reset({TAKE_ALL}, {-KOS_EBUSY});
        kickos::kconsole_write_all("init line\n", 10);
        EXPECT_EQ(g_endpoint, "init line\n");
        EXPECT_EQ(kickos::emit_detail::g_dropped, 0u);
        EXPECT_EQ(g_kconsole_writes, 1);
    }
}
