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

namespace kickos::driver
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
    uint16_t ready_offset;   // byte offset of the readiness latch inside the block
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
// barrier_after > thread_count names no barrier position at all. An unaligned 32-bit load is
// tolerated on ARMv7-M and FAULTS on RX and Xtensa.
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
// Print `tag` then `msg` and return -1, the bring-up failure code.
int fail(char const* tag, char const* msg);

constexpr uint32_t KOS_DRV_HANDOVER_PROBE_US = 1000000;

// The last two steps of a console handover: close the caller's own WAIT-bearing cap on E,
// then probe with a zero-length rendezvous on cap 0. Returns 0, or the probe's negative rc.
//
// THE ORDER MATTERS: closing first leaves the driver the SOLE receiver, so its death takes
// recv_holders to 0, which both EPIPEs the probe and reclaims the console. On any refusal but
// -KOS_EPIPE the service thread is still alive and still holds the console, and nothing here
// recovers.
int console_handover_finish(kos_cap_t ep, char const* tag, kos_task_t task);

// A cap-to-cap edge converter: wait on one line, post another. PRECONDITION, enforced by leg
// L5: the waited line must be EDGE.
void edge_relay_thread(void*);

constexpr uint32_t KOS_DRV_READY_WAIT_NS = 1000000u; // 1 ms
constexpr uint32_t KOS_DRV_READY_WAIT_MAX = 1000u;   // ~1 s total

// Poll the readiness latch at `off` inside `blk` until it is set or the budget runs out. The
// latch MUST be an Atomic<uint32_t, Order::RELAXED>, so `off` comes from a class-substrate
// constant and never from a descriptor literal.
bool wait_ready(void const* blk, uint16_t off);

// Give back everything a failed bring-up took: the claimed lines, the endpoint, the group.
// CLOSE BEFORE CANCELLING AND BEFORE PRINTING: closing takes the endpoint's last receiver
// holder to 0, which notes the console dead and reclaims it, so the tag the caller prints
// next reaches the wire.
void unwind(kos_cap_t const* line, uint8_t claimed, kos_cap_t ep, kos_task_t task);

// Spawn one descriptor thread into `task` with its per-thread grants and cap roles.
kos::thread::Handle spawn_one(Thread const& t, struct kos_service_cfg const* cfg, void* blk,
                              kos_cap_t ep, kos_cap_t const* line, kos_task_t task);

// The whole choreography. Returns 0, or a negative failure code: a bad descriptor or a failed
// step prints its own diagnostic; a handover probe refusal is returned unchanged.
//
// `out_ep` receives the retained endpoint under KOS_DRV_EP_RETAIN and must be null under
// HANDOVER. valid() cannot check that pairing, out_ep being a runtime pointer.
int bring_up(Descriptor const& d, struct kos_service_cfg const* cfg, kos_cap_t* out_ep);

}

#endif
