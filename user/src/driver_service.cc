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

void trap()
{
    while (true)
    {
#if defined(__XTENSA__)
        __asm volatile("ill");
#elif defined(__riscv)
        __asm volatile(".word 0x00000000"); // all-zero encoding: illegal on RV32 and RV64
#elif defined(__arm__) or defined(__thumb__)
        __asm volatile("udf #0");
#elif defined(__aarch64__)
        // Not __builtin_trap, which is `brk` there and raises a debug exception instead.
        __asm volatile(".inst 0x00000000");
#elif defined(__RX__)
        // A privileged-instruction exception in user mode; GCC lowers __builtin_trap to abort().
        __asm volatile("mvtipl #0");
#else
        __builtin_trap();
#endif
    }
}

namespace
{
void end_failed_task(struct kos_driver_instance& in)
{
    if (in.task != KOS_TASK_NONE and kos_task_slay(in.task, KOS_DRV_HANDOVER_PROBE_US) == 0)
    {
        in.task = KOS_TASK_NONE;
    }
}
}

void console_start_failed(struct kos_driver_instance& in)
{
    (void)kos_cap_narrow(in.endpoint, KOS_DRV_HANDOVER_KEPT);
    end_failed_task(in);
}

int console_handover_finish(struct kos_driver_instance& in, char const* tag)
{
    (void)kos_cap_narrow(in.endpoint, KOS_DRV_HANDOVER_KEPT);
    int const probe = kos_send_timed(KOS_CAP_STDOUT, "", 0, KOS_DRV_HANDOVER_PROBE_US);
    if (probe >= 0)
    {
        return 0;
    }
    end_failed_task(in);
    (void)fail(tag, "ERROR: the console handover probe was not taken\n");
    return probe;
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

kos::thread::Handle spawn_one(Thread const& t, struct kos_driver_instance const& in,
                              kos_cap_t const* line, kos_cap_t note, kos_task_t task,
                              uint32_t core_mask)
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
            grants[i].source_cap = in.endpoint;
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
        arg = in.block;
    }
    else if (t.arg == KOS_DRV_ARG_WINDOW)
    {
        arg = reinterpret_cast<void*>(in.mmio_base);
    }
    else if (t.arg == KOS_DRV_ARG_LINE0_INDEX)
    {
        arg = reinterpret_cast<void*>(line_index_arg(LineIndex{in.lines[0].index}));
    }

    kos_window const win = {in.mmio_base, in.mmio_window, KOS_WINDOW_DEVICE, 0};
    uint16_t win_count = 0;
    if (t.window_grant)
    {
        win_count = 1;
    }

    char const* name = t.name;
    if (name == nullptr)
    {
        name = in.name;
    }

    // No mem grant of its own: the ring block is the TASK's shared region, and a member
    // bringing one is refused -KOS_EINVAL.
    auto h = kos::thread::create(t.entry, arg, name,
                                 static_cast<uint8_t>(in.priority + t.prio_delta),
                                 KOS_POLICY_FIFO, /*quantum_ns=*/0, /*privileged=*/false,
                                 /*mem=*/nullptr, /*mem_size=*/0,
                                 /*stack=*/nullptr, /*stack_size=*/0,
                                 &win, win_count, grants, t.cap_count,
                                 /*authority=*/0, /*cap_dest=*/nullptr, task, core_mask);
    // The child holds its own copies now, so this spawn's sources go back.
    drop_minted(minted);
    return h;
}

namespace
{
// What a failed bring-up given an instance made goes; a console endpoint is narrowed before the
// diagnostic prints, its WAIT being what holds the console; the task and the endpoint stay the
// init's.
int instance_failed(Descriptor const& d, struct kos_driver_instance& in, kos_cap_t const* line,
                    uint8_t claimed, kos_cap_t note, char const* msg)
{
    for (uint8_t i = 0; i < claimed; i++)
    {
        kos_handle_close(line[i]);
    }
    if (note != KOS_CAP_NONE)
    {
        kos_handle_close(note);
    }
    if (d.ep_posture == KOS_DRV_EP_HANDOVER)
    {
        console_start_failed(in);
    }
    return fail(d.tag, msg);
}

// A line retiring from the instance before answers -KOS_EAGAIN until it is free.
int claim(uint16_t number, uint8_t trigger, kos_cap_t* out)
{
    uint32_t retries = 0;
    while (true)
    {
        int const rc = kos_irq_claim(number, trigger, out);
        if (rc != -KOS_EAGAIN or retries == KOS_DRV_CLAIM_RETRIES)
        {
            return rc;
        }
        retries++;
        kos_sleep_ns(KOS_DRV_CLAIM_RETRY_NS);
    }
}

// An instance that is not this descriptor's is refused before anything is made.
int instance_admits(Descriptor const& d, struct kos_driver_instance& in)
{
    in.task = KOS_TASK_NONE;
    kos_cap_t const* const line = nullptr;
    kos_cap_t const note = KOS_CAP_NONE;
    if (d.expected_base != 0u and in.mmio_base != d.expected_base)
    {
        return instance_failed(d, in, line, 0, note, "ERROR: the instance's window is not this driver's block\n");
    }
    // A block self-granted under another memory type than the descriptor's has no grant to match.
    if (in.block_size != d.block_size or (d.block_size != 0u and in.block == nullptr)
        or in.block_flags != d.block_flags)
    {
        return instance_failed(d, in, line, 0, note, "ERROR: the instance's ring block is not this driver's\n");
    }
    if (in.console != (d.ep_posture == KOS_DRV_EP_HANDOVER))
    {
        return instance_failed(d, in, line, 0, note, "ERROR: the table and the descriptor disagree on the console\n");
    }
    if (in.line_count != d.line_count)
    {
        return instance_failed(d, in, line, 0, note,
                               "ERROR: the table's lines are not as many as this driver's roles\n");
    }
    return 0;
}

// From the task on, the block laid out.
int instance_bring_up(Descriptor const& d, struct kos_driver_instance& in)
{
    kos_cap_t line[KOS_DRV_LINES_MAX] = {KOS_CAP_NONE, KOS_CAP_NONE};
    kos_cap_t note = KOS_CAP_NONE;
    kos_task_t task = KOS_TASK_NONE;
    if (kos_task_create(in.block, in.block_size, d.block_flags, &task) != 0)
    {
        return instance_failed(d, in, line, 0, note, "ERROR: task_create failed\n");
    }
    in.task = task;
    if (kos_task_sched_grant(task, in.ceiling, in.core_mask) != 0)
    {
        return instance_failed(d, in, line, 0, note, "ERROR: task_sched_grant refused the declaration\n");
    }
    if (kos_task_watch(task, in.watch, in.endpoint) != 0)
    {
        return instance_failed(d, in, line, 0, note, "ERROR: task_watch failed\n");
    }
    // PUBLISH BEFORE CLAIM: irq_claim refuses a line while any handler but the default is
    // attached, and only the publish detaches the kernel's own ring from that vector.
    if (d.ep_posture == KOS_DRV_EP_HANDOVER and kos_console_publish(in.endpoint, task) != 0)
    {
        return instance_failed(d, in, line, 0, note, "ERROR: console_publish failed\n");
    }

    uint8_t claimed = 0;
#if KICKOS_KERNEL_CORES > 1
    kos_thread_t const self = kos_thread_self();
    if (d.line_count != 0u and kos_thread_set_affinity(self, in.core_mask) != 0)
    {
        return instance_failed(d, in, line, 0, note, "ERROR: could not pin to the declared core\n");
    }
#endif
    // Claimed HERE: minting needs KOS_AUTH_IRQ and every driver thread runs at authority 0. A
    // line comes back MASKED, and the bound thread's first wait over its bit arms it.
    for (uint8_t i = 0; i < d.line_count; i++)
    {
        if (claim(in.lines[i].number, d.lines[i].trigger, &line[i]) != 0)
        {
            break;
        }
        claimed++;
    }
#if KICKOS_KERNEL_CORES > 1
    // Only the claim needs this thread pinned; the waits are the driver threads'.
    if (d.line_count != 0u)
    {
        (void)kos_thread_set_affinity(self, 0);
    }
#endif
    if (claimed != d.line_count)
    {
        return instance_failed(d, in, line, claimed, note, "ERROR: irq_claim failed\n");
    }

    // ONE object for the whole driver: every line raises a bit of it, and so does every
    // doorbell. LINE i IS BIT i, which is the rule leg L13 holds a doorbell badge above.
    if (notify_used(d))
    {
        if (kos_notify_create(&note) != 0)
        {
            return instance_failed(d, in, line, claimed, KOS_CAP_NONE, "ERROR: notify_create failed\n");
        }
        for (uint8_t i = 0; i < claimed; i++)
        {
            kos_cap_t badged = KOS_CAP_NONE;
            if (kos_notify_badge(note, i, &badged) != 0)
            {
                return instance_failed(d, in, line, claimed, note, "ERROR: notify_badge failed\n");
            }
            int const rc = kos_irq_bind_notify(line[i], badged);
            // The BINDING took a reference of its own, so this name is spent either way.
            kos_handle_close(badged);
            if (rc != 0)
            {
                return instance_failed(d, in, line, claimed, note, "ERROR: irq_bind_notify failed\n");
            }
        }
    }

    // thread_count + 1 barrier positions: barrier_after == thread_count polls AFTER the last
    // spawn, which L8 admits under RETAIN only.
    for (uint8_t i = 0; i <= d.thread_count; i++)
    {
        if (d.ready_offset != KOS_DRV_READY_NONE and i == d.barrier_after
            and not wait_ready(in.block, d.ready_offset))
        {
            return instance_failed(d, in, line, claimed, note, "ERROR: a driver thread never reached its loop\n");
        }
        if (i == d.thread_count)
        {
            break;
        }
        kos::thread::Handle const h = spawn_one(d.threads[i], in, line, note, task, in.core_mask);
        if (not h.valid())
        {
            return instance_failed(d, in, line, claimed, note, "ERROR: driver thread spawn failed\n");
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
    if (d.ep_posture == KOS_DRV_EP_HANDOVER)
    {
        return console_handover_finish(in, d.tag);
    }
    return 0;
}
}

int bring_up(Descriptor const& d, struct kos_driver_instance* in)
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
    int const refused = instance_admits(d, *in);
    if (refused != 0)
    {
        return refused;
    }
    // This function's one indirect call, which tests/static/caller_held_indirect.txt counts.
    if (d.block_size != 0u and d.block_init(in->block, in) != 0)
    {
        return instance_failed(d, *in, nullptr, 0, KOS_CAP_NONE, "ERROR: block_init refused the instance\n");
    }
    return instance_bring_up(d, *in);
}

}
