// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The bring-up choreography of an unprivileged driver service, once for every class and
// every chip: allocate and self-grant the shared block, create the endpoint, publish or
// retain it, claim the IRQ lines, spawn the threads with their per-thread grants and cap
// roles, poll the readiness latch strictly between two spawns, unwind on any failure, and
// finish a console handover.
//
// A class enters only as a thread-entry pointer and as the per-chip block_init. No chip
// header is included here: a descriptor is authored in the per-chip TU, the only one with
// REGDIR on its include path.

#ifndef KICKOS_SYS_DRIVER_SERVICE_H
#define KICKOS_SYS_DRIVER_SERVICE_H

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/cap_index.h> // KOS_CAP_STDOUT
#include <kickos/sys/errno.h>
#include <kickos/sys/service.h>

#include <stdint.h>
#include <stdlib.h>

namespace kickos
{
namespace driver
{

enum
{
    KOS_DRV_LINES_MAX = 2,
    KOS_DRV_THREADS_MAX = 3,
    KOS_DRV_CAPS_MAX = 2
};

// What a per-thread cap entry NAMES. A thread's caps[] index IS its child cap index offset
// from KOS_SPAWN_DELEGATED_CAP0: caps[0] lands at index 1, caps[1] at index 2.
enum
{
    KOS_DRV_RES_EP = 0,
    KOS_DRV_RES_LINE0 = 1,
    KOS_DRV_RES_LINE1 = 2
};

struct Cap
{
    uint8_t resource; // KOS_DRV_RES_EP, or KOS_DRV_RES_LINE0 + i
    uint8_t rights;   // a kos_cap_rights subset
};

struct Line
{
    int32_t number;  // the chip vector; REGDIR-private
    uint8_t trigger; // KOS_IRQ_EDGE or KOS_IRQ_LEVEL
};

// WHICH POINTER THE ENTRY RECEIVES, and nothing about reach: the block is the GROUP's
// region (Descriptor::block_size), so ARG_NONE buys a thread no isolation from it.
enum kos_drv_arg
{
    KOS_DRV_ARG_NONE = 0,
    KOS_DRV_ARG_BLOCK = 1, // the granted ring block pointer
    KOS_DRV_ARG_WINDOW = 2 // cfg->mmio_base as a VALUE, never dereferenced as memory
};

struct Thread
{
    void (*entry)(void*);
    char const* name; // null takes cfg->name
    int8_t prio_delta;
    uint8_t arg;       // enum kos_drv_arg
    bool window_grant; // cfg->mmio_base + cfg->mmio_window; a DEV window has one holder
    uint8_t cap_count;
    struct Cap caps[KOS_DRV_CAPS_MAX];
};

enum kos_drv_ep
{
    // kos_console_publish, then the handover tail. From the publish on the console is
    // USER_OWNED and a kernel-console write is DROPPED until closing E takes recv_holders
    // to 0, so every diagnostic below closes before it prints.
    KOS_DRV_EP_HANDOVER = 0,
    // No publish; root keeps a full-rights cap for the app to narrow per client. Root
    // therefore holds a WAIT-bearing cap forever, so recv_holders never reaches 0 and the
    // last-receiver-gone EPIPE wake never fires: NO failure path in a driver thread under
    // this posture may exit(), it must panic.
    KOS_DRV_EP_RETAIN = 1
};

// No readiness latch in the block, so no barrier.
constexpr uint16_t KOS_DRV_READY_NONE = 0xFFFFu;

struct Descriptor
{
    char const* tag;         // "[c6uart] ", prefixed to every diagnostic this bring-up prints
    uintptr_t expected_base; // 0 = no guard; no granted window has base 0
    // 0 = no ring block, no arena allocation, no self-grant. THE BLOCK IS THE GROUP'S SHARED
    // REGION AND EVERY THREAD OF THIS DRIVER SEES ALL OF IT, whatever its arg: a task owns
    // exactly one Domain and a member may bring no grant of its own. Keep here only state the
    // whole driver may touch; a DEV window, which has one holder, is per-thread instead.
    uint32_t block_size;
    uint16_t ready_offset;   // byte offset of a `volatile uint32_t` latch inside the block
    uint8_t ep_posture;      // enum kos_drv_ep
    uint8_t svc_kind;        // enum kos_svc_kind
    uint8_t line_count;
    uint8_t thread_count;
    uint8_t barrier_after; // threads spawned BEFORE the readiness poll
    struct Line lines[KOS_DRV_LINES_MAX];
    struct Thread threads[KOS_DRV_THREADS_MAX];
    // Lays out the block and fills the class config from the cfg. Null iff block_size == 0.
    int (*block_init)(void* blk, struct kos_service_cfg const* cfg);
};

// ---------------------------------------------------------------------------------
// The validator.

// True when `t` holds `resource` carrying every bit of `rights`.
constexpr bool holds(Thread const& t, uint8_t resource, uint8_t rights)
{
    for (uint8_t i = 0; i < t.cap_count; i++)
    {
        if (t.caps[i].resource == resource and (t.caps[i].rights & rights) == rights)
        {
            return true;
        }
    }
    return false;
}

// Index of the thread that receives on the endpoint, or thread_count when none does.
constexpr uint8_t ep_holder(Descriptor const& d)
{
    for (uint8_t i = 0; i < d.thread_count; i++)
    {
        if (holds(d.threads[i], KOS_DRV_RES_EP, KOS_CAP_WAIT))
        {
            return i;
        }
    }
    return d.thread_count;
}

constexpr uint8_t ep_holder_count(Descriptor const& d)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < d.thread_count; i++)
    {
        if (holds(d.threads[i], KOS_DRV_RES_EP, KOS_CAP_WAIT))
        {
            n++;
        }
    }
    return n;
}

constexpr uint8_t window_holder_count(Descriptor const& d)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < d.thread_count; i++)
    {
        if (d.threads[i].window_grant)
        {
            n++;
        }
    }
    return n;
}

constexpr uint8_t line_waiter_count(Descriptor const& d, uint8_t l)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < d.thread_count; i++)
    {
        if (holds(d.threads[i], static_cast<uint8_t>(KOS_DRV_RES_LINE0 + l), KOS_CAP_WAIT))
        {
            n++;
        }
    }
    return n;
}

// L1. Bounds every index the legs below take. thread_count == 0 is L6's to refuse.
constexpr bool valid_l1(Descriptor const& d)
{
    return d.thread_count <= KOS_DRV_THREADS_MAX and d.line_count <= KOS_DRV_LINES_MAX;
}

// L2. A cap naming a line the descriptor does not claim, or granting no right at all.
constexpr bool valid_l2(Descriptor const& d)
{
    for (uint8_t i = 0; i < d.thread_count; i++)
    {
        if (d.threads[i].cap_count > KOS_DRV_CAPS_MAX)
        {
            return false;
        }
        for (uint8_t j = 0; j < d.threads[i].cap_count; j++)
        {
            if (d.threads[i].caps[j].resource >= 1u + d.line_count)
            {
                return false;
            }
            if (d.threads[i].caps[j].rights == 0u)
            {
                return false;
            }
        }
    }
    return true;
}

// L3. A DEV window has exactly one holder: a second spawn asking for it is refused -KOS_EBUSY.
//
// Second arm: spawn_one hands cfg->mmio_base to an ARG_WINDOW thread whether or not that
// thread was granted the window, so one without the grant faults on its first register touch.
constexpr bool valid_l3(Descriptor const& d)
{
    if (window_holder_count(d) > 1u)
    {
        return false;
    }
    for (uint8_t i = 0; i < d.thread_count; i++)
    {
        if (d.threads[i].arg == KOS_DRV_ARG_WINDOW and not d.threads[i].window_grant)
        {
            return false;
        }
    }
    return true;
}

// L4. A thread handed a block nobody allocated, a block nothing lays out, or a block granted
// to the group that no thread ever reads.
//
// The second arm is the one that keeps the grant narrow: the block becomes a region on every
// member, so a descriptor carrying one nobody takes would hand the whole group memory for
// nothing. It is the only statement a descriptor makes about the group's memory.
constexpr bool valid_l4(Descriptor const& d)
{
    bool reader = false;
    for (uint8_t i = 0; i < d.thread_count; i++)
    {
        if (d.threads[i].arg == KOS_DRV_ARG_BLOCK)
        {
            if (d.block_size == 0u)
            {
                return false;
            }
            reader = true;
        }
    }
    if (d.block_size != 0u and not reader)
    {
        return false;
    }
    return (d.block_size == 0u) == (d.block_init == nullptr);
}

// L5, THE RELAY RULE. Only the window holder can clear a peripheral flag, so a thread that
// waits on a line while holding no window cannot serve a LEVEL source: it would rearm into
// a still-asserted line and spin.
constexpr bool valid_l5(Descriptor const& d)
{
    for (uint8_t i = 0; i < d.thread_count; i++)
    {
        if (d.threads[i].window_grant)
        {
            continue;
        }
        for (uint8_t j = 0; j < d.threads[i].cap_count; j++)
        {
            uint8_t const res = d.threads[i].caps[j].resource;
            if (res == KOS_DRV_RES_EP)
            {
                continue;
            }
            if ((d.threads[i].caps[j].rights & KOS_CAP_WAIT) == 0u)
            {
                continue;
            }
            if (d.lines[res - KOS_DRV_RES_LINE0].trigger != KOS_IRQ_EDGE)
            {
                return false;
            }
        }
    }
    return true;
}

// L6. Two receivers on the request endpoint, or none.
constexpr bool valid_l6(Descriptor const& d)
{
    return ep_holder_count(d) == 1u;
}

// L7. A console handover with no readiness latch has no reportable window, because L8 puts
// the poll strictly BEFORE the endpoint's receiver exists. A one-thread driver has no such
// window at all, its only thread being that receiver, so L7 and L8 would be jointly
// unsatisfiable: there the handover probe is the witness instead.
constexpr bool valid_l7(Descriptor const& d)
{
    return d.ep_posture != KOS_DRV_EP_HANDOVER or d.thread_count == 1u
           or d.ready_offset != KOS_DRV_READY_NONE;
}

// L8, THE BARRIER RULE, and the latch's own arithmetic.
//
// POSTURELESS arms: barrier_after == 0 would poll before any thread could have latched, and
// barrier_after > thread_count names no barrier position at all. An unaligned
// `volatile uint32_t` load is tolerated on ARMv7-M and FAULTS on RX and Xtensa.
//
// HANDOVER-ONLY arms: the poll must sit STRICTLY between the spawns, because once the
// endpoint's receiver exists recv_holders never reaches 0, nothing reclaims the console, and
// the timeout diagnostic goes to an endpoint nobody drains. Under RETAIN recv_holders never
// reaches 0 in the first place and there is no console to reclaim, so a one-thread RETAIN bus
// may latch after its only spawn and its receiver may be thread 0.
//
// The ep_holder arm is the SOLE rejecter of a receiver spawned before the barrier.
constexpr bool valid_l8(Descriptor const& d)
{
    if (d.ready_offset == KOS_DRV_READY_NONE)
    {
        return true;
    }
    if (d.block_size == 0u or d.ready_offset + 4u > d.block_size)
    {
        return false;
    }
    if (d.ready_offset % 4u != 0u)
    {
        return false;
    }
    if (d.barrier_after < 1u or d.barrier_after > d.thread_count)
    {
        return false;
    }
    if (d.ep_posture != KOS_DRV_EP_HANDOVER)
    {
        return true;
    }
    if (d.barrier_after >= d.thread_count)
    {
        return false;
    }
    return ep_holder(d) >= d.barrier_after;
}

// L9, THE BASE-PIN RULE. A driver that claims a vector BY NUMBER is hard-wired to one
// peripheral instance, so a cfg naming another window would grant one block and interrupt
// on another.
constexpr bool valid_l9(Descriptor const& d)
{
    return d.line_count == 0u or window_holder_count(d) == 0u or d.expected_base != 0u;
}

// L10. A half-authored descriptor.
// A null entry is NOT checked here: an entry that is forward-declared at the descriptor,
// which the sim console does deliberately, has no constant address under
// -fsanitize=undefined, so testing it makes valid() non-constant. bring_up checks it.
constexpr bool valid_l10(Descriptor const& d)
{
    return d.tag != nullptr;
}

// L11. bring_up publishes the endpoint AS THE CONSOLE under HANDOVER, so any other kind
// would route every stdout writer on the board at a bus endpoint.
constexpr bool valid_l11(Descriptor const& d)
{
    return d.ep_posture != KOS_DRV_EP_HANDOVER or d.svc_kind == KOS_SVC_CONSOLE;
}

// L12, THE LINE-ROLE RULE. A claimed line comes back MASKED and only its waiter's first
// irq_wait arms it, so a line no thread waits on stays masked forever and every event on it
// is lost with no diagnostic. Two line caps swapped breaks the count on BOTH lines at once,
// which is why this leg needs no per-chip knowledge of which line is transmit.
constexpr bool valid_l12(Descriptor const& d)
{
    for (uint8_t l = 0; l < d.line_count; l++)
    {
        if (line_waiter_count(d, l) != 1u)
        {
            return false;
        }
    }
    return true;
}

// The legs are ordered, not just conjoined: L1 bounds the counts L2 walks, and L2 bounds the
// resource ids L5 and L12 use to index lines[] and caps[].
constexpr bool valid(Descriptor const& d)
{
    return valid_l1(d) and valid_l2(d) and valid_l3(d) and valid_l4(d) and valid_l5(d)
           and valid_l6(d) and valid_l7(d) and valid_l8(d) and valid_l9(d) and valid_l10(d)
           and valid_l11(d) and valid_l12(d);
}

// ---------------------------------------------------------------------------------
// NOT a leg of valid(): the cap layout uart_service.h and usb_cdc_service.h SHARE and
// spi_service.h does not. A ring plus a doorbell, one service thread receiving on caps[0] and
// ringing caps[1], every other thread parked in irq_wait on its own caps[0] and, when it
// relays, posting its own caps[1].
//
// `ready_offset` and `block_size` are the CALLER's class constants: a descriptor writing
// either as a literal is what the first arm refuses.
constexpr bool ring_doorbell_shape_ok(Descriptor const& d, uint16_t ready_offset,
                                      uint32_t block_size)
{
    if (d.ready_offset != ready_offset or d.block_size != block_size)
    {
        return false;
    }
    uint8_t const svc = ep_holder(d);
    if (svc >= d.thread_count)
    {
        return false;
    }
    // The window belongs to the IRQ thread, never the service thread. L5 catches this only
    // when the line happens to be LEVEL.
    if (d.threads[svc].window_grant)
    {
        return false;
    }
    if (d.threads[svc].cap_count != 2u)
    {
        return false;
    }
    if (d.threads[svc].caps[0].resource != KOS_DRV_RES_EP)
    {
        return false;
    }
    if (d.threads[svc].caps[1].resource == KOS_DRV_RES_EP
        or (d.threads[svc].caps[1].rights & KOS_CAP_SIGNAL) == 0u)
    {
        return false;
    }
    for (uint8_t i = 0; i < d.thread_count; i++)
    {
        if (i == svc)
        {
            continue;
        }
        if (d.threads[i].cap_count == 0u
            or d.threads[i].caps[0].resource == KOS_DRV_RES_EP
            or (d.threads[i].caps[0].rights & KOS_CAP_WAIT) == 0u)
        {
            return false;
        }
        if (d.threads[i].cap_count < 2u)
        {
            continue;
        }
        // edge_relay_thread posts KOS_SPAWN_DELEGATED_CAP0 + 1 unconditionally.
        if (d.threads[i].caps[1].resource == KOS_DRV_RES_EP
            or (d.threads[i].caps[1].rights & KOS_CAP_SIGNAL) == 0u)
        {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------
inline int fail(char const* tag, char const* msg)
{
    kos::print(tag);
    kos::print(msg);
    return -1;
}

constexpr uint32_t KOS_DRV_HANDOVER_PROBE_US = 1000000;

// The LAST two steps of a console handover, in this order because either order wrong fails
// silently. Closes the caller's own WAIT-bearing cap on E, then PROVES a driver is serving
// before any console client runs. Returns 0, or the probe's negative rc.
//
// Closing FIRST leaves the driver as the SOLE receiver, so its death takes recv_holders to 0,
// which both EPIPEs the probe AND reclaims the console. The probe is a ZERO-LENGTH rendezvous
// on cap 0, the same route every client uses.
//
// -KOS_EPIPE means a SERVICE thread died. The console comes back only once the register
// window is free, and the window holder may still be alive, so the whole GROUP is ended
// before the tag is printed -- ONE call, because the threads are one task.
//
// Any other refusal, -KOS_ETIMEDOUT above all, leaves the service thread ALIVE and still the
// sole receiver: recv_holders never reaches 0, the console is NOT reclaimed, and nothing here
// recovers. The code is returned unchanged and no tag is printed.
inline int console_handover_finish(kos_cap_t ep, char const* tag, kos_task_t task)
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

// A cap-to-cap edge converter: wait on one line, post another. PRECONDITION, enforced by leg
// L5: the waited line must be EDGE. This thread holds no window, so it cannot clear a
// peripheral flag, and on a LEVEL source it would rearm into a still-asserted line and spin.
inline void edge_relay_thread(void*)
{
    while (true)
    {
        if (kos_irq_wait(KOS_SPAWN_DELEGATED_CAP0) != 0)
        {
            break; // the cap went away: no line left to relay
        }
        (void)kos_irq_notify(KOS_SPAWN_DELEGATED_CAP0 + 1);
    }
    exit(0);
}

constexpr uint32_t KOS_DRV_READY_WAIT_NS = 1000000u; // 1 ms
constexpr uint32_t KOS_DRV_READY_WAIT_MAX = 1000u;   // ~1 s total

// The latch MUST be a `volatile uint32_t` at this offset, so the offset comes from a
// class-substrate constant and never from a descriptor literal.
inline bool wait_ready(void const* blk, uint16_t off)
{
    volatile uint32_t const* const flag = reinterpret_cast<volatile uint32_t const*>(
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

// CLOSE BEFORE CANCELLING AND BEFORE PRINTING. Closing takes the endpoint's last receiver
// holder to 0, which notes the console dead and reclaims it, so the tag the caller prints
// next reaches the wire; and the note must already be set when a cancelled thread's exit
// runs the reclaim.
inline void unwind(kos_cap_t const* line, uint8_t claimed, kos_cap_t ep, kos_task_t task)
{
    for (uint8_t i = 0; i < claimed; i++)
    {
        kos_handle_close(line[i]);
    }
    kos_handle_close(ep);
    // Ends every member AND drops root's hold, so an abandoned bring-up leaves neither a live
    // thread nor a reserved task slot. Legal on a group that never got a member.
    (void)kos_task_kill(task);
}

inline kos::thread::Handle spawn_one(Thread const& t, struct kos_service_cfg const* cfg,
                                     void* blk, kos_cap_t ep, kos_cap_t const* line,
                                     kos_task_t task)
{
    kos_cap_grant grants[KOS_DRV_CAPS_MAX] = {};
    for (uint8_t i = 0; i < t.cap_count; i++)
    {
        if (t.caps[i].resource == KOS_DRV_RES_EP)
        {
            grants[i].source_cap = ep;
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
    // bringing one is refused -KOS_EINVAL. The window is the per-thread grant.
    return kos::thread::spawn(t.entry, arg, name,
                              static_cast<uint8_t>(cfg->prio + t.prio_delta),
                              KOS_POLICY_FIFO, /*quantum_ns=*/0, /*privileged=*/false,
                              /*mem=*/nullptr, /*mem_size=*/0,
                              /*stack=*/nullptr, /*stack_size=*/0,
                              win, win_size, grants, t.cap_count,
                              /*authority=*/0, /*cap_dest=*/nullptr, task);
}

// `out_ep` receives the retained endpoint under KOS_DRV_EP_RETAIN and must be null under
// HANDOVER. valid() cannot check that pairing, out_ep being a runtime pointer.
inline int bring_up(Descriptor const& d, struct kos_service_cfg const* cfg, kos_cap_t* out_ep)
{
    // Every index below is bounded by a leg of valid(): a driver that omits its
    // static_assert, or a descriptor whose line number stops being constexpr, would
    // otherwise write past line[] or ThreadSet::t[] on ROOT's stack. d.tag is L10's own
    // subject, so a descriptor that failed only L10 would reach fail() with a null tag.
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
        if (kos_mem_self_grant(blk, d.block_size) != 0)
        {
            return fail(d.tag, "ERROR: mem_self_grant of the ring block refused\n");
        }
        if (d.block_init(blk, cfg) != 0)
        {
            return fail(d.tag, "ERROR: block_init refused the cfg\n");
        }
    }

    // THE GROUP. Every thread of this driver joins it, so a peer's death ends the rest and one
    // call ends them all -- which is what lets the unwind below name no thread at all. Created
    // BEFORE the endpoint, so the earliest failure that has a task to give back is the first
    // one that has anything to give back.
    //
    // ITS SHARED REGION IS THE WHOLE BLOCK, FOR EVERY MEMBER. A task owns exactly one Domain
    // and a member may bring no grant of its own, so there is no per-thread subset to declare:
    // a thread that must not reach the block needs a task of its own, which today would also
    // take it out of the kill group. A driver with no block gets a group that is only a kill
    // group. L4 is what keeps this narrow, by refusing a block no thread reads.
    kos_task_t task = KOS_TASK_NONE;
    if (kos_task_create(blk, d.block_size, &task) != 0)
    {
        return fail(d.tag, "ERROR: task_create failed\n");
    }

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
        // 0. A line comes back MASKED, and the waiting thread's first irq_wait arms it.
        if (kos_irq_claim(d.lines[i].number, d.lines[i].trigger, &line[i]) != 0)
        {
            unwind(line, claimed, ep, task);
            return fail(d.tag, "ERROR: irq_claim failed\n");
        }
        claimed++;
    }

    // thread_count + 1 barrier positions, not thread_count: barrier_after == thread_count
    // polls AFTER the last spawn, the only readiness window a one-thread service has, which
    // L8 admits under RETAIN only.
    for (uint8_t i = 0; i <= d.thread_count; i++)
    {
        if (d.ready_offset != KOS_DRV_READY_NONE and i == d.barrier_after)
        {
            if (not wait_ready(blk, d.ready_offset))
            {
                unwind(line, claimed, ep, task);
                return fail(d.tag, "ERROR: a driver thread never reached its loop\n");
            }
        }
        if (i == d.thread_count)
        {
            break;
        }
        if (not spawn_one(d.threads[i], cfg, blk, ep, line, task).valid())
        {
            unwind(line, claimed, ep, task);
            return fail(d.tag, "ERROR: driver thread spawn failed\n");
        }
    }

    // With the driver threads the only holders, a line returns to the pool when they die.
    for (uint8_t i = 0; i < claimed; i++)
    {
        kos_handle_close(line[i]);
    }

    if (d.ep_posture == KOS_DRV_EP_RETAIN)
    {
        *out_ep = ep;
        return 0;
    }
    return console_handover_finish(ep, d.tag, task);
}

} // namespace driver
} // namespace kickos

#endif
