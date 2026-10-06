// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The bring-up choreography of an unprivileged driver service, once for every class and
// every chip: lay out the shared block, make the group, publish or retain the endpoint, claim
// the IRQ lines, spawn the threads with their per-thread grants and cap roles, poll the
// readiness latch strictly between two spawns, unwind on any failure, and finish a console
// handover. Given an instance (docs/design-m10-target.md, section 2) the init holds the block
// and the endpoint and handles the task's failure; on a service list the bring-up makes them
// and kills the task itself.
//
// A class enters only as a thread-entry pointer and as the per-chip block_init. No chip
// header is included here: a descriptor is authored in the per-chip TU, the only one with
// REGDIR on its include path.

#ifndef KICKOS_SYS_DRIVER_SERVICE_H
#define KICKOS_SYS_DRIVER_SERVICE_H

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/cap_index.h> // KOS_CAP_STDOUT
#include <kickos/sys/driver_geometry.h> // KICKOS_DRIVER_ENDPOINTS (generated)
#include <kickos/sys/errno.h>
#include <kickos/sys/service.h>

#include <stdint.h>
#include <stdlib.h>

// A line of an instance: its number at the interrupt controller, and its index among its
// device's lines, which a device routing its events onto one of them is programmed with.
struct kos_driver_line
{
    uint16_t number;
    uint16_t index;
};

// What the init keeps of a packaged driver task across its restarts, filled at each start from
// its record and its table entry and passed in kos_service_cfg::instance.
struct kos_driver_instance
{
    void* block;          // the ring block the init reserved and self-granted at boot, or null
    uint32_t block_size;  // its bytes, 0 for none
    uint32_t block_flags; // its self-grant's kos_mem_flags, KOS_MEM_NOCACHE for an uncached one
    uint32_t core_mask;   // the declared core's bit, 0 for none
    uint8_t ceiling;      // the task's priority ceiling the table carries
    kos_cap_t endpoint;   // the endpoint the init created at boot and keeps with HANDOUT
    kos_cap_t watch;      // this start's badged copy of the init's notification
    kos_task_t task;      // the task the bring-up created, written back; KOS_TASK_NONE before it
    struct kos_driver_line lines[2]; // the table's lines by role, KOS_DRV_LINES_MAX of them
    uint8_t line_count;
    bool console;         // the table names it the console driver, which the init narrows at handover
};

namespace kickos::driver
{

enum
{
    KOS_DRV_LINES_MAX = 2,
    KOS_DRV_THREADS_MAX = 3,
    KOS_DRV_CAPS_MAX = 3
};

// What a per-thread cap entry NAMES. A thread's caps[] index IS its child cap index offset
// from KOS_SPAWN_DELEGATED_CAP0: caps[0] lands at index 1, caps[1] at index 2.
//
// KOS_DRV_RES_NOTIFY is the one notification this bring-up creates. Every claimed line
// signals it, line i on bit i, so a driver's badge space starts at bit line_count and the
// validator refuses a doorbell badge below that.
enum
{
    KOS_DRV_RES_EP = 0,
    KOS_DRV_RES_NOTIFY = 1,
    KOS_DRV_RES_LINE0 = 2,
    KOS_DRV_RES_LINE1 = 3
};

static_assert(sizeof(kos_driver_instance::lines) / sizeof(kos_driver_line) == KOS_DRV_LINES_MAX,
              "an instance carries a line per line a descriptor claims");

struct Cap
{
    uint8_t resource; // KOS_DRV_RES_EP, KOS_DRV_RES_NOTIFY, or KOS_DRV_RES_LINE0 + i
    uint8_t rights;   // a kos_cap_rights subset
    // KOS_DRV_RES_NOTIFY only: the badge this copy carries, as BIT + 1, so 0 is the
    // unbadged object capability. The bring-up mints the copy; a badged one reaches its own
    // bit and no other.
    uint8_t badge;
};

// The index a service-list Line leaves unstated, which no ARG_LINE0_INDEX thread may take.
constexpr uint16_t KOS_DRV_LINE_INDEX_NONE = 0xFFFFu;

// A line's index among its device's lines, as a spawn hands it on.
struct LineIndex
{
    uint16_t value;
};

struct Line
{
    int32_t number;  // the chip vector, claimed on a service list only; REGDIR-private
    uint8_t trigger; // KOS_IRQ_EDGE or KOS_IRQ_LEVEL
    uint16_t index = KOS_DRV_LINE_INDEX_NONE; // among its device's lines, on a service list only
};

// Which pointer the entry receives, and nothing about reach: the block is the group's
// region (Descriptor::block_size), so ARG_NONE buys a thread no isolation from it.
enum kos_drv_arg
{
    KOS_DRV_ARG_NONE = 0,
    KOS_DRV_ARG_BLOCK = 1, // the granted ring block pointer
    KOS_DRV_ARG_WINDOW = 2, // cfg->mmio_base as a VALUE, never dereferenced as memory
    KOS_DRV_ARG_LINE0_INDEX = 3 // line 0's index among its device's lines, read by line_index_of
};

// Shifted past bit 0, which carries the posture under an instance (thread_start).
constexpr uintptr_t line_index_arg(LineIndex index)
{
    return static_cast<uintptr_t>(index.value) << 1u;
}

// What an ARG_LINE0_INDEX thread reads from the arg thread_start returns.
inline uint16_t line_index_of(void const* arg)
{
    return static_cast<uint16_t>(reinterpret_cast<uintptr_t>(arg) >> 1u);
}

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
    // USER_OWNED and a kernel-console write is DROPPED until recv_holders reaches 0, so every
    // diagnostic below drops the caller's WAIT on E before it prints.
    KOS_DRV_EP_HANDOVER = 0,
    // No publish. On a service list root keeps a full-rights cap for the app to narrow per
    // client, so it holds a WAIT-bearing cap forever, recv_holders never reaches 0 and the
    // last-receiver-gone wake never fires: NO failure path in a driver thread under this
    // posture may exit() there, it must panic. Under the init it traps (trap_under_init).
    KOS_DRV_EP_RETAIN = 1
};

// No readiness latch in the block, so no barrier.
constexpr uint16_t KOS_DRV_READY_NONE = 0xFFFFu;

struct Descriptor
{
    char const* tag;         // "[c6uart] ", prefixed to every diagnostic this bring-up prints
    uintptr_t expected_base; // 0 = no guard; no granted window has base 0
    // 0 = no ring block, no arena allocation, no self-grant. The block is the group's shared
    // region and every thread of this driver sees all of it, whatever its arg: a task owns
    // exactly one Domain and a member may bring no grant of its own. Keep here only state the
    // whole driver may touch; a DEV window, which has one holder, is per-thread instead.
    uint32_t block_size;
    // kos_mem_flags passed identically to the bring-up's own self-grant AND to the task
    // grant, so no cacheable mapping of a KOS_MEM_NOCACHE block ever exists. 0 for ordinary
    // memory; a driver whose controller is a bus master over this block sets KOS_MEM_NOCACHE.
    uint32_t block_flags;
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

// The badge a DOORBELL takes on a driver claiming `line_count` lines: the first bit above
// the lines, stored as bit + 1. Leg L13 refuses anything lower, a badge inside the lines'
// range being one a servicer would read as a device interrupt.
constexpr uint8_t doorbell_badge(uint8_t line_count)
{
    return static_cast<uint8_t>(line_count + 1u);
}

// The bit a badge field names, or 0 for the unbadged capability.
constexpr uint8_t badge_bit(uint8_t badge)
{
    if (badge == 0u)
    {
        return 0;
    }
    return static_cast<uint8_t>(badge - 1u);
}

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

constexpr uint8_t notify_holder_count(Descriptor const& d, uint8_t rights)
{
    uint8_t n = 0;
    for (uint8_t i = 0; i < d.thread_count; i++)
    {
        if (holds(d.threads[i], KOS_DRV_RES_NOTIFY, rights))
        {
            n++;
        }
    }
    return n;
}

// Does this driver need a notification at all? A descriptor with no line and no thread
// naming one creates none: the sim console is such a driver.
constexpr bool notify_used(Descriptor const& d)
{
    for (uint8_t i = 0; i < d.thread_count; i++)
    {
        for (uint8_t j = 0; j < d.threads[i].cap_count; j++)
        {
            if (d.threads[i].caps[j].resource == KOS_DRV_RES_NOTIFY)
            {
                return true;
            }
        }
    }
    return false;
}

// Index of the thread that WAITS on the notification, or thread_count when none does.
constexpr uint8_t notify_waiter(Descriptor const& d)
{
    for (uint8_t i = 0; i < d.thread_count; i++)
    {
        if (holds(d.threads[i], KOS_DRV_RES_NOTIFY, KOS_CAP_WAIT))
        {
            return i;
        }
    }
    return d.thread_count;
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

// L2. A cap or an arg naming a line the descriptor does not claim, an ARG_LINE0_INDEX thread
// whose line 0 states no index for a service list, or a cap granting no right at all.
constexpr bool valid_l2(Descriptor const& d)
{
    for (uint8_t i = 0; i < d.thread_count; i++)
    {
        if (d.threads[i].cap_count > KOS_DRV_CAPS_MAX)
        {
            return false;
        }
        if (d.threads[i].arg == KOS_DRV_ARG_LINE0_INDEX
            and (d.line_count == 0u or d.lines[0].index == KOS_DRV_LINE_INDEX_NONE))
        {
            return false;
        }
        for (uint8_t j = 0; j < d.threads[i].cap_count; j++)
        {
            if (d.threads[i].caps[j].resource >= KOS_DRV_RES_LINE0 + d.line_count)
            {
                return false;
            }
            if (d.threads[i].caps[j].rights == 0u)
            {
                return false;
            }
            // A badge on anything but the notification would be silently ignored.
            if (d.threads[i].caps[j].resource != KOS_DRV_RES_NOTIFY
                and d.threads[i].caps[j].badge != 0u)
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
    // Flags on a block that does not exist reach no grant and would be ignored in silence.
    if (d.block_size == 0u and d.block_flags != 0u)
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
            if (res == KOS_DRV_RES_EP or res == KOS_DRV_RES_NOTIFY)
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

// L12, THE LINE-ROLE RULE. A claimed line comes back MASKED and only a notify_wait that
// accepts its bit arms it, so a line whose ack-and-discard cap no thread holds has no
// servicer and every event on it is lost with no diagnostic. Two line caps swapped breaks
// the count on BOTH lines at once, which is why this leg needs no per-chip knowledge of
// which line is transmit.
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

// L13, THE NOTIFICATION RULE, in four parts.
//
// A claimed line signals the notification and nothing else, so a descriptor that claims one
// and names no notification leaves every line masked forever. One WAITER, for the same
// reason L6 admits one endpoint receiver: the object takes exactly one bound thread. That
// waiter must also be the thread holding the lines, because the rearm a wait issues and the
// ack a driver issues are the same controller access and only a line's holder may make it.
// And a badge below line_count would ALIAS a line's own bit, so a doorbell would read as a
// device interrupt and the servicer would touch a device nothing raised.
constexpr bool valid_l13(Descriptor const& d)
{
    uint8_t const waiters = notify_holder_count(d, KOS_CAP_WAIT);
    if (d.line_count != 0u and waiters != 1u)
    {
        return false;
    }
    if (waiters > 1u)
    {
        return false;
    }
    uint8_t const w = notify_waiter(d);
    for (uint8_t l = 0; l < d.line_count; l++)
    {
        if (not holds(d.threads[w], static_cast<uint8_t>(KOS_DRV_RES_LINE0 + l), KOS_CAP_WAIT))
        {
            return false;
        }
    }
    uint32_t seen = 0;
    for (uint8_t i = 0; i < d.thread_count; i++)
    {
        for (uint8_t j = 0; j < d.threads[i].cap_count; j++)
        {
            if (d.threads[i].caps[j].resource != KOS_DRV_RES_NOTIFY)
            {
                continue;
            }
            uint8_t const b = d.threads[i].caps[j].badge;
            if (b == 0u)
            {
                continue; // the unbadged object capability, which the waiter takes
            }
            uint8_t const bit = badge_bit(b);
            if (bit < d.line_count or bit >= 32u)
            {
                return false;
            }
            if ((seen & (1u << bit)) != 0u)
            {
                return false; // two signallers on one bit cannot be told apart
            }
            seen = seen | (1u << bit);
        }
    }
    return true;
}

// The legs are ordered, not just conjoined: L1 bounds the counts L2 walks, and L2 bounds the
// resource ids L5, L12 and L13 use to index lines[] and caps[].
constexpr bool valid(Descriptor const& d)
{
    return valid_l1(d) and valid_l2(d) and valid_l3(d) and valid_l4(d) and valid_l5(d)
           and valid_l6(d) and valid_l7(d) and valid_l8(d) and valid_l9(d) and valid_l10(d)
           and valid_l11(d) and valid_l12(d) and valid_l13(d);
}

// ---------------------------------------------------------------------------------
// NOT a leg of valid(): the cap layout uart_service.h and usb_cdc_service.h SHARE and
// spi_service.h does not. A ring plus a doorbell, one service thread receiving on caps[0]
// and ringing its BADGED notification copy at caps[1], and one IRQ thread bound to that
// notification at its own caps[0] with the lines it services at caps[1..].
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
    // The doorbell: a BADGED copy of the notification, so its raise is one bit and not the
    // bit a line owns. An unbadged copy here would ring bit 0, which is line 0's.
    if (d.threads[svc].caps[1].resource != KOS_DRV_RES_NOTIFY
        or d.threads[svc].caps[1].badge == 0u
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
        // The waiter takes the UNBADGED capability: it binds and waits on the whole object,
        // and a badge would say it signals one bit instead.
        if (d.threads[i].cap_count < 2u
            or d.threads[i].caps[0].resource != KOS_DRV_RES_NOTIFY
            or d.threads[i].caps[0].badge != 0u
            or (d.threads[i].caps[0].rights & KOS_CAP_WAIT) == 0u)
        {
            return false;
        }
        for (uint8_t j = 1; j < d.threads[i].cap_count; j++)
        {
            if (d.threads[i].caps[j].resource < KOS_DRV_RES_LINE0
                or (d.threads[i].caps[j].rights & KOS_CAP_WAIT) == 0u)
            {
                return false;
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------
// What kickos_add_driver declares for a packaged driver and exports in the manifest's
// catalogue, read from the generated <kickos/driver/declared/<name>.h>. A descriptor takes its
// line and thread counts, ring block, posture, barrier point, thread names and priority offsets
// from it, and declared_as checks the descriptor's own statement of every other declared fact.
struct Declared
{
    uint8_t window_count;
    uint8_t line_count;
    uint8_t thread_count;
    int8_t prio_delta[KOS_DRV_THREADS_MAX];
    char const* thread_name[KOS_DRV_THREADS_MAX]; // null takes cfg->name
    uint8_t cap_count[KOS_DRV_THREADS_MAX];
    uint8_t badged[KOS_DRV_THREADS_MAX]; // the badged notification copies each spawn mints
    uint8_t receiver; // the thread that waits on the endpoint
    bool notify;
    uint32_t block_size;
    uint32_t block_flags; // KOS_MEM_NOCACHE for an uncached block
    uint8_t ep_posture; // enum kos_drv_ep
    bool barrier;
    uint8_t barrier_after; // thread_count where there is no barrier
    bool console;
};

constexpr bool declared_as(Descriptor const& d, Declared const& m)
{
    if (window_holder_count(d) != m.window_count)
    {
        return false;
    }
    for (uint8_t i = 0; i < d.thread_count; i++)
    {
        if (d.threads[i].cap_count != m.cap_count[i])
        {
            return false;
        }
        uint8_t badged = 0;
        for (uint8_t c = 0; c < d.threads[i].cap_count; c++)
        {
            if (d.threads[i].caps[c].resource == KOS_DRV_RES_NOTIFY and d.threads[i].caps[c].badge != 0u)
            {
                badged++;
            }
        }
        if (badged != m.badged[i])
        {
            return false;
        }
    }
    if (ep_holder(d) != m.receiver)
    {
        return false;
    }
    if (notify_used(d) != m.notify)
    {
        return false;
    }
    if ((d.ready_offset != KOS_DRV_READY_NONE) != m.barrier)
    {
        return false;
    }
    if (d.block_flags != m.block_flags)
    {
        return false;
    }
    return (d.svc_kind == KOS_SVC_CONSOLE) == m.console;
}

// ---------------------------------------------------------------------------------
// Print `tag` then `msg` and return -1, the bring-up failure code.
int fail(char const* tag, char const* msg);

// Ends the calling thread's task by a synchronous fault. The kernel's fault path cancels the
// group, whichever member traps.
[[noreturn]] void trap();

// Every descriptor thread's entry calls this first, with the arg it was spawned with, and uses
// what it returns as the arg its descriptor declares. Given an instance, bring_up spawns each
// thread with bit 0 of its arg set, every declared arg being null, a ring block, a window base
// or a line_index_arg, none of them odd; this records that posture in the calling thread's own
// space, where trap_under_init reads it, and clears the bit. On a service list no arg carries it.
void* thread_start(void* arg);

// Given an instance, a driver thread whose call failed traps, so its task's death reaches the
// init; it returns on a service list, where the caller then fails as it does there. The posture
// is the one the calling space's threads recorded through thread_start.
void trap_under_init();

constexpr uint32_t KOS_DRV_HANDOVER_PROBE_US = 1000000;

// What the init keeps of a console's endpoint once its handover ends: SIGNAL, TRANSFER, HANDOUT.
constexpr uint32_t KOS_DRV_HANDOVER_KEPT = KOS_CAP_SIGNAL | KOS_CAP_TRANSFER | KOS_CAP_HANDOUT;

// The last two steps of a console handover: drop the caller's own WAIT on E, then probe with a
// zero-length rendezvous on cap 0. Returns 0, or the probe's negative rc.
//
// Given an instance, the caller's capability is NARROWED to SIGNAL, TRANSFER and HANDOUT,
// dropping the WAIT it created the endpoint with, or the one a restart's publish through HANDOUT
// seated: the init keeps the endpoint across the driver's death, so a dead receiver answers its
// writers -KOS_EAGAIN until the next start publishes it again, and -KOS_ECONNREFUSED once the
// init, its restarts spent, closes it. A probe that fails is a failed start, which the init
// slays. On a service list it is CLOSED, which leaves the driver the sole receiver, so its death
// takes recv_holders to 0 and reclaims the console; a probe answered -KOS_ECONNREFUSED kills the
// group, and any other refusal leaves a live service thread holding the console, which nothing
// here recovers.
int console_handover_finish(kos_cap_t ep, char const* tag, kos_task_t task,
                            struct kos_driver_instance const* instance);

constexpr uint32_t KOS_DRV_READY_WAIT_NS = 1000000u; // 1 ms
constexpr uint32_t KOS_DRV_READY_WAIT_MAX = 1000u;   // ~1 s total

// How often, and how far apart, a claim answered -KOS_EAGAIN, a line retiring from the
// instance before, is tried again given an instance.
constexpr uint32_t KOS_DRV_CLAIM_RETRIES = 100u;
constexpr uint64_t KOS_DRV_CLAIM_RETRY_NS = 1000000ull;

// Poll the readiness latch at `off` inside `blk` until it is set or the budget runs out. The
// latch MUST be an Atomic<uint32_t, Order::RELAXED>, so `off` comes from a class-substrate
// constant and never from a descriptor literal.
bool wait_ready(void const* blk, uint16_t off);

// Give back everything a failed bring-up took: the claimed lines, the endpoint, the group.
// CLOSE BEFORE CANCELLING AND BEFORE PRINTING: closing takes the endpoint's last receiver
// holder to 0, which notes the console dead and reclaims it, so the tag the caller prints
// next reaches the wire.
void unwind(kos_cap_t const* line, uint8_t claimed, kos_cap_t ep, kos_cap_t note,
            kos_task_t task);

// Spawn one descriptor thread into `task` with its per-thread grants and cap roles. A
// KOS_DRV_RES_NOTIFY cap with a badge is MINTED from `note` for the spawn and closed after
// it; an unbadged one takes `note` itself. KOS_CAP_NONE where the descriptor names none.
// `line0_index` is what an ARG_LINE0_INDEX thread receives. `core_mask` places the thread, 0
// for its task's default set. `under_init` sets bit 0 of the thread's arg, the posture
// thread_start records.
kos::thread::Handle spawn_one(Thread const& t, struct kos_service_cfg const* cfg, void* blk,
                              kos_cap_t ep, kos_cap_t const* line, LineIndex line0_index,
                              kos_cap_t note, kos_task_t task, uint32_t core_mask, bool under_init);

// The catalogue states what bring_up creates from the build's declaration of it.
static_assert(KICKOS_DRIVER_ENDPOINTS == 1 and KICKOS_DRIVER_NOTIFICATIONS == 1,
              "bring_up creates one endpoint, and one notification for a driver that uses one");

// The whole choreography. Returns 0, or a negative failure code: a bad descriptor or a failed
// step prints its own diagnostic; a handover probe refusal is returned unchanged. A cfg whose
// reserved bytes are not zero is refused.
//
// Given an instance (cfg->instance), the ring block, the endpoint and the lines are the
// instance's and the kind is the descriptor's; every thread runs on the declared core, where the
// lines are claimed at the descriptor's trigger, and the task's priority ceiling and core grant
// are narrowed to the declaration. The task is written back as soon as it exists; a step that
// fails closes what this call made, narrows a console endpoint before it prints, leaves the task
// and the endpoint to the init, and returns its code. `out_ep` is neither read nor written.
//
// On a service list, `out_ep` receives the retained endpoint under KOS_DRV_EP_RETAIN and must
// be null under HANDOVER; valid() cannot check that pairing, out_ep being a runtime pointer.
// Above one kernel core a descriptor with lines pins the CALLING thread to the line core for
// the claims and then resets it to its task's default mask, whatever mask it held before.
int bring_up(Descriptor const& d, struct kos_service_cfg const* cfg, kos_cap_t* out_ep);

}

#endif
