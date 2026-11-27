// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The relay against a scripted kernel: each receive takes the next answer of the script, and a
// receive past its end means the relay asked again where it should have stopped.

#include <gtest/gtest.h>

#include <csetjmp>
#include <cstring>
#include <deque>
#include <string>

#include <kickos/sys.h>
#include <kickos/sys/table.h>

extern "C" int amprelay_main(void);

namespace
{
    constexpr kos_cap_t EP = 0x41u;
    constexpr kos_cap_t REPLY = 0x77u;
    constexpr uint32_t PORT = 6u;

    // A message, or with `error` set the answer itself; a call carries its reply capability.
    struct Arrival
    {
        int32_t error;
        std::string bytes;
        kos_cap_t reply_cap;
    };

    struct Receive
    {
        kos_cap_t reply_cap;
        size_t reply_len;
        kos_cap_t ep;
        uint32_t flags;
    };

    std::deque<Arrival> g_script;
    std::deque<Receive> g_receives;
    // Each call the relay makes on its own standard output, and the answer it is given: a
    // negative error, else these bytes.
    std::deque<std::string> g_calls;
    int32_t g_call_error = 0;
    std::string g_call_answer;
    std::string g_out;
    std::string g_endpoint_name;
    std::jmp_buf g_spun;

    struct alignas(8) Table
    {
        kos_table_header header;
        kos_table_task task;
        kos_table_grant grant;
    };
    Table g_table;

    void reset(std::deque<Arrival> script, uint8_t flags)
    {
        g_script = std::move(script);
        g_receives.clear();
        g_calls.clear();
        g_call_error = 0;
        g_call_answer.clear();
        g_out.clear();
        g_endpoint_name.clear();
        g_table = {};
        g_table.header.task_count = 1u;
        g_table.header.grant_count = 1u;
        g_table.task.first_grant = 0u;
        g_table.task.grant_count = 1u;
        g_table.grant.kind = KOS_GRANT_PORT;
        g_table.grant.flags = flags;
        g_table.grant.base = PORT;
        g_table.grant.size = 1u;
    }

    // The relay's exit status, or -1 where it asked for a receive the script does not hold.
    int run()
    {
        if (setjmp(g_spun) != 0)
        {
            return -1;
        }
        return amprelay_main();
    }
}

extern "C"
{
    struct kos_table_header const* const kickos_table = &g_table.header;

    kos_self_t const* kos_self(void)
    {
        return &g_table.task;
    }

    kos_cap_t kos_grant_endpoint(char const* name)
    {
        g_endpoint_name = name;
        return EP;
    }

    int kos_reply_recv(kos_cap_t reply_cap, void* buf, uintptr_t lens, struct kos_reply_recv_opts* opts)
    {
        g_receives.push_back({reply_cap, kos_call_lens_send(lens), opts->ep, opts->flags});
        if (g_script.empty())
        {
            std::longjmp(g_spun, 1);
        }
        Arrival const a = g_script.front();
        g_script.pop_front();
        if (a.error != 0)
        {
            return a.error;
        }
        size_t n = a.bytes.size();
        if (n > kos_call_lens_recv(lens))
        {
            n = kos_call_lens_recv(lens);
        }
        std::memcpy(buf, a.bytes.data(), n);
        opts->info.reply_cap = KOS_CAP_NONE;
        if ((opts->flags & KOS_RECV_NO_INFO) == 0u)
        {
            opts->info.reply_cap = a.reply_cap;
        }
        return static_cast<int>(n);
    }

    int32_t kos_call(kos_cap_t ep, void* buf, size_t send_len, size_t recv_cap, kos_monotonic_t)
    {
        EXPECT_EQ(ep, static_cast<kos_cap_t>(KOS_CAP_STDOUT));
        g_calls.emplace_back(static_cast<char const*>(buf), send_len);
        if (g_call_error != 0)
        {
            return g_call_error;
        }
        size_t n = g_call_answer.size();
        if (n > recv_cap)
        {
            n = recv_cap;
        }
        std::memcpy(buf, g_call_answer.data(), n);
        return static_cast<int32_t>(n);
    }

    ssize_t amprelay_write(int fd, void const* buf, size_t n)
    {
        EXPECT_EQ(fd, 1);
        g_out.append(static_cast<char const*>(buf), n);
        return static_cast<ssize_t>(n);
    }
}

namespace
{
    TEST(AmpRelay, ARefusedReceiveEndsTheRelayWithADiagnostic)
    {
        reset({{-KOS_EBADF, "", KOS_CAP_NONE}}, KOS_CAP_WAIT);
        EXPECT_EQ(run(), 1) << "the relay received again on an endpoint that refused it";
        EXPECT_EQ(g_out.rfind("amprelay: ", 0), 0u) << g_out;
        EXPECT_EQ(g_receives.size(), 1u);
    }

    TEST(AmpRelay, ACancelledReceiveIsAskedAgain)
    {
        reset({{-KOS_ECANCELED, "", KOS_CAP_NONE}, {0, "line\n", KOS_CAP_NONE}, {-KOS_EBADF, "", KOS_CAP_NONE}},
              KOS_CAP_WAIT);
        EXPECT_EQ(run(), 1);
        EXPECT_EQ(g_out.rfind("line\n", 0), 0u) << g_out;
        EXPECT_EQ(g_receives.size(), 3u);
    }

    // A console request crosses as a call: the relay makes it again on its own standard output
    // and answers with what that answered, writing nothing.
    TEST(AmpRelay, ACallIsForwardedAsACallAndItsAnswerIsTheReply)
    {
        reset({{0, std::string("\x02\0\0\0req", 7), REPLY}, {0, "next\n", KOS_CAP_NONE},
               {-KOS_EBADF, "", KOS_CAP_NONE}},
              KOS_CAP_WAIT);
        g_call_answer = std::string("\0\0\0\0ans", 7);
        EXPECT_EQ(run(), 1);
        ASSERT_EQ(g_calls.size(), 1u) << "the call was not forwarded as a call";
        EXPECT_EQ(g_calls[0], std::string("\x02\0\0\0req", 7));
        ASSERT_GE(g_receives.size(), 3u);
        EXPECT_EQ(g_receives[1].reply_cap, REPLY) << "the call was never answered";
        EXPECT_EQ(g_receives[1].reply_len, 7u) << "the answer was not relayed whole";
        EXPECT_EQ(g_receives[2].reply_cap, KOS_CAP_NONE) << "a send was answered as a call";
        EXPECT_EQ(g_out.rfind("next\n", 0), 0u) << "the call's bytes reached the console: " << g_out;
    }

    // A forward the relay's standard output refuses is answered empty.
    TEST(AmpRelay, ARefusedForwardIsAnsweredEmpty)
    {
        reset({{0, "req", REPLY}, {-KOS_EBADF, "", KOS_CAP_NONE}}, KOS_CAP_WAIT);
        g_call_error = -KOS_EBADF;
        EXPECT_EQ(run(), 1);
        ASSERT_GE(g_receives.size(), 2u);
        EXPECT_EQ(g_receives[1].reply_cap, REPLY);
        EXPECT_EQ(g_receives[1].reply_len, 0u);
        EXPECT_EQ(g_out.find("req"), std::string::npos) << g_out;
    }

    TEST(AmpRelay, EveryReceiveIsOnTheServedEndpoint)
    {
        reset({{0, "abc", KOS_CAP_NONE}, {0, "\n", KOS_CAP_NONE}, {-KOS_EBADF, "", KOS_CAP_NONE}}, KOS_CAP_WAIT);
        EXPECT_EQ(run(), 1);
        EXPECT_EQ(g_out.rfind("abc\n", 0), 0u) << g_out;
        for (Receive const& r : g_receives)
        {
            EXPECT_EQ(r.ep, EP);
        }
    }

    TEST(AmpRelay, APortGrantHoldingWaitAmongOtherRightsIsServed)
    {
        reset({{-KOS_EBADF, "", KOS_CAP_NONE}}, KOS_CAP_WAIT | KOS_CAP_SIGNAL);
        EXPECT_EQ(run(), 1);
        EXPECT_EQ(g_endpoint_name, "/amp/6");
        EXPECT_EQ(g_receives.size(), 1u) << "the relay found no port to serve";
    }

    TEST(AmpRelay, APortGrantWithoutWaitIsNotServed)
    {
        reset({}, KOS_CAP_SIGNAL);
        EXPECT_EQ(run(), 1);
        EXPECT_EQ(g_receives.size(), 0u);
        EXPECT_EQ(g_out.rfind("amprelay: ", 0), 0u) << g_out;
    }
}
