// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The bring-up of an unprivileged driver service. The contract for each function is stated at
// its declaration in <kickos/sys/driver_service.h>.

#include <kickos/sys/driver_service.h>

#include <kickos/sys/emit.h>
#include <kickos/sys/trap.h>

#include <stdlib.h>
#include <string.h>

namespace kickos::driver
{

int fail(char const* tag, char const* msg)
{
    kos::print(tag);
    kos::print(msg);
    return -1;
}

void report(char const* s)
{
    (void)kickos::kconsole_offer(s, strlen(s));
}

void trap()
{
    while (true)
    {
        KOS_TRAP_ILLEGAL();
    }
}

namespace
{
void waiter(void* raw)
{
    Launch const l = *static_cast<Launch const*>(raw);
    if (not ready_wait(l.latch))
    {
        trap();
    }
    l.entry(l.arg);
}

void* thread_arg(Thread const& t, struct kos_driver_instance const& in)
{
    if (t.arg == KOS_DRV_ARG_BLOCK)
    {
        return in.block;
    }
    if (t.arg == KOS_DRV_ARG_WINDOW)
    {
        return reinterpret_cast<void*>(in.mmio_base);
    }
    if (t.arg == KOS_DRV_ARG_LINE0_INDEX)
    {
        return reinterpret_cast<void*>(line_index_arg(LineIndex{in.lines[0].index}));
    }
    return nullptr;
}

// Thread `t` into the caller's task, each of its roles delegated from the entry thread's place
// of it. Thread 0 of a hosted instance takes its windows and its authority, and each of its
// threads that waits on a role is essential to the task it runs in.
kos::thread::Handle spawn(Thread const& t, bool first, Layout const& l, struct kos_driver_instance const& in,
                          void (*entry)(void*), void* arg)
{
    kos_cap_grant grants[KOS_DRV_CAPS_MAX] = {};
    for (uint8_t i = 0; i < t.cap_count; i++)
    {
        uint8_t const at = l.at[token_of(t.caps[i])];
        grants[i].source_cap = static_cast<kos_cap_t>(KOS_SPAWN_DELEGATED_CAP0 + at);
        if (in.hosted != nullptr)
        {
            grants[i].source_cap = in.hosted[at];
        }
        grants[i].rights_mask = t.caps[i].rights;
    }
    char const* name = t.name;
    if (name == nullptr)
    {
        name = in.name;
    }
    kos_thread_params p{};
    p.entry = entry;
    p.arg = arg;
    p.name = name;
    p.prio = static_cast<uint8_t>(in.priority + t.prio_delta);
    p.policy = KOS_POLICY_FIFO;
    p.caps = grants;
    p.cap_count = t.cap_count;
    p.core_mask = in.core_mask;
    if (first)
    {
        p.windows = in.windows;
        p.window_count = in.window_count;
        p.authority = in.authority;
    }
    if (in.hosted != nullptr and essential(t))
    {
        p.flags = KOS_THREAD_ESSENTIAL;
    }
    kos_thread_t h = KOS_THREAD_NONE;
    int const rc = kos_thread_create(&p, &h);
    return kos::thread::Handle(h, rc);
}

// An instance that is not this descriptor's is refused before anything is made.
int instance_admits(Descriptor const& d, struct kos_driver_instance const& in)
{
    if (d.expected_base != 0u and in.mmio_base != d.expected_base)
    {
        return fail(d.tag, "ERROR: the instance's window is not this driver's block\n");
    }
    // A block self-granted under another memory type than the descriptor's has no grant to match.
    if (in.block_size != d.block_size or (d.block_size != 0u and in.block == nullptr)
        or in.block_flags != d.block_flags)
    {
        return fail(d.tag, "ERROR: the instance's ring block is not this driver's\n");
    }
    if (in.console != (d.ep_posture == KOS_DRV_EP_HANDOVER))
    {
        return fail(d.tag, "ERROR: the table and the descriptor disagree on the console\n");
    }
    if (in.line_count != d.line_count)
    {
        return fail(d.tag, "ERROR: the table's lines are not as many as this driver's roles\n");
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
    if (d.block_size != 0u and d.block_init(in->block, in) != 0)
    {
        return fail(d.tag, "ERROR: block_init refused the instance\n");
    }

    Layout const l = layout_of(d);
    uint8_t first = 1;
    if (in->hosted != nullptr)
    {
        first = 0;
    }
    for (uint8_t i = first; i < d.thread_count; i++)
    {
        Thread const& t = d.threads[i];
        void (*entry)(void*) = t.entry;
        void* arg = thread_arg(t, *in);
        if (d.ready_offset != KOS_DRV_READY_NONE and i >= d.barrier_after)
        {
            d.launch[i] = Launch{t.entry, arg, ready_latch(in->block, d.ready_offset)};
            entry = waiter;
            arg = &d.launch[i];
        }
        if (spawn(t, i == 0u, l, *in, entry, arg).valid())
        {
            continue;
        }
        (void)fail(d.tag, "ERROR: driver thread spawn failed\n");
        if (i == 0u)
        {
            return -1;
        }
        trap();
    }
    if (in->hosted != nullptr)
    {
        if (d.ready_offset != KOS_DRV_READY_NONE
            and not ready_wait_for(ready_latch(in->block, d.ready_offset), KOS_DRV_READY_SERVICE))
        {
            (void)fail(d.tag, "ERROR: the driver's service never reached its first receive\n");
            trap();
        }
        return 0;
    }

    // Each spawned thread holds its own copies now: thread 0 keeps its roles at its rights and
    // gives back the rest.
    Thread const& t0 = d.threads[0];
    for (uint8_t j = 0; j < t0.cap_count; j++)
    {
        (void)kos_cap_narrow(static_cast<kos_cap_t>(KOS_SPAWN_DELEGATED_CAP0 + j), t0.caps[j].rights);
    }
    for (uint8_t j = t0.cap_count; j < l.count; j++)
    {
        (void)kos_handle_close(static_cast<kos_cap_t>(KOS_SPAWN_DELEGATED_CAP0 + j));
    }
    if (t0.prio_delta != 0
        and kos_thread_set_priority(static_cast<uint8_t>(in->priority + t0.prio_delta)) != 0)
    {
        (void)fail(d.tag, "ERROR: thread 0 could not take its priority\n");
        trap();
    }
    t0.entry(thread_arg(t0, *in));
    kos_exit(0);
}

}
