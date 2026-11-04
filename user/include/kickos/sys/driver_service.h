// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc
//
// The bring-up of an unprivileged driver service, once for every class and every chip. In a driver
// task it runs in the entry thread, which is descriptor thread 0: lay out the shared block, spawn
// threads 1 to n-1 into the task with their cap roles, the ones past the barrier waiting on the
// readiness latch, keep thread 0's own capabilities, and run thread 0's body. The kernel's create
// has published the console, claimed the lines and seated the window; the init holds the block
// and the endpoint, finishes a console handover and handles the task's end
// (docs/design-m10-target.md, section 2). A driver listed under a task's `drivers` runs as threads
// of that task: its entry thread stages the same layout and the bring-up spawns thread 0 too.
//
// A class enters only as a thread-entry pointer and as the per-chip block_init. No chip
// header is included here: a descriptor is generated from the driver's kickos_add_driver
// declaration and expanded in the per-chip TU, the only one with REGDIR on its include path.

#ifndef KICKOS_SYS_DRIVER_SERVICE_H
#define KICKOS_SYS_DRIVER_SERVICE_H

#include <kickos/kos.h>
#include <kickos/sys.h>
#include <kickos/sys/atomic.h>
#include <kickos/sys/cap_index.h> // KOS_CAP_STDOUT
#include <kickos/sys/driver_geometry.h> // KICKOS_DRIVER_ENDPOINTS (generated)
#include <kickos/sys/errno.h>

#include <stdint.h>
#include <stdlib.h>

// A line of an instance: its number at the interrupt controller, and its index among its
// device's lines, which a device routing its events onto one of them is programmed with.
struct kos_driver_line
{
    uint16_t number;
    uint16_t index;
};

// A packaged driver's instance, filled from its table entry and its ring block by the entry thread
// of its task or of the task it runs in: START's only argument.
struct kos_driver_instance
{
    char const* name;     // the task's name, which a descriptor thread naming none takes
    uintptr_t mmio_base;  // the window's base, 0 for none
    uint32_t mmio_window; // its bytes
    uint8_t priority;     // the task's priority, which each thread offsets
    void* block;          // the ring block the init reserved and self-granted at boot, or null
    uint32_t block_size;  // its bytes, 0 for none
    uint32_t block_flags; // its self-grant's kos_mem_flags, KOS_MEM_NOCACHE for an uncached one
    uint32_t core_mask;   // the declared core's bit, 0 for none
    uint8_t ceiling;      // the task's priority ceiling the table carries
    struct kos_driver_line lines[2]; // the table's lines by role, KOS_DRV_LINES_MAX of them
    uint8_t line_count;
    bool console;         // the table names it the console driver
    uint32_t authority;   // the KOS_AUTH_* bits its descriptor states
    // Inside another task: the capability at each place of the entry thread's layout, from
    // KOS_SPAWN_DELEGATED_CAP0, and thread 0's windows. Null in a driver task.
    kos_cap_t const* hosted;
    struct kos_window const* windows;
    uint8_t window_count;
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
// from KOS_SPAWN_DELEGATED_CAP0: caps[0] lands there, caps[1] one above.
//
// KOS_DRV_RES_NOTIFY is the one notification the init creates for each start. Every declared
// line signals it, line i on bit i, so a driver's badge space starts at bit line_count and the
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

// A line's index among its device's lines, as a spawn hands it on.
struct LineIndex
{
    uint16_t value;
};

struct Line
{
    uint8_t trigger; // KOS_IRQ_EDGE or KOS_IRQ_LEVEL
};

// Which pointer the entry receives, and nothing about reach: the block is the task's
// region (Descriptor::block_size), so ARG_NONE buys a thread no isolation from it.
enum kos_drv_arg
{
    KOS_DRV_ARG_NONE = 0,
    KOS_DRV_ARG_BLOCK = 1, // the granted ring block pointer
    KOS_DRV_ARG_WINDOW = 2, // the window's base as a VALUE, never dereferenced as memory
    KOS_DRV_ARG_LINE0_INDEX = 3 // line 0's index among its device's lines, read by line_index_of
};

constexpr uintptr_t line_index_arg(LineIndex index)
{
    return static_cast<uintptr_t>(index.value);
}

// What an ARG_LINE0_INDEX thread reads from its arg.
inline uint16_t line_index_of(void const* arg)
{
    return static_cast<uint16_t>(reinterpret_cast<uintptr_t>(arg));
}

struct Thread
{
    void (*entry)(void*);
    char const* name; // null takes the instance's name
    int8_t prio_delta;
    uint8_t arg;       // enum kos_drv_arg
    bool window_grant; // the instance's window, which the kernel seats on thread 0 alone
    uint8_t cap_count;
    struct Cap caps[KOS_DRV_CAPS_MAX];
};

enum kos_drv_ep
{
    // The create publishes the console, and the init probes it. From the publish on the console
    // is USER_OWNED and a kernel-console write from the driver's task is DROPPED.
    KOS_DRV_EP_HANDOVER = 0,
    // No publish. The init keeps SIGNAL, TRANSFER and HANDOUT on the endpoint and never
    // receives, so a caller is answered -KOS_EAGAIN while no server receives. A failing driver
    // thread under this posture traps, never exits.
    KOS_DRV_EP_RETAIN = 1
};

// No readiness latch in the block, so no barrier.
constexpr uint16_t KOS_DRV_READY_NONE = 0xFFFFu;

// The readiness latch: 0 until the device half is up, then KOS_DRV_READY_DEVICE, then
// KOS_DRV_READY_SERVICE once the service thread has reached its first receive.
using Latch = Atomic<uint32_t, Order::ACQUIRE | Order::RELEASE>;

constexpr uint32_t KOS_DRV_READY_DEVICE = 1u;
constexpr uint32_t KOS_DRV_READY_SERVICE = 2u;

// What a thread past the barrier starts with: the body it runs once the latch is set, and its
// arg.
struct Launch
{
    void (*entry)(void*);
    void* arg;
    Latch* latch;
};

struct Descriptor
{
    char const* tag;         // "[c6uart] ", prefixed to every diagnostic this bring-up prints
    uintptr_t expected_base; // 0 = no guard; no granted window has base 0
    // 0 = no ring block, no arena allocation, no self-grant. The block is the task's shared
    // region and every thread of this driver sees all of it, whatever its arg: a task owns
    // exactly one Domain and a member may bring no grant of its own. Keep here only state the
    // whole driver may touch; a DEV window, which has one holder, is per-thread instead.
    uint32_t block_size;
    // kos_mem_flags passed identically to the init's own self-grant AND to the task grant, so no
    // cacheable mapping of a KOS_MEM_NOCACHE block ever exists. 0 for ordinary memory; a driver
    // whose controller is a bus master over this block sets KOS_MEM_NOCACHE.
    uint32_t block_flags;
    uint16_t ready_offset;   // byte offset of the readiness latch inside the block
    uint8_t ep_posture;      // enum kos_drv_ep
    uint8_t line_count;
    uint8_t thread_count;
    uint8_t barrier_after; // the first thread that waits on the readiness latch before its body
    struct Line lines[KOS_DRV_LINES_MAX];
    struct Thread threads[KOS_DRV_THREADS_MAX];
    // Lays out the block and fills the class config from the instance. Null iff block_size == 0.
    int (*block_init)(void* blk, struct kos_driver_instance const* in);
    // KOS_DRV_THREADS_MAX records in static storage, which every thread of the task reads and a
    // sibling's stack is not; one per thread past the barrier. Null iff ready_offset is
    // KOS_DRV_READY_NONE.
    Launch* launch;
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

// L2. A cap or an arg naming a line the descriptor does not claim, or a cap granting no right
// at all.
constexpr bool valid_l2(Descriptor const& d)
{
    for (uint8_t i = 0; i < d.thread_count; i++)
    {
        if (d.threads[i].cap_count > KOS_DRV_CAPS_MAX)
        {
            return false;
        }
        if (d.threads[i].arg == KOS_DRV_ARG_LINE0_INDEX and d.line_count == 0u)
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

// L3. A DEV window has exactly one holder, thread 0, the entry thread the kernel seats it on.
//
// Second arm: a spawn hands the window's base to an ARG_WINDOW thread whether or not that thread
// was granted the window, so one without the grant faults on its first register touch.
constexpr bool valid_l3(Descriptor const& d)
{
    if (window_holder_count(d) > 1u)
    {
        return false;
    }
    for (uint8_t i = 1; i < d.thread_count; i++)
    {
        if (d.threads[i].window_grant)
        {
            return false;
        }
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

// L7. A multi-thread console handover needs a readiness latch, which L8 has the endpoint's
// receiver wait on. A one-thread driver has no device half to wait for, its only thread being
// that receiver, so L7 and L8 would be jointly unsatisfiable: there the handover probe is the
// witness instead.
constexpr bool valid_l7(Descriptor const& d)
{
    return d.ep_posture != KOS_DRV_EP_HANDOVER or d.thread_count == 1u
           or d.ready_offset != KOS_DRV_READY_NONE;
}

// L8, THE BARRIER RULE, and the latch's own arithmetic.
//
// Threads from barrier_after on wait on the latch before their body, threads before it set it:
// barrier_after == 0 would leave nobody to set it, and barrier_after >= thread_count nobody to
// wait. An unaligned 32-bit load is tolerated on ARMv7-M and FAULTS on RX and Xtensa.
//
// HANDOVER-ONLY arm: the receiver must wait, because it takes the handover's probe, and a probe
// taken before the device half is ready would prove a console that cannot write yet, with
// clients sending to it. The ep_holder arm is the SOLE rejecter of a receiver that does not wait.
constexpr bool valid_l8(Descriptor const& d)
{
    if ((d.ready_offset == KOS_DRV_READY_NONE) != (d.launch == nullptr))
    {
        return false;
    }
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
    if (d.barrier_after < 1u or d.barrier_after >= d.thread_count)
    {
        return false;
    }
    if (d.ep_posture != KOS_DRV_EP_HANDOVER)
    {
        return true;
    }
    return ep_holder(d) >= d.barrier_after;
}

// L10. A half-authored descriptor.
// A null entry is NOT checked here: an entry that is forward-declared at the descriptor,
// which the sim console does deliberately, has no constant address under
// -fsanitize=undefined, so testing it makes valid() non-constant. bring_up checks it.
constexpr bool valid_l10(Descriptor const& d)
{
    return d.tag != nullptr;
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

// ---------------------------------------------------------------------------------
// The entry thread's capability layout. A token is what a capability of the layout names: the
// endpoint, the unbadged notification, the badged doorbell copy of it, or line k.
enum
{
    KOS_DRV_TOK_EP = 0,
    KOS_DRV_TOK_NOTIFY = 1,
    KOS_DRV_TOK_DOORBELL = 2,
    KOS_DRV_TOK_LINE0 = 3,
    KOS_DRV_TOK_COUNT = KOS_DRV_TOK_LINE0 + KOS_DRV_LINES_MAX,
    KOS_DRV_TOK_NONE = 0xFF
};

constexpr uint8_t token_of(Cap const& c)
{
    if (c.resource == KOS_DRV_RES_NOTIFY and c.badge != 0u)
    {
        return KOS_DRV_TOK_DOORBELL;
    }
    if (c.resource >= KOS_DRV_RES_LINE0)
    {
        return static_cast<uint8_t>(KOS_DRV_TOK_LINE0 + (c.resource - KOS_DRV_RES_LINE0));
    }
    return c.resource;
}

// Whether the entry thread's table holds `tok`: the endpoint and every line always, the
// notification and the doorbell where a thread names one.
constexpr bool token_held(Descriptor const& d, uint8_t tok)
{
    if (tok == KOS_DRV_TOK_EP)
    {
        return true;
    }
    if (tok >= KOS_DRV_TOK_LINE0)
    {
        return tok < KOS_DRV_TOK_LINE0 + d.line_count;
    }
    for (uint8_t i = 0; i < d.thread_count; i++)
    {
        for (uint8_t j = 0; j < d.threads[i].cap_count; j++)
        {
            if (token_of(d.threads[i].caps[j]) == tok)
            {
                return true;
            }
        }
    }
    return false;
}

// Where the entry thread's table holds each token, as an offset from KOS_SPAWN_DELEGATED_CAP0:
// thread 0's capabilities first, in their order, so that its body reads them where every thread
// reads its own, then the other tokens it holds in token order. KOS_DRV_TOK_NONE for a token it
// does not hold. kickos_add_driver states the same layout to the init (cmake/driver_metadata.cmake).
struct Layout
{
    uint8_t at[KOS_DRV_TOK_COUNT];
    uint8_t count;
};

constexpr Layout layout_of(Descriptor const& d)
{
    Layout l = {};
    for (uint8_t t = 0; t < KOS_DRV_TOK_COUNT; t++)
    {
        l.at[t] = KOS_DRV_TOK_NONE;
    }
    if (d.thread_count != 0u)
    {
        for (uint8_t j = 0; j < d.threads[0].cap_count; j++)
        {
            l.at[token_of(d.threads[0].caps[j])] = l.count;
            l.count++;
        }
    }
    for (uint8_t t = 0; t < KOS_DRV_TOK_COUNT; t++)
    {
        if (l.at[t] == KOS_DRV_TOK_NONE and token_held(d, t))
        {
            l.at[t] = l.count;
            l.count++;
        }
    }
    return l;
}

// L14, THE LAYOUT RULE. Thread 0 names no token twice, so each of its capabilities has a place
// of its own, and every doorbell carries the one badge the init mints, the first bit above the
// lines.
constexpr bool valid_l14(Descriptor const& d)
{
    if (d.thread_count == 0u)
    {
        return true;
    }
    for (uint8_t j = 0; j < d.threads[0].cap_count; j++)
    {
        for (uint8_t k = 0; k < j; k++)
        {
            if (token_of(d.threads[0].caps[j]) == token_of(d.threads[0].caps[k]))
            {
                return false;
            }
        }
    }
    for (uint8_t i = 0; i < d.thread_count; i++)
    {
        for (uint8_t j = 0; j < d.threads[i].cap_count; j++)
        {
            if (token_of(d.threads[i].caps[j]) == KOS_DRV_TOK_DOORBELL
                and d.threads[i].caps[j].badge != doorbell_badge(d.line_count))
            {
                return false;
            }
        }
    }
    return true;
}

// The legs are ordered, not just conjoined: L1 bounds the counts L2 walks, and L2 bounds the
// resource ids L5, L12, L13 and L14 use to index lines[] and caps[].
constexpr bool valid(Descriptor const& d)
{
    return valid_l1(d) and valid_l2(d) and valid_l3(d) and valid_l4(d) and valid_l5(d)
           and valid_l6(d) and valid_l7(d) and valid_l8(d) and valid_l10(d) and valid_l12(d)
           and valid_l13(d) and valid_l14(d);
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

// A thread that waits on one of its roles, the endpoint, the notification or a line: a service or
// IRQ worker, whose end leaves that role unserved.
constexpr bool essential(Thread const& t)
{
    for (uint8_t i = 0; i < t.cap_count; i++)
    {
        if ((t.caps[i].rights & KOS_CAP_WAIT) != 0u)
        {
            return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------------
// Print `tag` then `msg` and return -1, the bring-up failure code.
int fail(char const* tag, char const* msg);

// Print `s` on the kernel console alone, never waiting on a console endpoint. From the task
// serving the published console it is dropped until that task has ended.
void report(char const* s);

// Ends the calling thread's task by a synchronous fault, which a driver thread whose call failed
// takes so that its task's death reaches the init. The kernel's fault path cancels the group,
// whichever member traps.
[[noreturn]] void trap();

constexpr uint32_t KOS_DRV_HANDOVER_PROBE_US = 1000000;

// What the init keeps of a console's endpoint once its handover ends: SIGNAL, TRANSFER, HANDOUT.
constexpr uint32_t KOS_DRV_HANDOVER_KEPT = KOS_CAP_SIGNAL | KOS_CAP_TRANSFER | KOS_CAP_HANDOUT;

// The readiness wait's whole budget, about a second.
constexpr uint32_t KOS_DRV_READY_WAIT_NS = 1000000u; // 1 ms
constexpr uint32_t KOS_DRV_READY_WAIT_MAX = 1000u;

// The latch at `off` inside `blk`, which MUST come from a class-substrate constant and never
// from a descriptor literal.
inline Latch* ready_latch(void* blk, uint16_t off)
{
    return reinterpret_cast<Latch*>(static_cast<unsigned char*>(blk) + off);
}

// The producer's side: a release store of `state`, so everything written before it is seen by a
// waiter that reads it, then a wake of every waiter parked on it. Returns the waiters woken, or
// the wake's refusal.
inline int ready_set(Latch* latch, uint32_t state)
{
    latch->store(state);
    return kos_word_wake(reinterpret_cast<uint32_t*>(latch), 0xFFFFFFFFu, 0u);
}

// The device half is up: the IRQ thread's publish.
inline int ready_publish(Latch* latch)
{
    return ready_set(latch, KOS_DRV_READY_DEVICE);
}

// The service thread is at its first receive. It MUST already have read the device half up,
// or the device's later publish takes the latch back below this state.
inline int service_publish(Latch* latch)
{
    return ready_set(latch, KOS_DRV_READY_SERVICE);
}

// The waiter's side: an acquire load, and while it reads below `state` a park on the word for
// what is left of the budget, read again after every return. True once it reads `state` or
// above, false past the budget.
inline bool ready_wait_for(Latch* latch, uint32_t state)
{
    uint64_t const budget_ns = uint64_t{KOS_DRV_READY_WAIT_MAX} * KOS_DRV_READY_WAIT_NS;
    uint64_t const deadline = kos_clock_now() + budget_ns;
    while (true)
    {
        uint32_t const seen = latch->load();
        if (seen >= state)
        {
            return true;
        }
        uint64_t const now = kos_clock_now();
        if (now >= deadline)
        {
            return false;
        }
        uint64_t const left_us = (deadline - now + 999u) / 1000u;
        (void)kos_word_wait(reinterpret_cast<uint32_t*>(latch), seen, static_cast<uint32_t>(left_us));
    }
}

// The device half is up, as a thread past the barrier waits for it.
inline bool ready_wait(Latch* latch)
{
    return ready_wait_for(latch, KOS_DRV_READY_DEVICE);
}

// The catalogue states what the init creates for a start from the build's declaration of it.
static_assert(KICKOS_DRIVER_ENDPOINTS == 1 and KICKOS_DRIVER_NOTIFICATIONS == 1,
              "a start takes one endpoint, and one notification for a driver that uses one");

// The bring-up, run by the driver task's entry thread, which is thread 0 and holds the layout
// layout_of(d) states. Returns a negative failure code having spawned nothing, after printing its
// own diagnostic; once a thread is spawned it never returns: a failed step traps, ending the
// task, and thread 0's body returning exits the entry thread, which ends the task too.
//
// Threads 1 to n-1 are spawned into the task on the declared core, each delegated its roles from
// the entry thread's table; a thread from barrier_after on waits on the readiness latch before
// its body and traps past the budget. Thread 0 then keeps its own capabilities, narrowed to its
// rights, closes the rest, raises itself to its descriptor priority and runs its body.
//
// With in->hosted, threads 0 to n-1 are all spawned, from in->hosted, thread 0 with the windows
// and the authority, each essential() thread KOS_THREAD_ESSENTIAL, so that its end by any cause
// fails the task it runs in, and it returns 0 once the latch reads KOS_DRV_READY_SERVICE, or at once for a
// driver with no latch; a failed step past the first spawn still traps, ending the task the
// driver runs in. The caller then probes the endpoint before anything calls it.
int bring_up(Descriptor const& d, struct kos_driver_instance* in);

}

#endif
