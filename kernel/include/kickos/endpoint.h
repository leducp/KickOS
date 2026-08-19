// SPDX-License-Identifier: CECILL-C
// Copyright (c) 2026 Philippe Leduc

#ifndef KICKOS_ENDPOINT_H
#define KICKOS_ENDPOINT_H

#include <stdint.h>

#include <kickos/config.h> // KICKOS_MAX_ENDPOINTS bounds the served-chain ref below
#include <kickos/list.h>

namespace kickos
{
    struct Thread; // kickos/thread.h

    // A served-endpoint chain entry (Thread::served_head, next_served below): an endpoint
    // pool index biased by one, so the sentinel is 0 and a zeroed TCB or slot is already
    // unlinked with nothing to seat.
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
        // Intrusive link in `server`'s served-endpoint chain, or EP_SERVED_NONE.
        // Non-sentinel exactly while `server` is non-null.
        uint16_t next_served = EP_SERVED_NONE;
        // The conventional single receiver, re-set at every recv. One server per endpoint
        // is documented, not enforced. Also the boost target when a caller parks on
        // send_waiters. MUST be cleared in the endpoint close/teardown arm when the server
        // drops its WAIT cap, else this raw pointer dangles onto a reused TCB.
        // Write it ONLY through endpoint_server_set/endpoint_server_clear (sync.h) once the
        // endpoint is live; the chain above indexes this field, and a bare store leaves that
        // chain stale. endpoint_create is the one bare store, and it seats next_served too.
        Thread* server = nullptr;
    };

    // Unwind `t` out of whichever endpoint park it sits under (WAIT_EP_SEND, WAIT_EP_RECV or
    // WAIT_EP_REPLY), reverting any priority donation that park had pinned, and wake it with
    // `result`. Implemented in kernel/thread/park.cc so a caller only has to decide THAT the
    // park must end: unlinking the right list and reverting the right boost are endpoint
    // internals. Two callers, an expired deadline (-KOS_ETIMEDOUT) and a cancel
    // (-KOS_ECANCELED). Caller holds IrqLock, with `t` already off the timer delta list if it
    // was on one and its wait edge still set.
    void endpoint_wait_abort(Thread* t, intptr_t result);
}

#endif
