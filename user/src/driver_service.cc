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
namespace
{
#if KICKOS_KERNEL_CORES > 1
// A thread handling a line does not migrate: above one kernel core the claim is refused to a
// caller not pinned where it runs, and a wait, ack or discard to a thread not pinned to the
// claim core. On a service list core 0, which is in every grant and no image isolates.
constexpr uint32_t LINE_CORES = 1u << 0;
#endif

// Bit 0 of a thread's spawn arg: given an instance, every descriptor thread carries it.
constexpr uintptr_t UNDER_INIT = 1u;

// Written by this space's own driver threads through thread_start, never by the bring-up: on a
// translating board the bring-up runs in the init's space, not the driver's.
Atomic<uint32_t, Order::RELAXED> g_under_init{0u};

// What the init keeps of a console's endpoint once its handover ends: SIGNAL, TRANSFER, HANDOUT.
constexpr uint32_t HANDOVER_KEPT = KOS_CAP_SIGNAL | KOS_CAP_TRANSFER | KOS_CAP_HANDOUT;
}

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

void* thread_start(void* arg)
{
    uintptr_t const word = reinterpret_cast<uintptr_t>(arg);
    g_under_init = static_cast<uint32_t>(word & UNDER_INIT);
    return reinterpret_cast<void*>(word & ~UNDER_INIT);
}

void trap_under_init()
{
    if (g_under_init != 0u)
    {
        trap();
    }
}

int console_handover_finish(kos_cap_t ep, char const* tag, kos_task_t task,
                            struct kos_driver_instance const* instance)
{
    if (instance != nullptr)
    {
        (void)kos_cap_narrow(ep, HANDOVER_KEPT);
        int const probe = kos_send_timed(KOS_CAP_STDOUT, "", 0, KOS_DRV_HANDOVER_PROBE_US);
        if (probe >= 0)
        {
            return 0;
        }
        (void)fail(tag, "ERROR: the console handover probe was not taken\n");
        return probe;
    }
    kos_handle_close(ep);
    int const rc = kos_send_timed(KOS_CAP_STDOUT, "", 0, KOS_DRV_HANDOVER_PROBE_US);
    if (rc >= 0)
    {
        return 0;
    }
    if (rc != -KOS_ECONNREFUSED)
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
                              kos_cap_t ep, kos_cap_t const* line, uint16_t line0_index,
                              kos_cap_t note, kos_task_t task, uint32_t core_mask, bool under_init)
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
    else if (t.arg == KOS_DRV_ARG_LINE0_INDEX)
    {
        arg = reinterpret_cast<void*>(line_index_arg(line0_index));
    }
    if (under_init)
    {
        arg = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(arg) | UNDER_INIT);
    }

    kos_window const win = {cfg->mmio_base, cfg->mmio_window, KOS_WINDOW_DEVICE, 0};
    uint16_t win_count = 0;
    if (t.window_grant)
    {
        win_count = 1;
    }

    char const* name = t.name;
    if (name == nullptr)
    {
        name = cfg->name;
    }

    // On a service list a thread holding a line, or the notification claimed lines signal, is
    // pinned to their claim core.
#if KICKOS_KERNEL_CORES > 1
    if (core_mask == 0u and line != nullptr and line[0] != KOS_CAP_NONE)
    {
        for (uint8_t i = 0; i < t.cap_count; i++)
        {
            if (t.caps[i].resource != KOS_DRV_RES_EP)
            {
                core_mask = LINE_CORES;
            }
        }
    }
#endif

    // No mem grant of its own: the ring block is the TASK's shared region, and a member
    // bringing one is refused -KOS_EINVAL.
    auto h = kos::thread::create(t.entry, arg, name,
                                 static_cast<uint8_t>(cfg->prio + t.prio_delta),
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
int instance_failed(Descriptor const& d, struct kos_driver_instance const& in, kos_cap_t const* line,
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
        (void)kos_cap_narrow(in.endpoint, HANDOVER_KEPT);
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
int instance_admits(Descriptor const& d, struct kos_service_cfg const* cfg, struct kos_driver_instance& in)
{
    in.task = KOS_TASK_NONE;
    kos_cap_t const* const line = nullptr;
    kos_cap_t const note = KOS_CAP_NONE;
    if (d.expected_base != 0u and cfg->mmio_base != d.expected_base)
    {
        return instance_failed(d, in, line, 0, note, "ERROR: cfg mmio_base is not this driver's block\n");
    }
    // A block self-granted under another memory type than the descriptor's has no grant to match.
    if (in.block_size != d.block_size or (d.block_size != 0u and in.block == nullptr)
        or in.block_flags != d.block_flags)
    {
        return instance_failed(d, in, line, 0, note, "ERROR: the instance's ring block is not this driver's\n");
    }
    // Bit 0 of a thread's arg carries the posture, so no arg it declares may be odd.
    if (((reinterpret_cast<uintptr_t>(in.block) | cfg->mmio_base) & UNDER_INIT) != 0u)
    {
        return instance_failed(d, in, line, 0, note, "ERROR: an odd block or window base cannot carry the posture\n");
    }
    if (in.console != (d.ep_posture == KOS_DRV_EP_HANDOVER))
    {
        return instance_failed(d, in, line, 0, note, "ERROR: the table and the descriptor disagree on the console\n");
    }
    if (in.line_count != d.line_count)
    {
        return instance_failed(d, in, line, 0, note, "ERROR: the table's lines are not as many as this driver's roles\n");
    }
    return 0;
}

// From the task on, the block laid out.
int instance_bring_up(Descriptor const& d, struct kos_service_cfg const* cfg,
                      struct kos_driver_instance& in)
{
    kos_cap_t line[KOS_DRV_LINES_MAX] = {KOS_CAP_NONE, KOS_CAP_NONE};
    kos_cap_t note = KOS_CAP_NONE;
    kos_task_t task = KOS_TASK_NONE;
    if (kos_task_create(in.block, in.block_size, d.block_flags, &task) != 0)
    {
        return instance_failed(d, in, line, 0, note, "ERROR: task_create failed\n");
    }
    in.task = task;
    int8_t top = d.threads[0].prio_delta;
    for (uint8_t i = 1; i < d.thread_count; i++)
    {
        if (d.threads[i].prio_delta > top)
        {
            top = d.threads[i].prio_delta;
        }
    }
    if (kos_task_sched_grant(task, static_cast<uint8_t>(cfg->prio + top), in.core_mask) != 0)
    {
        return instance_failed(d, in, line, 0, note, "ERROR: task_sched_grant refused the declaration\n");
    }
    if (kos_task_watch(task, in.watch, in.endpoint) != 0)
    {
        return instance_failed(d, in, line, 0, note, "ERROR: task_watch failed\n");
    }
    // PUBLISH BEFORE CLAIM, as on a service list.
    if (d.ep_posture == KOS_DRV_EP_HANDOVER and kos_console_publish(in.endpoint) != 0)
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
    for (uint8_t i = 0; i < d.line_count; i++)
    {
        if (claim(in.lines[i].number, d.lines[i].trigger, &line[i]) != 0)
        {
            break;
        }
        claimed++;
    }
#if KICKOS_KERNEL_CORES > 1
    if (d.line_count != 0u)
    {
        (void)kos_thread_set_affinity(self, 0);
    }
#endif
    if (claimed != d.line_count)
    {
        return instance_failed(d, in, line, claimed, note, "ERROR: irq_claim failed\n");
    }

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
            kos_handle_close(badged);
            if (rc != 0)
            {
                return instance_failed(d, in, line, claimed, note, "ERROR: irq_bind_notify failed\n");
            }
        }
    }

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
        kos::thread::Handle const h = spawn_one(d.threads[i], cfg, in.block, in.endpoint, line, in.lines[0].index,
                                                note, task, in.core_mask, true);
        if (not h.valid())
        {
            return instance_failed(d, in, line, claimed, note, "ERROR: driver thread spawn failed\n");
        }
    }

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
        return console_handover_finish(in.endpoint, d.tag, task, &in);
    }
    return 0;
}

// A service list's cfg and posture, and the ring block it allocates and self-grants into *blk.
int admit_service_list(Descriptor const& d, struct kos_service_cfg const* cfg, kos_cap_t const* out_ep, void** blk)
{
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
    if (d.block_size != 0u)
    {
        // ONE power-of-two, naturally aligned: the RAM arm of the grant predicate demands it
        // of root too.
        *blk = kos_ram_alloc(d.block_size);
        if (*blk == nullptr)
        {
            return fail(d.tag, "ERROR: arena cannot spare the ring block\n");
        }
        // Reach it before writing it: kos_ram_alloc grants nothing, and under enforcement
        // root's own region set does not cover the arena.
        if (kos_mem_self_grant(*blk, d.block_size, d.block_flags) != 0)
        {
            return fail(d.tag, "ERROR: mem_self_grant of the ring block refused\n");
        }
    }
    return 0;
}
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
    if (cfg != nullptr
        and (cfg->rsv[0] != 0u or cfg->rsv[1] != 0u or cfg->rsv[2] != 0u or cfg->rsv[3] != 0u))
    {
        return fail(d.tag, "ERROR: the service cfg's reserved bytes are not zero\n");
    }
    bool const under_init = cfg != nullptr and cfg->instance != nullptr;
    void* blk = nullptr;
    if (under_init)
    {
        int const refused = instance_admits(d, cfg, *cfg->instance);
        if (refused != 0)
        {
            return refused;
        }
        blk = cfg->instance->block;
    }
    else
    {
        int const refused = admit_service_list(d, cfg, out_ep, &blk);
        if (refused != 0)
        {
            return refused;
        }
    }
    // This function's one indirect call, which tests/static/caller_held_indirect.txt counts.
    if (d.block_size != 0u and d.block_init(blk, cfg) != 0)
    {
        if (under_init)
        {
            return instance_failed(d, *cfg->instance, nullptr, 0, KOS_CAP_NONE, "ERROR: block_init refused the cfg\n");
        }
        return fail(d.tag, "ERROR: block_init refused the cfg\n");
    }
    if (under_init)
    {
        return instance_bring_up(d, cfg, *cfg->instance);
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
#if KICKOS_KERNEL_CORES > 1
    kos_thread_t const self = kos_thread_self();
    if (d.line_count != 0u)
    {
        if (kos_thread_set_affinity(self, LINE_CORES) != 0)
        {
            unwind(line, claimed, ep, note, task);
            return fail(d.tag, "ERROR: could not pin to the core the lines are claimed on\n");
        }
    }
#endif
    for (uint8_t i = 0; i < d.line_count; i++)
    {
        // Claimed HERE: minting needs KOS_AUTH_IRQ and every driver thread runs at authority
        // 0. A line comes back MASKED, and the bound thread's first wait over its bit arms
        // it.
        if (kos_irq_claim(d.lines[i].number, d.lines[i].trigger, &line[i]) != 0)
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
        unwind(line, claimed, ep, note, task);
        return fail(d.tag, "ERROR: irq_claim failed\n");
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
        if (not spawn_one(d.threads[i], cfg, blk, ep, line, d.lines[0].index, note, task, 0u, false).valid())
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
    return console_handover_finish(ep, d.tag, task, nullptr);
}

}
