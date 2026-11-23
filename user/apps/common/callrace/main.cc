// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// A register call against a device interrupt taken inside its trap. The caller loops calls into
// a server above it; the waker, above the caller, waits on a claimed line with no hardware
// source, which the emulator gate's debugger pends at the instruction after the trap reads the
// pending-switch bit. Both clients hold a copy of the server's endpoint at the same index with a
// badge of their own, so a call the kernel ran as the waker's reaches the server with its badge.

#include <kickos/chip_limits.h>
#include <kickos/sys/irq_free.h>
#include <kickos/kos.h>
#include <kickos/libc/fmt.h>
#include <kickos/sys.h>
#include <kickos/sys/atomic.h>

#include <stdint.h>

// The debugger reads it: set once the waker parks, after which every caller trap may be raced.
__attribute__((used)) kickos::Atomic<uint32_t, kickos::Order::RELAXED> g_callrace_armed{0};

namespace
{
    using kickos::Atomic;
    using kickos::Order;
    using kos::child_cap;

    constexpr uint32_t BADGE_CALLER = 0xCA11u;
    constexpr uint32_t BADGE_WAKER = 0x0BADu;

    // The fastpath refuses a caller outranking the receiver, and the wake must preempt the
    // caller; main outranks all three so its sleep ends the run.
    constexpr uint8_t PRIO_CALLER = 4;
    constexpr uint8_t PRIO_WAKER = 10;
    constexpr uint8_t PRIO_SERVER = 12;

    // Must stay a line with no hardware source: only the debugger raises it.
#if defined(KICKOS_IRQ_SOFT_ONLY_BASE)
    constexpr int WAKE_LINE = KICKOS_IRQ_SOFT_ONLY_BASE + 1;
#else
    constexpr int WAKE_LINE = KICKOS_IRQ_FREE_BASE + 10;
#endif
    constexpr int64_t RUN_MS = 3000;
    constexpr uint32_t NOTE_ALL = 0xFFFFFFFFu;
}

// The line the debugger pends; read from memory by the claim so the link keeps it.
int g_callrace_line = WAKE_LINE;

namespace
{
    Atomic<uint32_t, Order::RELAXED> g_stop{0};
    Atomic<uint32_t, Order::RELAXED> g_calls{0};
    Atomic<uint32_t, Order::RELAXED> g_wakes{0};
    Atomic<uint32_t, Order::RELAXED> g_wrong_badges{0};
    Atomic<uint32_t, Order::RELAXED> g_bad_replies{0};

    void server(void*) // caps: E(WAIT)@0
    {
        uint32_t buf[2] = {};
        struct kos_reply_recv_opts opts = {};
        opts.ep = child_cap(0);
        opts.until = KOS_NEVER;
        kos_cap_t reply_cap = KOS_CAP_NONE;
        while (true)
        {
            opts.info.reply_cap = KOS_CAP_NONE;
            int const n = kos_reply_recv(reply_cap, buf,
                                         kos_call_lens_pack(sizeof(buf), sizeof(buf)), &opts);
            reply_cap = KOS_CAP_NONE;
            if (n < 0 or opts.info.reply_cap == KOS_CAP_NONE)
            {
                break; // main's reply-less stop
            }
            if (opts.info.badge != BADGE_CALLER)
            {
                g_wrong_badges = g_wrong_badges + 1;
                char msg[96];
                ksnprintf(msg, sizeof(msg), "[callrace] FAIL: a call arrived with badge 0x%x\n",
                          static_cast<unsigned>(opts.info.badge));
                kos::print(msg);
            }
            buf[0] = buf[0] + 1u;
            reply_cap = opts.info.reply_cap;
        }
    }

    void waker(void*) // caps: E(SIGNAL, BADGE_WAKER)@0, never used; line(WAIT)@1, note@2
    {
        auto note = kos::Notification::adopt(child_cap(2));
        note.bind();
        g_callrace_armed = 1;
        while (g_stop == 0)
        {
            kos::notify_wait(NOTE_ALL);
            g_wakes = g_wakes + 1;
        }
    }

    void caller(void*) // caps: E(SIGNAL, BADGE_CALLER)@0
    {
        uint32_t seq = 0;
        while (g_stop == 0)
        {
            uint32_t buf[2] = {seq, ~seq};
            int32_t const rc = kos_call(child_cap(0), buf, sizeof(buf), sizeof(buf), KOS_NEVER);
            if (rc != static_cast<int32_t>(sizeof(buf)) or buf[0] != seq + 1u or buf[1] != ~seq)
            {
                g_bad_replies = g_bad_replies + 1;
                char msg[128];
                ksnprintf(msg, sizeof(msg),
                          "[callrace] FAIL: call %u answered %d with 0x%x 0x%x\n",
                          static_cast<unsigned>(seq), static_cast<int>(rc),
                          static_cast<unsigned>(buf[0]), static_cast<unsigned>(buf[1]));
                kos::print(msg);
                return;
            }
            seq = seq + 1u;
            g_calls = seq;
        }
    }

    bool spawn(kos_cap_t ep, kos_cap_t line, kos_cap_t note, kos::thread::Handle* out)
    {
        kos_cap_t for_caller = KOS_CAP_NONE;
        kos_cap_t for_waker = KOS_CAP_NONE;
        if (kos_endpoint_badge(ep, BADGE_CALLER, &for_caller) != 0
            or kos_endpoint_badge(ep, BADGE_WAKER, &for_waker) != 0)
        {
            return false;
        }
        kos_cap_grant const scaps[] = {{ep, KOS_CAP_WAIT}};
        kos_cap_grant const wcaps[] = {{for_waker, KOS_CAP_SIGNAL},
                                       {line, KOS_CAP_WAIT},
                                       {note, KOS_CAP_WAIT | KOS_CAP_SIGNAL | KOS_CAP_TRANSFER}};
        kos_cap_grant const ccaps[] = {{for_caller, KOS_CAP_SIGNAL}};
        out[0] = kos::thread::create_caps(server, nullptr, "cr_srv", PRIO_SERVER, scaps, 1);
        out[1] = kos::thread::create_caps(waker, nullptr, "cr_wake", PRIO_WAKER, wcaps, 3);
        out[2] = kos::thread::create_caps(caller, nullptr, "cr_call", PRIO_CALLER, ccaps, 1);
        (void)kos_handle_close(for_caller);
        (void)kos_handle_close(for_waker);
        return out[0].valid() and out[1].valid() and out[2].valid();
    }

    // The waker's line, attached and armed.
    bool claim_line(kos_cap_t* line, kos_cap_t* note)
    {
        if (kos_irq_claim(g_callrace_line, KOS_IRQ_EDGE, line) != 0)
        {
            return false;
        }
        if (kos_notify_create(note) != 0)
        {
            return false;
        }
        if (kos_watch(KOS_WATCH_IRQ, KOS_IRQ_FIRED, *line, *note, 0, nullptr) != 0)
        {
            return false;
        }
        return kos_irq_ack(*line) == 0;
    }
}

int main(int, char**)
{
    kos_cap_t ep = KOS_CAP_NONE;
    kos_cap_t line = KOS_CAP_NONE;
    kos_cap_t note = KOS_CAP_NONE;
    if (kos_endpoint_create(&ep) != 0 or not claim_line(&line, &note))
    {
        kos::print("[callrace] ERROR: no endpoint or no line\n");
        return 1;
    }
    kos::thread::Handle t[3];
    if (not spawn(ep, line, note, t))
    {
        kos::print("[callrace] ERROR: the threads could not be created\n");
        return 1;
    }
    kos_sleep_for(kos_ms(RUN_MS));
    g_stop = 1;
    (void)t[2].join();
    uint32_t const wakes = g_wakes;
    (void)kos_irq_raise(line); // the waker's stop
    (void)t[1].join();
    unsigned char stop = 0;
    (void)kos_send(ep, &stop, 1, KOS_NEVER);
    (void)t[0].join();

    uint32_t const calls = g_calls;
    char msg[128];
    ksnprintf(msg, sizeof(msg), "[callrace] %u calls, %u wakes\n", static_cast<unsigned>(calls),
              static_cast<unsigned>(wakes));
    kos::print(msg);
    if (g_wrong_badges != 0 or g_bad_replies != 0)
    {
        return 1;
    }
    if (calls == 0 or wakes == 0)
    {
        kos::print("[callrace] ERROR: the caller or the waker never ran, so nothing raced\n");
        return 1;
    }
    kos::print("[callrace] PASS: every call reached the server under the caller's own badge\n");
    return 0;
}
