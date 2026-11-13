// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The call/reply peers of the bench, which its main runs as threads of its own task and as the
// entries of its peer program's two tasks.

#ifndef KICKOS_USER_APPS_COMMON_USERBENCH_CALLREPLY_H
#define KICKOS_USER_APPS_COMMON_USERBENCH_CALLREPLY_H

#include <stdint.h>
#include <kickos/libc/string.h>

#include <kickos/kos.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys.h>

namespace bench
{
    using kos::child_cap;

    // Both call/reply peers are SPAWNED, so the figure is worker-to-worker and not main's.
    constexpr uint32_t CALLREPLY_REPS = 20000;

    // Both peers outrank main (priority 2, its composition's), which joins them.
    constexpr uint8_t CR_PRIO = 4;

    // The caller's parameters travel in its thread argument, by value: a caller in another
    // task runs on its own copy of the app's data where the board translates, taken when that
    // task was created, so a global written afterwards never reaches it.
    constexpr uintptr_t CR_LEN_MASK = 0x1FFu; // KOS_EP_MSG_MAX fits
    // kos_call_generic, which issues KOS_SYS_CALL without attempting the register form.
    constexpr uintptr_t CR_GENERIC = 1u << 9;
    constexpr uintptr_t CR_DONATING = 1u << 10;
    // The cross-task row, never printed under the same-task row's name.
    constexpr uintptr_t CR_XTASK = 1u << 11;
    static_assert(KOS_EP_MSG_MAX <= CR_LEN_MASK, "a message length must fit its field");

    inline void callreply_server(void*) // caps: E(WAIT)@0
    {
        unsigned char buf[KOS_EP_MSG_MAX];
        struct kos_reply_recv_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.ep = child_cap(0);
        opts.timeout_us = KOS_TIMEOUT_NONE;
        kos_cap_t reply_cap = KOS_CAP_NONE;
        size_t reply_len = 0;
        for (uint32_t i = 0; i < CALLREPLY_REPS; i++)
        {
            opts.info.reply_cap = KOS_CAP_NONE;
            long const n = kos_reply_recv(reply_cap, buf,
                                          kos_call_lens_pack(reply_len, sizeof(buf)), &opts);
            reply_cap = KOS_CAP_NONE;
            if (n < 0 or opts.info.reply_cap == KOS_CAP_NONE)
            {
                // A reply-less message is measure_callreply's stop sentinel: a parked
                // receiver pins its own WAIT cap, so nothing but a message ends that park.
                break;
            }
            reply_cap = opts.info.reply_cap;
            reply_len = static_cast<size_t>(n);
        }
        if (reply_cap != KOS_CAP_NONE)
        {
            kos_reply(reply_cap, buf, reply_len);
        }
    }
    inline void callreply_caller(void* arg) // caps: E(SIGNAL)@0
    {
        uintptr_t const params = reinterpret_cast<uintptr_t>(arg);
        unsigned char buf[KOS_EP_MSG_MAX];
        size_t const len = static_cast<size_t>(params & CR_LEN_MASK);
        for (unsigned i = 0; i < sizeof(buf); i++)
        {
            buf[i] = static_cast<unsigned char>(i);
        }
        // Selected OUTSIDE the timed loop, so neither arm carries the other's branch.
        bool const generic = (params & CR_GENERIC) != 0;
        int32_t const want = static_cast<int32_t>(len);
        int32_t got = want;
        uint64_t t0 = kos::clock_now();
        uint32_t reps = 0;
        if (generic)
        {
            while (reps < CALLREPLY_REPS)
            {
                got = kos_call_generic(child_cap(0), buf, len, len, KOS_TIMEOUT_NONE);
                if (got != want)
                {
                    break;
                }
                reps++;
            }
        }
        else
        {
            while (reps < CALLREPLY_REPS)
            {
                // In place: the reply lands back in the request buffer.
                got = kos_call(child_cap(0), buf, len, len, KOS_TIMEOUT_NONE);
                if (got != want)
                {
                    break;
                }
                reps++;
            }
        }
        uint64_t d_ns = kos::clock_now() - t0;
        if (reps != CALLREPLY_REPS)
        {
            char f[96];
            ksnprintf(f, sizeof(f), "  call/reply: %u B FAIL: call %u answered %d\n",
                      static_cast<unsigned>(len), static_cast<unsigned>(reps),
                      static_cast<int>(got));
            kos::print(f);
        }

        if (reps == CALLREPLY_REPS)
        {
            uint32_t rt_per_s = 0;
            uint32_t ns_per_rt = 0;
            if (d_ns != 0)
            {
                rt_per_s = static_cast<uint32_t>(
                    static_cast<uint64_t>(CALLREPLY_REPS) * 1000000000ull / d_ns);
                ns_per_rt = static_cast<uint32_t>(d_ns / CALLREPLY_REPS);
            }
            char const* row_name = "call/reply";
            if ((params & CR_XTASK) != 0)
            {
                row_name = "xtask call/reply";
            }
            char const* shape = "";
            if ((params & CR_DONATING) != 0)
            {
                shape = " [caller outranks server, D1 donates]";
            }
            char const* path = "";
            if (generic)
            {
                path = " [generic path]";
            }
            char s[256];
            ksnprintf(
                s, sizeof(s),
                "  %s: %u B  %u ns/round-trip  (%u round-trips/s over %u calls / %u ms)%s%s\n",
                row_name, static_cast<unsigned>(len), static_cast<unsigned>(ns_per_rt),
                static_cast<unsigned>(rt_per_s), static_cast<unsigned>(CALLREPLY_REPS),
                static_cast<unsigned>(d_ns / 1000000ull), path, shape);
            kos::print(s);
        }
    }

    // Every switch the cross-task rows drive goes from one task to another, which on a
    // translating board is a change of address space; every row above stays in main's task.
    constexpr uint32_t XT_SPAN = 16;
    constexpr uint32_t XT_ROUNDS = 20000;

    // Both players on one core: the throughput row is the same-core handoff. Under a wider
    // mask the placement invariant would spread the equal-priority pair across cores.
    constexpr uint32_t PLAYER_CORES = 1u << 0;

    // The two players' words, in a block their creator reserves: each reaches it through the
    // first window of its create.
    struct XtWords
    {
        kos_sem_t leader;
        kos_sem_t follower;
    };
    constexpr uint32_t XT_WORDS_BYTES = 4096;

    inline XtWords* xt_words()
    {
        kos_window w = {};
        if (kos_window_get(0, &w) != 0)
        {
            return nullptr;
        }
        return reinterpret_cast<XtWords*>(w.base);
    }

    // The leader posts first and times the whole run.
    inline void xt_player(bool leads)
    {
        XtWords* const w = xt_words();
        if (w == nullptr)
        {
            return;
        }
        kos_sem_t* own = &w->follower;
        kos_sem_t* peer = &w->leader;
        if (leads)
        {
            own = &w->leader;
            peer = &w->follower;
        }
        uint64_t const t0 = kos::clock_now();
        uint32_t r = 0;
        for (; r < XT_ROUNDS; r++)
        {
            if (leads)
            {
                if (kos_sem_post(peer) != 0 or kos_sem_wait(own, KOS_TIMEOUT_NONE) != 0)
                {
                    break;
                }
            }
            else if (kos_sem_wait(own, KOS_TIMEOUT_NONE) != 0 or kos_sem_post(peer) != 0)
            {
                break;
            }
        }
        uint64_t const d_ns = kos::clock_now() - t0;
        if (leads and r == XT_ROUNDS)
        {
            char s[160];
            ksnprintf(s, sizeof(s), "  xtask ping-pong: %u ns/switch  (%u switches / %u ms)\n",
                      static_cast<unsigned>(d_ns / (2ull * XT_ROUNDS)),
                      static_cast<unsigned>(2u * XT_ROUNDS),
                      static_cast<unsigned>(d_ns / 1000000ull));
            kos::print(s);
        }
    }
}

#endif
