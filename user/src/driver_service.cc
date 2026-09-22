// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The bring-up choreography of an unprivileged driver service. The contract for each
// function is stated at its declaration in <kickos/sys/driver_service.h>.

#include <kickos/sys/driver_service.h>

#include <kickos/sys/atomic.h>

#include <stdlib.h>

namespace kickos::driver
{

int fail(char const* tag, char const* msg)
{
    kos::print(tag);
    kos::print(msg);
    return -1;
}

int console_handover_finish(kos_cap_t ep, char const* tag, kos_task_t task)
{
    kos_handle_close(ep);
    int const rc = kos_send_timed(KOS_CAP_STDOUT, "", 0, KOS_DRV_HANDOVER_PROBE_US);
    if (rc >= 0)
    {
        return 0;
    }
    if (rc != -KOS_EPIPE)
    {
        return rc;
    }
    (void)kos_task_kill(task);
    (void)fail(tag, "ERROR: a driver thread died during bring-up\n");
    return rc;
}

bool wait_ready(void const* blk, uint16_t off)
{
    Atomic<uint32_t, Order::RELAXED> const* const flag =
        reinterpret_cast<Atomic<uint32_t, Order::RELAXED> const*>(
        static_cast<unsigned char const*>(blk) + off);
    // Sleeping, not spinning: the IRQ thread may sit below root's priority.
    for (uint32_t i = 0; i < KOS_DRV_READY_WAIT_MAX; i++)
    {
        if (*flag != 0u)
        {
            return true;
        }
        kos_sleep_ns(KOS_DRV_READY_WAIT_NS);
    }
    return *flag != 0u;
}

void unwind(kos_cap_t const* line, uint8_t claimed, kos_cap_t ep, kos_cap_t note,
            kos_task_t task)
{
    for (uint8_t i = 0; i < claimed; i++)
    {
        kos_handle_close(line[i]);
    }
    if (note != KOS_CAP_NONE)
    {
        kos_handle_close(note); // guarded: a driver with no notification must not close one
    }
    kos_handle_close(ep);
    // Ends every member and drops root's hold on the task slot. Legal on a group that never
    // got a member.
    (void)kos_task_kill(task);
}

// Every capability this spawn minted for itself, closed once the child holds its copies.
void drop_minted(kos_cap_t* minted)
{
    for (uint8_t i = 0; i < KOS_DRV_CAPS_MAX; i++)
    {
        if (minted[i] != KOS_CAP_NONE)
        {
            kos_handle_close(minted[i]);
            minted[i] = KOS_CAP_NONE;
        }
    }
}

kos::thread::Handle spawn_one(Thread const& t, struct kos_service_cfg const* cfg, void* blk,
                              kos_cap_t ep, kos_cap_t const* line, kos_cap_t note,
                              kos_task_t task)
{
    kos_cap_grant grants[KOS_DRV_CAPS_MAX] = {};
    // A BADGED notification copy exists only as long as this spawn needs a source for it.
    kos_cap_t minted[KOS_DRV_CAPS_MAX] = {};
    for (uint8_t i = 0; i < KOS_DRV_CAPS_MAX; i++)
    {
        minted[i] = KOS_CAP_NONE;
    }
    for (uint8_t i = 0; i < t.cap_count; i++)
    {
        if (t.caps[i].resource == KOS_DRV_RES_EP)
        {
            grants[i].source_cap = ep;
        }
        else if (t.caps[i].resource == KOS_DRV_RES_NOTIFY)
        {
            if (t.caps[i].badge == 0u)
            {
                grants[i].source_cap = note;
            }
            else if (kos_notify_badge(note, badge_bit(t.caps[i].badge), &minted[i]) != 0)
            {
                drop_minted(minted);
                return kos::thread::Handle();
            }
            else
            {
                grants[i].source_cap = minted[i];
            }
        }
        else
        {
            grants[i].source_cap = line[t.caps[i].resource - KOS_DRV_RES_LINE0];
        }
        grants[i].rights_mask = t.caps[i].rights;
    }

    void* arg = nullptr;
    if (t.arg == KOS_DRV_ARG_BLOCK)
    {
        arg = blk;
    }
    else if (t.arg == KOS_DRV_ARG_WINDOW)
    {
        arg = reinterpret_cast<void*>(cfg->mmio_base);
    }

    void* win = nullptr;
    uint32_t win_size = 0;
    if (t.window_grant)
    {
        win = reinterpret_cast<void*>(cfg->mmio_base);
        win_size = cfg->mmio_window;
    }

    char const* name = t.name;
    if (name == nullptr)
    {
        name = cfg->name;
    }

    // No mem grant of its own: the ring block is the TASK's shared region, and a member
    // bringing one is refused -KOS_EINVAL.
    auto h = kos::thread::create(t.entry, arg, name,
                                 static_cast<uint8_t>(cfg->prio + t.prio_delta),
                                 KOS_POLICY_FIFO, /*quantum_ns=*/0, /*privileged=*/false,
                                 /*mem=*/nullptr, /*mem_size=*/0,
                                 /*stack=*/nullptr, /*stack_size=*/0,
                                 win, win_size, grants, t.cap_count,
                                 /*authority=*/0, /*cap_dest=*/nullptr, task);
    // The child holds its own copies now, so this spawn's sources go back.
    drop_minted(minted);
    return h;
}

int bring_up(Descriptor const& d, struct kos_service_cfg const* cfg, kos_cap_t* out_ep)
{
    // valid() bounds every index below, so it runs first. d.tag is L10's own subject: a
    // descriptor that failed only L10 reaches fail() with a null tag.
    if (not valid(d))
    {
        char const* tag = d.tag;
        if (tag == nullptr)
        {
            tag = "[driver] ";
        }
        return fail(tag, "ERROR: the descriptor is not a well-formed driver shape\n");
    }
    for (uint8_t i = 0; i < d.thread_count; i++)
    {
        if (d.threads[i].entry == nullptr)
        {
            return fail(d.tag, "ERROR: a thread in the descriptor has no entry\n");
        }
    }
    if (cfg == nullptr or cfg->kind != d.svc_kind)
    {
        return fail(d.tag, "ERROR: bad or wrong-kind service cfg\n");
    }
    if (d.expected_base != 0u and cfg->mmio_base != d.expected_base)
    {
        return fail(d.tag, "ERROR: cfg mmio_base is not this driver's block\n");
    }
    if ((d.ep_posture == KOS_DRV_EP_RETAIN) != (out_ep != nullptr))
    {
        return fail(d.tag, "ERROR: out_ep does not match the endpoint posture\n");
    }

    void* blk = nullptr;
    if (d.block_size != 0u)
    {
        // ONE power-of-two, naturally aligned: the RAM arm of the grant predicate demands it
        // of root too.
        blk = kos_ram_alloc(d.block_size);
        if (blk == nullptr)
        {
            return fail(d.tag, "ERROR: arena cannot spare the ring block\n");
        }
        // Reach it before writing it: kos_ram_alloc grants nothing, and under enforcement
        // root's own region set does not cover the arena.
        if (kos_mem_self_grant(blk, d.block_size, d.block_flags) != 0)
        {
            return fail(d.tag, "ERROR: mem_self_grant of the ring block refused\n");
        }
        if (d.block_init(blk, cfg) != 0)
        {
            return fail(d.tag, "ERROR: block_init refused the cfg\n");
        }
    }

    // THE GROUP. Every thread of this driver joins it, so a peer's death ends the rest and
    // one call ends them all.
    kos_task_t task = KOS_TASK_NONE;
    if (kos_task_create(blk, d.block_size, d.block_flags, &task) != 0)
    {
        return fail(d.tag, "ERROR: task_create failed\n");
    }

    kos_cap_t note = KOS_CAP_NONE;
    kos_cap_t ep = KOS_CAP_NONE;
    if (kos_endpoint_create(&ep) != 0)
    {
        (void)kos_task_kill(task);
        return fail(d.tag, "ERROR: endpoint_create failed\n");
    }

    // PUBLISH BEFORE CLAIM: irq_claim refuses a line while any handler but the default is
    // attached, and only the publish detaches the kernel's own ring from that vector.
    if (d.ep_posture == KOS_DRV_EP_HANDOVER)
    {
        if (kos_console_publish(ep) != 0)
        {
            kos_handle_close(ep);
            (void)kos_task_kill(task);
            return fail(d.tag, "ERROR: console_publish failed\n");
        }
    }

    kos_cap_t line[KOS_DRV_LINES_MAX] = {KOS_CAP_NONE, KOS_CAP_NONE};
    uint8_t claimed = 0;
    for (uint8_t i = 0; i < d.line_count; i++)
    {
        // Claimed HERE: minting needs KOS_AUTH_IRQ and every driver thread runs at authority
        // 0. A line comes back MASKED, and the bound thread's first wait over its bit arms
        // it.
        if (kos_irq_claim(d.lines[i].number, d.lines[i].trigger, &line[i]) != 0)
        {
            unwind(line, claimed, ep, note, task);
            return fail(d.tag, "ERROR: irq_claim failed\n");
        }
        claimed++;
    }

    // ONE object for the whole driver: every line raises a bit of it, and so does every
    // doorbell. LINE i IS BIT i, which is the rule leg L13 holds a doorbell badge above.
    if (notify_used(d))
    {
        if (kos_notify_create(&note) != 0)
        {
            unwind(line, claimed, ep, note, task);
            return fail(d.tag, "ERROR: notify_create failed\n");
        }
        for (uint8_t i = 0; i < claimed; i++)
        {
            kos_cap_t badged = KOS_CAP_NONE;
            if (kos_notify_badge(note, i, &badged) != 0)
            {
                unwind(line, claimed, ep, note, task);
                return fail(d.tag, "ERROR: notify_badge failed\n");
            }
            int const rc = kos_irq_bind_notify(line[i], badged);
            // The BINDING took a reference of its own, so this name is spent either way.
            kos_handle_close(badged);
            if (rc != 0)
            {
                unwind(line, claimed, ep, note, task);
                return fail(d.tag, "ERROR: irq_bind_notify failed\n");
            }
        }
    }

    // thread_count + 1 barrier positions: barrier_after == thread_count polls AFTER the last
    // spawn, which L8 admits under RETAIN only.
    for (uint8_t i = 0; i <= d.thread_count; i++)
    {
        if (d.ready_offset != KOS_DRV_READY_NONE and i == d.barrier_after)
        {
            if (not wait_ready(blk, d.ready_offset))
            {
                unwind(line, claimed, ep, note, task);
                return fail(d.tag, "ERROR: a driver thread never reached its loop\n");
            }
        }
        if (i == d.thread_count)
        {
            break;
        }
        if (not spawn_one(d.threads[i], cfg, blk, ep, line, note, task).valid())
        {
            unwind(line, claimed, ep, note, task);
            return fail(d.tag, "ERROR: driver thread spawn failed\n");
        }
    }

    // With the driver threads the only holders, a line returns to the pool when they die,
    // and the notification with the last of them: its binder's reference and each attached
    // line's go at the same death.
    for (uint8_t i = 0; i < claimed; i++)
    {
        kos_handle_close(line[i]);
    }
    if (note != KOS_CAP_NONE)
    {
        kos_handle_close(note);
    }

    if (d.ep_posture == KOS_DRV_EP_RETAIN)
    {
        *out_ep = ep;
        return 0;
    }
    return console_handover_finish(ep, d.tag, task);
}

}
