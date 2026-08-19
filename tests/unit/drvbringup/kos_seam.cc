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
    // A power-of-two, naturally aligned block: bring_up asks kos_ram_alloc for one, and the
    // RAM arm of the grant predicate demands it of root on silicon.
    alignas(1024) unsigned char g_arena[1024];

    char g_trace[KOS_SEAM_TRACE_MAX];
    uint32_t g_trace_len;
    char g_msg[KOS_SEAM_MSG_MAX];
    uint32_t g_msg_len;

    uint32_t g_next_cap;
    uint32_t g_next_thread;
    uint32_t g_next_task;
    uint32_t g_irq_claims;
    uint32_t g_spawns;

    // Run of consecutive kos_sleep_ns calls not yet rendered.
    uint32_t g_pending_sleeps;

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

    void raw(char const* token)
    {
        if (g_trace_len != 0u)
        {
            append(g_trace, KOS_SEAM_TRACE_MAX, &g_trace_len, " ");
        }
        append(g_trace, KOS_SEAM_TRACE_MAX, &g_trace_len, token);
    }

    void flush_sleeps()
    {
        if (g_pending_sleeps == 0u)
        {
            return;
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "sleep*%u", g_pending_sleeps);
        g_pending_sleeps = 0;
        raw(buf);
    }

    void note(char const* token)
    {
        flush_sleeps();
        raw(token);
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
    g_seam.latch_on_spawn = true;
    memset(g_arena, 0, sizeof(g_arena));
    g_trace[0] = '\0';
    g_trace_len = 0;
    g_msg[0] = '\0';
    g_msg_len = 0;
    g_next_cap = KOS_SEAM_CAP_BASE;
    g_next_thread = KOS_SEAM_THREAD_BASE;
    g_next_task = KOS_SEAM_TASK_BASE;
    g_irq_claims = 0;
    g_spawns = 0;
    g_pending_sleeps = 0;
}

char const* kos_seam_trace()
{
    flush_sleeps();
    return g_trace;
}

char const* kos_seam_msg()
{
    return g_msg;
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

    void kos_sleep_ns(uint64_t)
    {
        g_pending_sleeps++;
    }

    void* kos_ram_alloc(size_t size)
    {
        note("alloc");
        if (g_seam.ram_alloc_fails or size > sizeof(g_arena))
        {
            return nullptr;
        }
        return g_arena;
    }

    int kos_mem_self_grant(void*, size_t, uint32_t flags)
    {
        // The flag goes in the TOKEN: an arm reads which of the block's two grants carried it.
        if (flags == KOS_MEM_NOCACHE)
        {
            note("grantnc");
        }
        else
        {
            note("grant");
        }
        if (g_seam.self_grant_fails)
        {
            return -KOS_EINVAL;
        }
        return 0;
    }

    int kos_endpoint_create(kos_cap_t* out_cap)
    {
        if (g_seam.endpoint_create_fails)
        {
            note("ep!");
            return -KOS_ENOMEM;
        }
        *out_cap = g_next_cap;
        g_next_cap++;
        note_id("ep", *out_cap);
        return 0;
    }

    int kos_console_publish(kos_cap_t ep)
    {
        if (g_seam.console_publish_fails)
        {
            note("pub!");
            return -KOS_EBUSY;
        }
        note_id("pub", ep);
        return 0;
    }

    int kos_irq_claim(int, unsigned int, kos_cap_t* out_cap)
    {
        g_irq_claims++;
        if (g_irq_claims == g_seam.irq_claim_fail_at)
        {
            // The out-param is left UNTOUCHED, as the real syscall leaves it on a refusal:
            // bring_up's line[] pre-fill with KOS_CAP_NONE is the only thing that keeps the
            // slot readable, and a fake that helpfully wrote one would hide a regression
            // that dropped it.
            note("claim!");
            return -KOS_EBUSY;
        }
        *out_cap = g_next_cap;
        g_next_cap++;
        note_id("claim", *out_cap);
        return 0;
    }

    int kos_thread_spawn(struct kos_thread_params const* params, kos_thread_t* out_thread)
    {
        g_spawns++;
        if (params == nullptr)
        {
            note("spawn!");
            return -KOS_EINVAL;
        }
        if (g_spawns == g_seam.spawn_fail_at)
        {
            *out_thread = KOS_THREAD_NONE;
            note("spawn!");
            return -KOS_ENOMEM;
        }
        *out_thread = g_next_thread;
        g_next_thread++;
        note_id("spawn", *out_thread);
        // The child does not run here, so its ONE observable effect on the parent, reaching
        // its loop and setting the latch, is modelled at the spawn instead.
        if (g_seam.latch_on_spawn and g_seam.latch != nullptr)
        {
            *g_seam.latch = 1u;
        }
        return 0;
    }

    int kos_thread_kill(kos_thread_t thread)
    {
        note_id("kill", thread);
        return 0;
    }

    int kos_task_create(void* mem_base, uint32_t mem_size, uint32_t mem_flags,
                        kos_task_t* out_task)
    {
        *out_task = KOS_TASK_NONE;
        if (g_seam.task_create_fails)
        {
            note("task!");
            return -KOS_ENOMEM;
        }
        *out_task = g_next_task;
        g_next_task++;
        // The SHARED grant is recorded, not just the handle: whether the ring block reaches
        // the group is the whole of what moved from the spawn to the task.
        if (mem_base != nullptr and mem_size != 0)
        {
            if (mem_flags == KOS_MEM_NOCACHE)
            {
                note_id("taskmemnc", *out_task);
            }
            else
            {
                note_id("taskmem", *out_task);
            }
        }
        else
        {
            note_id("task", *out_task);
        }
        return 0;
    }

    int kos_task_kill(kos_task_t task)
    {
        note_id("tkill", task);
        return 0;
    }

    int kos_handle_close(kos_cap_t cap)
    {
        note_id("close", cap);
        return 0;
    }

    int32_t kos_send_timed(kos_cap_t, void const*, size_t, uint32_t)
    {
        note("probe");
        return g_seam.send_timed_rc;
    }

    // edge_relay_thread's two. No arm here runs that thread body; these exist so the TU
    // carrying bring_up links.
    int kos_irq_wait(kos_cap_t)
    {
        return -KOS_EBADF;
    }

    int kos_irq_notify(kos_cap_t)
    {
        return -KOS_EBADF;
    }
}
