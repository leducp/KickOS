// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The stdout policy, and kos_print over it, against a scripted kernel. Each send and each kernel
// console write takes the next answer of its own script; an answer that accepts bytes records
// them on that route.
// The short-accept arms run the endpoint's rendezvous instead of a send script. A non-blocking
// task's kernel answers -KOS_ETIMEDOUT where a blocking one would wait. libc's _write is the
// stubs' own body, compiled in under the shim's names.
// The stubs' fstat: newlib reads st_mode to decide line buffering and st_blksize as a buffer
// size, so a field left as the caller's stack bytes makes stdout's buffering vary.

#include <gtest/gtest.h>

#include <cerrno>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

#include <sys/stat.h>

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
    int g_waiting_sends = 0;    // sends that could park
    int g_waiting_kconsole = 0; // kernel console writes that could wait

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
        g_waiting_sends = 0;
        g_waiting_kconsole = 0;
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

    void expect_result(kickos::WriteResult r, size_t sent, int32_t error)
    {
        EXPECT_EQ(r.sent, sent);
        EXPECT_EQ(r.error, error);
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
        g_waiting_sends = g_waiting_sends + 1;
        if (g_rendezvous)
        {
            return rendezvous(buf, len);
        }
        return take(g_send_script, g_endpoint, buf, len);
    }

    int32_t kos_send_timed(kos_cap_t ep, void const* buf, size_t len, uint32_t timeout_us)
    {
        EXPECT_EQ(ep, KOS_CAP_STDOUT);
        g_sends = g_sends + 1;
        if (timeout_us != 0u)
        {
            g_waiting_sends = g_waiting_sends + 1;
        }
        return take(g_send_script, g_endpoint, buf, len);
    }

    int32_t kos_kconsole_write(void const* buf, size_t len)
    {
        g_kconsole_writes = g_kconsole_writes + 1;
        g_waiting_kconsole = g_waiting_kconsole + 1;
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

    void kos_exit(int)
    {
        abort();
    }

    int _write(int fd, char const* buf, int len);
    int _fstat(int fd, struct stat* st);
    int kos_ut_plain_fstat(int fd, struct stat* st);
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

    // The kernel console waits for room itself, so a writer never offers a line it refused with
    // nothing taken again: a try-again ends the write instead of spinning on it.
    TEST(StdoutWrite, AKernelConsoleTakingNothingIsNotOfferedTheLineAgain)
    {
        reset({-KOS_EBADF}, {-KOS_EAGAIN});
        kickos::emit("line\n");
        EXPECT_EQ(g_kconsole_writes, 1);
        EXPECT_TRUE(g_kconsole_script.empty());
    }

    // A writer going straight to the kernel console is handed back to its endpoint too.
    TEST(StdoutWrite, KconsoleWriteAllSendsAServedRefusalToStdout)
    {
        reset({TAKE_ALL}, {-KOS_EBUSY});
        kickos::kconsole_write_all("init line\n", 10);
        EXPECT_EQ(g_endpoint, "init line\n");
        EXPECT_EQ(g_kconsole_writes, 1);
    }

    // A non-blocking task. A published console with no receiver now answers its send at once:
    // nothing taken, and the kernel console never asked.
    TEST(StdoutNonblocking, AConsoleNobodyReceivesOnTakesNothing)
    {
        reset({-KOS_ETIMEDOUT}, {});
        expect_result(kickos::stdout_write("line\n", 5), 0u, -KOS_ETIMEDOUT);
        EXPECT_EQ(g_endpoint, "");
        EXPECT_EQ(g_kconsole_writes, 0);
    }

    // A receiver taking part of a message, then none being there for the rest: the short count.
    TEST(StdoutNonblocking, APartialAcceptIsTheShortCount)
    {
        reset({2, -KOS_ETIMEDOUT}, {});
        expect_result(kickos::stdout_write("line\n", 5), 2u, -KOS_ETIMEDOUT);
        EXPECT_EQ(g_endpoint, "li");
        EXPECT_EQ(g_sends, 2);
    }

    // The dark window: the send finds the dead driver's endpoint unserved, and the kernel
    // console answers that it would wait. Nothing is written and nothing retried.
    TEST(StdoutNonblocking, TheDarkWindowTakesNothing)
    {
        reset({-KOS_EAGAIN}, {-KOS_ETIMEDOUT});
        expect_result(kickos::stdout_write("line\n", 5), 0u, -KOS_ETIMEDOUT);
        EXPECT_EQ(g_kconsole_writes, 1);
    }

    // A console nobody published takes what fits on the kernel console, and the rest would wait.
    TEST(StdoutNonblocking, TheKernelConsoleTakesWhatFits)
    {
        reset({-KOS_EBADF}, {3, -KOS_ETIMEDOUT});
        expect_result(kickos::stdout_write("line\n", 5), 3u, -KOS_ETIMEDOUT);
        EXPECT_EQ(g_kconsole, "lin");
    }

    // A publish landing between the two sends the line to the endpoint after all.
    TEST(StdoutNonblocking, ARepublishSendsTheLineToTheEndpoint)
    {
        reset({-KOS_EAGAIN, TAKE_ALL}, {-KOS_EBUSY});
        expect_result(kickos::stdout_write("line\n", 5), 5u, 0);
        EXPECT_EQ(g_endpoint, "line\n");
    }

    // A send no route serves falls back to the kernel console, whose answer is the write's: its
    // refusal of the buffer included, with nothing counted taken.
    class StdoutRoute : public ::testing::TestWithParam<int32_t>
    {
    };

    TEST_P(StdoutRoute, FallsBackToTheKernelConsole)
    {
        reset({GetParam()}, {-KOS_EFAULT});
        expect_result(kickos::stdout_write("line\n", 5), 0u, -KOS_EFAULT);
        EXPECT_EQ(g_kconsole_writes, 1);
        reset({GetParam()}, {TAKE_ALL});
        expect_result(kickos::stdout_write("line\n", 5), 5u, 0);
        EXPECT_EQ(g_kconsole, "line\n");
    }

    INSTANTIATE_TEST_SUITE_P(NoServer, StdoutRoute,
                             ::testing::Values(-KOS_EBADF, -KOS_EAGAIN, -KOS_ECONNREFUSED));

    // Any other refusal of the send ends the write where it stands: the kernel console is not
    // asked, and the bytes not sent are not counted.
    class StdoutRefusal : public ::testing::TestWithParam<int32_t>
    {
    };

    TEST_P(StdoutRefusal, EndsTheWrite)
    {
        reset({2, GetParam()}, {TAKE_ALL});
        expect_result(kickos::stdout_write("line\n", 5), 2u, GetParam());
        EXPECT_EQ(g_endpoint, "li");
        EXPECT_EQ(g_kconsole_writes, 0);
    }

    INSTANTIATE_TEST_SUITE_P(Permanent, StdoutRefusal,
                             ::testing::Values(-KOS_EFAULT, -KOS_ECANCELED, -KOS_EACCES,
                                               -KOS_EINVAL));

    // The kernel console's own refusal ends the write too, with what it took counted.
    class KconsoleRefusal : public ::testing::TestWithParam<int32_t>
    {
    };

    TEST_P(KconsoleRefusal, EndsTheWrite)
    {
        reset({-KOS_EBADF}, {3, GetParam()});
        expect_result(kickos::stdout_write("line\n", 5), 3u, GetParam());
        EXPECT_EQ(g_kconsole, "lin");
        EXPECT_TRUE(g_kconsole_script.empty());
    }

    INSTANTIATE_TEST_SUITE_P(Permanent, KconsoleRefusal,
                             ::testing::Values(-KOS_EFAULT, -KOS_ECANCELED));

    // Both routes refuse the buffer: nothing is taken, and write(2) answers -1 with EFAULT.
    TEST(StdoutWrite, ABufferBothRoutesRefuseTakesNothing)
    {
        reset({-KOS_EFAULT}, {-KOS_EFAULT});
        expect_result(kickos::stdout_write("line\n", 5), 0u, -KOS_EFAULT);
        reset({-KOS_EFAULT}, {-KOS_EFAULT});
        errno = 0;
        EXPECT_EQ(_write(1, "line\n", 5), -1);
        EXPECT_EQ(errno, EFAULT);
    }

    // A writer cancelled waiting on the kernel console has its lost bytes refused, not taken.
    TEST(StdoutWrite, ACancelledWriterCountsNothingLost)
    {
        reset({-KOS_EAGAIN}, {-KOS_ECANCELED});
        expect_result(kickos::stdout_write("line\n", 5), 0u, -KOS_ECANCELED);
        reset({-KOS_EAGAIN}, {-KOS_ECANCELED});
        errno = 0;
        EXPECT_EQ(_write(1, "line\n", 5), -1);
        EXPECT_EQ(errno, ECANCELED);
    }

    // write(2) answers the count where any byte went, the error the kernel gave only where none
    // did.
    TEST(StdoutWrite, LibcWriteAnswersTheBytesThatWent)
    {
        reset({2, -KOS_EFAULT}, {});
        errno = 0;
        EXPECT_EQ(_write(1, "line\n", 5), 2);
        EXPECT_EQ(errno, 0);
        reset({-KOS_ETIMEDOUT}, {});
        EXPECT_EQ(_write(1, "line\n", 5), -1);
        EXPECT_EQ(errno, EAGAIN);
        reset({TAKE_ALL}, {});
        EXPECT_EQ(_write(1, "line\n", 5), 5);
    }

    // kos_print is the same writer: the kernel waits a full ring out, so a try-again ends the call.
    TEST(KosPrint, AFullRingIsWaitedOutInTheKernel)
    {
        reset({-KOS_EBADF}, {-KOS_EAGAIN});
        kos_print("line\n");
        EXPECT_EQ(g_kconsole_writes, 1);
        EXPECT_TRUE(g_kconsole_script.empty());
    }

    // A driver owns the UART: the line goes to the console it publishes, never to the kernel's.
    TEST(KosPrint, AUserOwnedUartTakesTheLineThroughStdout)
    {
        reset({TAKE_ALL}, {});
        kos_print("line\n");
        EXPECT_EQ(g_endpoint, "line\n");
        EXPECT_EQ(g_kconsole_writes, 0);
    }

    // The publish lands between the send and the kernel console, which refuses the line to it.
    TEST(KosPrint, APublishLandingBetweenSendsTheLineToStdout)
    {
        reset({-KOS_EBADF, TAKE_ALL}, {-KOS_EBUSY});
        kos_print("line\n");
        EXPECT_EQ(g_endpoint, "line\n");
        EXPECT_EQ(g_kconsole, "");
    }

    // A non-blocking task stops at the first byte that would make it wait.
    TEST(KosPrint, ANonBlockingTaskStopsWhereItWouldWait)
    {
        reset({-KOS_EBADF}, {2, -KOS_ETIMEDOUT});
        kos_print("line\n");
        EXPECT_EQ(g_kconsole, "li");
        EXPECT_TRUE(g_kconsole_script.empty());
    }

    void expect_console(int (*call)(int, struct stat*), int fd)
    {
        struct stat st;
        memset(&st, 0xA5, sizeof(st));
        ASSERT_EQ(call(fd, &st), 0);
        EXPECT_TRUE(S_ISCHR(st.st_mode));
        EXPECT_EQ(st.st_mode, static_cast<mode_t>(S_IFCHR));
        EXPECT_EQ(st.st_size, 0);
        EXPECT_EQ(st.st_blksize, 0);
        EXPECT_EQ(st.st_blocks, 0);
    }

    TEST(NewlibStubs, FstatReportsTheConsoleAsACharacterDeviceWithNothingElseSet)
    {
        for (int fd = 0; fd < 3; ++fd)
        {
            expect_console(_fstat, fd);
        }
    }

    TEST(NewlibStubs, ThePlainNameAnswersAsTheUnderscoredOne)
    {
        expect_console(kos_ut_plain_fstat, 1);
    }
}
