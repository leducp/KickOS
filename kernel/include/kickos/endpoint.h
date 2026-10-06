// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ENDPOINT_H
#define KICKOS_ENDPOINT_H

#include <stdint.h>

#include <kickos/arch/arch.h> // KICKOS_AMP_NODE
#include <kickos/config.h> // KICKOS_MAX_ENDPOINTS bounds the served-chain ref below
#include <kickos/list.h>
#include <kickos/sys/errno.h> // KOS_EAGAIN, KOS_ECONNREFUSED

namespace kickos
{
    struct Thread; // kickos/thread.h

#if KICKOS_AMP_NODE
    namespace amp
    {
        // kickos/ampwindow.h
        struct ReplyTag;
    }
#endif

    // A served-endpoint chain entry (Thread::served_head, next_served below): an endpoint
    // pool index biased by one, so the sentinel is 0 and a zeroed TCB or slot is already
    // unlinked with nothing to seat.
    //
    // THE BIAS IS WHAT MAKES SlotPool::at THE WHOLE WALK CONDITION: the sentinel decodes to -1,
    // which at() answers nullptr for, so a served-chain walk carries no sentinel test of its
    // own. One testing both pays a frame on a chain that reaches kpanic, and that descent is
    // measured (tests/static/check_trap_redzone.sh).
    constexpr uint16_t EP_SERVED_NONE = 0;
    static_assert(KICKOS_MAX_ENDPOINTS < UINT16_MAX,
                  "an endpoint's served-chain ref would collide with EP_SERVED_NONE");

    constexpr uint16_t ep_served_ref(int index)
    {
        return static_cast<uint16_t>(index + 1);
    }
    constexpr int ep_served_index(uint16_t ref)
    {
        return static_cast<int>(ref) - 1;
    }

    static_assert(ep_served_index(EP_SERVED_NONE) < 0,
                  "EP_SERVED_NONE must decode outside [0, KICKOS_MAX_ENDPOINTS): the "
                  "served-chain walks in kernel/sync/sync.cc end on SlotPool::at's refusal "
                  "and carry no sentinel test of their own");

    // A cap-named synchronous rendezvous point. There is NO kernel payload storage: the
    // parked side's own user buffer is the storage, stable because that side is BLOCKED,
    // and the ARRIVING thread does the bounded copy under IrqLock.
    // INVARIANT: the endpoint names nothing by address. No buffer address is stored here
    // or handed out as a badge; the transient user-buffer pointer lives in the parked
    // thread's TCB ipc descriptor for the rendezvous only.
    // INVARIANT: the two waitqs are never simultaneously non-empty, because an arrival
    // always drains the opposite queue before parking on its own.
    struct Endpoint
    {
        List send_waiters;    // parked senders (buffer descriptor in their TCB)
        List recv_waiters;    // parked receivers (buffer + badge-out descriptor in their TCB)
        // Live caps carrying CAP_WAIT naming this endpoint, and the single home for that
        // state. NOT the pool refcount, which counts ALL caps: this one gates the send-side
        // dead-endpoint check and fires EPIPE at 0. Shares endpoint_refs' uint8_t ceiling
        // and refusal, because obj_ref_inc tests both before moving either.
        uint8_t recv_holders = 0;
        // Live caps carrying CAP_HANDOUT: holders that may seat a receiver without being one.
        // Rides the padding byte before next_served.
        uint8_t handout_holders = 0;
        // Intrusive link in `server`'s served-endpoint chain, or EP_SERVED_NONE.
        // Non-sentinel exactly while `server` is non-null.
        uint16_t next_served = EP_SERVED_NONE;
        // Set when recv_holders falls to 0 and cleared when a receiver next waits here: a
        // receiver a handout seats counts as none until it first receives.
        uint8_t vacated = 0;
        // EP_CONSOLE_*: whether this is the published console, and whether its task lives.
        uint8_t console = 0;
#if KICKOS_AMP_NODE
        // The node whose kernel holds the receiver, PLUS ONE: 0 is local, so a zeroed slot
        // is already local and nothing has to be seated. THE WHOLE ANSWER to "is this
        // endpoint far": no flag sits beside it to disagree with.
        uint8_t far_node = 0;
        uint8_t far_port = 0;
#endif
        // The conventional single receiver, re-set at every recv. One server per endpoint
        // is documented, not enforced. Also the boost target when a caller parks on
        // send_waiters. MUST be cleared in the endpoint close/teardown arm when the server
        // drops its WAIT cap, else this raw pointer dangles onto a reused TCB.
        // Write it ONLY through endpoint_server_set/endpoint_server_clear (sync.h) once the
        // endpoint is live; the chain above indexes this field, and a bare store leaves that
        // chain stale. The slot claim in kernel/syscall/syscall_ipc.cc is the one bare store,
        // and it seats next_served too.
        Thread* server = nullptr;
    };

#if KICKOS_AMP_NODE
    inline bool endpoint_is_far(Endpoint const* e)
    {
        return e != nullptr and e->far_node != 0;
    }

    // The far node UNBIASED. 0 for a local endpoint, which names node 0: ask
    // endpoint_is_far first.
    inline uint32_t endpoint_far_node(Endpoint const* e)
    {
        if (not endpoint_is_far(e))
        {
            return 0;
        }
        return static_cast<uint32_t>(e->far_node) - 1u;
    }

    // Hand one far CALL to a thread parked on the endpoint its port is bound to. True where a
    // receiver took it: the call slot is then a record the receiver holds, and it lands the
    // payload out of that slot in its own receive (endpoint_reply_recv), never in the masked
    // service that runs this.
    bool endpoint_far_call_deliver(uint32_t from, uint32_t port, amp::ReplyTag const& tag,
                                   uint32_t len, uint32_t slot);

#if defined(KICKOS_ENABLE_SELFTEST)
    // Make the next far delivery fail when writing back its new reply capability.
    // The syscall validates this pointer before parking; this hook tests a failure
    // at delivery time. Payload faults use real frame unmaps instead.
    void endpoint_far_blind_arm(void);
    bool endpoint_far_blind_take(void);
#else
    inline bool endpoint_far_blind_take(void)
    {
        return false;
    }
#endif

    // Hand one far reply to whatever local thread `tag` names, and answer whether one took it.
    // `tag` is another node's writing: nothing in it may be spent before this validates it.
    // `from` is the RING the reply arrived on, which the validation tests the caller's own
    // far node against. True hands the caller `hold`, the reply's slot, which it lands and
    // releases in its own call (endpoint_call); nothing is copied here. Caller holds no lock;
    // the doorbell's mask is the exclusion.
    bool endpoint_far_reply_deliver(uint32_t from, amp::ReplyTag const& tag, uint32_t hold,
                                    uint32_t len);

    // Create a LOCAL endpoint, bind `port` of this node to it, and install a capability for it
    // in `c`. Caller holds IrqLock.
    int amp_port_bind_local(Thread* c, uint32_t port, uint32_t* out_cap);

    // Walk CONFIG_KICKOS_AMP_PORTS in order and seat this node's derived capabilities into
    // `root`: a local endpoint per entry naming this node, a far endpoint per entry naming
    // another. Kernel init, before root's first instruction and before any other dynamic
    // install into its run. Panics rather than booting a node that could not be seated.
    void amp_ports_seat(Thread* root);

#if defined(KICKOS_ENABLE_SELFTEST)
    // Scaffolding: the route a far side would have been handed for the one thread parked on
    // a far reply, so a forge can play a HOSTILE one at it. False where no thread is parked
    // on one.
    bool endpoint_far_reply_route(amp::ReplyTag* out_tag, uint32_t* out_node);
#endif
#else
    inline bool endpoint_is_far(Endpoint const*)
    {
        return false;
    }

    inline uint32_t endpoint_far_node(Endpoint const*)
    {
        return 0;
    }

#endif

    // Whether a sender or caller finds a receiver: one holds WAIT, and one has waited here
    // since the last receiver left.
    inline bool endpoint_receiving(Endpoint const* e)
    {
        return e->recv_holders != 0 and e->vacated == 0;
    }

    // Endpoint::console: SERVED from the publish to the end of the task it names, ENDED after.
    constexpr uint8_t EP_CONSOLE_NONE = 0;
    constexpr uint8_t EP_CONSOLE_SERVED = 1;
    constexpr uint8_t EP_CONSOLE_ENDED = 2;

    // Whether a send is taken, parked where no receiver waits: a receiving endpoint, and the
    // published console while its task lives, receiver or not. always_inline: -Os otherwise
    // emits it out of line on every send and call.
    __attribute__((always_inline)) inline bool endpoint_takes_sends(Endpoint const* e)
    {
        if (e->console != EP_CONSOLE_NONE)
        {
            return e->console == EP_CONSOLE_SERVED;
        }
        return endpoint_receiving(e);
    }

    // What a caller finding no receiver is answered, a vacated endpoint included:
    // -KOS_EAGAIN while a WAIT holder beyond the `leaving` ones already on their way out, or a
    // HANDOUT holder, remains, since a receiver may come; -KOS_ECONNREFUSED once neither does.
    inline int32_t endpoint_unserved(Endpoint const* e, unsigned leaving)
    {
        if (e->recv_holders > leaving or e->handout_holders > 0)
        {
            return -KOS_EAGAIN;
        }
        return -KOS_ECONNREFUSED;
    }

#if KICKOS_AMP_NODE
    constexpr unsigned EP_NARROW_BYTES = 8; // the two holder counts, next_served, vacated, console, far_node, far_port
#else
    constexpr unsigned EP_NARROW_BYTES = 6; // the two holder counts, next_served, vacated, console
#endif
    // The span the narrow fields take ahead of `server` on every posture: one pointer on a
    // 64-bit build, two words on a 32-bit one.
    constexpr unsigned EP_NARROW_SPAN = 8;

    // ONE guard over BOTH arms: a field added under the AMP guard costs its size in .bss on
    // every AMP board, times the endpoint pool, exactly as one added outside it does. The
    // narrow fields ride the padding ahead of `server`, so they are free wherever that padding
    // absorbs them and cost one alignment step where it does not. Widen this deliberately or
    // not at all.
    static_assert(sizeof(Endpoint)
                      == 2 * sizeof(List)
                             + ((EP_NARROW_BYTES + alignof(Thread*) - 1) / alignof(Thread*))
                                   * alignof(Thread*)
                             + sizeof(Thread*),
                  "Endpoint grew past its two lists, the narrow fields packed into the padding "
                  "ahead of `server`, and `server` itself");
    static_assert(sizeof(Endpoint) == 2 * sizeof(List) + EP_NARROW_SPAN + sizeof(Thread*),
                  "Endpoint grew past EP_NARROW_SPAN bytes of narrow fields ahead of `server`");

    // Unwind `t` out of whichever endpoint park it sits under (WAIT_EP_SEND, WAIT_EP_RECV or
    // WAIT_EP_REPLY), reverting any priority donation that park had pinned, and wake it with
    // `result`. Implemented in kernel/thread/park.cc so a caller only has to decide THAT the
    // park must end: unlinking the right list and reverting the right boost are endpoint
    // internals. `result` is -KOS_ETIMEDOUT for an expired deadline or -KOS_ECANCELED for a
    // cancel. Caller holds IrqLock, with `t` already off the timer delta list if it was on
    // one and its wait edge still set.
    void endpoint_wait_abort(Thread* t, intptr_t result);
}

#endif
