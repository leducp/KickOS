// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The recording fake behind kos_seam.h. Every definition here is a public kos_* name; see
// that header for why this TU may never reach a target image.

#include "kos_seam.h"

#include <kickos/sys.h>
#include <kickos/sys/errno.h>

#include <stdio.h>
#include <string.h>

struct kos_seam_control g_seam = {};

namespace
{
    char g_trace[KOS_SEAM_TRACE_MAX];
    uint32_t g_trace_len;
    char g_msg[KOS_SEAM_MSG_MAX];
    uint32_t g_msg_len;

    uint32_t g_next_thread;
    uint32_t g_spawns;
    void* g_spawn_args[8];
    uint64_t g_now_ns;

    // snprintf returns the length it WOULD have written, so advancing a cursor by it walks
    // past the buffer on the first truncation and makes the next `cap - len` underflow.
    void append(char* buf, uint32_t cap, uint32_t* len, char const* text)
    {
        if (*len + 1u >= cap)
        {
            return;
        }
        uint32_t const room = cap - *len;
        int const n = snprintf(&buf[*len], room, "%s", text);
        if (n <= 0)
        {
            return;
        }
        uint32_t written = static_cast<uint32_t>(n);
        if (written >= room)
        {
            written = room - 1u;
        }
        *len += written;
    }

    void note(char const* token)
    {
        if (g_trace_len != 0u)
        {
            append(g_trace, KOS_SEAM_TRACE_MAX, &g_trace_len, " ");
        }
        append(g_trace, KOS_SEAM_TRACE_MAX, &g_trace_len, token);
    }

    void note_id(char const* verb, uint32_t id)
    {
        char buf[40];
        snprintf(buf, sizeof(buf), "%s%u", verb, id);
        note(buf);
    }
}

void kos_seam_reset()
{
    g_seam = kos_seam_control{};
    g_trace[0] = '\0';
    g_trace_len = 0;
    g_msg[0] = '\0';
    g_msg_len = 0;
    g_next_thread = KOS_SEAM_THREAD_BASE;
    g_spawns = 0;
    memset(g_spawn_args, 0, sizeof(g_spawn_args));
    g_now_ns = 0;
}

char const* kos_seam_trace()
{
    return g_trace;
}

char const* kos_seam_msg()
{
    return g_msg;
}

void* kos_seam_spawn_arg(uint32_t i)
{
    if (i >= sizeof(g_spawn_args) / sizeof(g_spawn_args[0]))
    {
        return nullptr;
    }
    return g_spawn_args[i];
}

void kos_seam_note(char const* token)
{
    note(token);
}

extern "C"
{

    void kos_print(char const* s)
    {
        note("print");
        if (s == nullptr)
        {
            return;
        }
        append(g_msg, KOS_SEAM_MSG_MAX, &g_msg_len, s);
    }

    int32_t kos_kconsole_write(void const* buf, size_t len, uint32_t)
    {
        note("kprint");
        char line[KOS_SEAM_MSG_MAX];
        size_t n = len;
        if (n > sizeof(line) - 1u)
        {
            n = sizeof(line) - 1u;
        }
        memcpy(line, buf, n);
        line[n] = '\0';
        append(g_msg, KOS_SEAM_MSG_MAX, &g_msg_len, line);
        return static_cast<int32_t>(len);
    }

    void kos_sleep_ns(uint64_t ns)
    {
        g_now_ns += ns;
    }

    uint64_t kos_clock_now(void)
    {
        return g_now_ns;
    }

    // The thread, its priority, its core where one is named, then each delegated capability as
    // the spawner's index it comes from and the rights it carries.
    int kos_thread_create(struct kos_thread_params const* params, kos_thread_t* out_thread)
    {
        g_spawns++;
        if (params == nullptr or g_spawns == g_seam.spawn_fail_at)
        {
            *out_thread = KOS_THREAD_NONE;
            note("spawn!");
            return -KOS_ENOMEM;
        }
        *out_thread = g_next_thread;
        g_next_thread++;
        char buf[64];
        snprintf(buf, sizeof(buf), "spawn%u/p%u", *out_thread, params->prio);
        note(buf);
        if ((params->flags & KOS_THREAD_ESSENTIAL) != 0u)
        {
            note("essential");
        }
        if (params->window_count != 0u)
        {
            note("foreign!");
        }
        if (params->core_mask != 0u)
        {
            note_id("core", params->core_mask);
        }
        for (uint8_t i = 0; i < params->cap_count; i++)
        {
            snprintf(buf, sizeof(buf), "g%u/%u", params->caps[i].source_cap, params->caps[i].rights_mask);
            note(buf);
        }
        if (g_spawns <= sizeof(g_spawn_args) / sizeof(g_spawn_args[0]))
        {
            g_spawn_args[g_spawns - 1u] = params->arg;
        }
        if (g_seam.run_on_spawn)
        {
            params->entry(params->arg);
        }
        return 0;
    }

    int kos_cap_narrow(kos_cap_t cap, uint32_t mask)
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "narrow%u/%u", cap, mask);
        note(buf);
        return 0;
    }

    int kos_handle_close(kos_cap_t cap)
    {
        note_id("close", cap);
        return 0;
    }

    int kos_thread_set_priority(uint8_t priority)
    {
        note_id("prio", priority);
        return g_seam.set_priority_rc;
    }

    void kos_exit(int code)
    {
        note_id("exit", static_cast<uint32_t>(code));
        throw kos_seam_exit{code};
    }

    int kos_word_wait(uint32_t* word, uint32_t expected, uint32_t timeout_us)
    {
        if (*word != expected)
        {
            note("park?");
            return -KOS_EAGAIN;
        }
        note_id("park", timeout_us);
        if (g_seam.publish_on_park)
        {
            *word = 1u;
            return 0;
        }
        g_now_ns += uint64_t{timeout_us} * 1000u;
        return -KOS_ETIMEDOUT;
    }

    int kos_word_wake(uint32_t*, uint32_t n, uint32_t units)
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "wake%u/%u", n, units);
        note(buf);
        return 0;
    }

    int32_t kos_send(kos_cap_t, void const*, size_t, uint32_t)
    {
        note("send");
        return 0;
    }

    int kos_notify_badge(kos_cap_t, uint32_t, kos_cap_t*)
    {
        note("badge");
        return -KOS_EINVAL;
    }
}
